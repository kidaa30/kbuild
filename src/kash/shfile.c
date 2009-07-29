/* $Id: shfile.c 2312 2009-03-02 01:14:43Z bird $ */
/** @file
 *
 * File management.
 *
 * Copyright (c) 2007-2009  knut st. osmundsen <bird-kBuild-spamix@anduin.net>
 *
 *
 * This file is part of kBuild.
 *
 * kBuild is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * kBuild is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with kBuild; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#include "shfile.h"
#include "shinstance.h" /* TRACE2 */
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#if K_OS == K_OS_WINDOWS
# include <limits.h>
# ifndef PIPE_BUF
#  define PIPE_BUF 512
# endif
# include <Windows.h>
#else
# include <unistd.h>
# include <fcntl.h>
# include <dirent.h>
#endif


/*******************************************************************************
*   Defined Constants And Macros                                               *
*******************************************************************************/
/** @def SHFILE_IN_USE
 * Whether the file descriptor table stuff is actually in use or not.
 */
#if K_OS == K_OS_WINDOWS \
 || !defined(SH_FORKED_MODE)
# define SHFILE_IN_USE
#endif
/** The max file table size. */
#define SHFILE_MAX          1024
/** The file table growth rate. */
#define SHFILE_GROW         64
/** The min native unix file descriptor. */
#define SHFILE_UNIX_MIN_FD  32
/** The path buffer size we use. */
#define SHFILE_MAX_PATH     4096

/** Set errno and return. Doing a trace in debug build. */
#define RETURN_ERROR(rc, err, msg)  \
    do { \
        TRACE2((NULL, "%s: " ## msg ## " - returning %d / %d\n", __FUNCTION__, (rc), (err))); \
        errno = (err); \
        return (rc); \
    } while (0)

#if K_OS == K_OS_WINDOWS
 /* See msdos.h for description. */
# define FOPEN              0x01
# define FEOFLAG            0x02
# define FCRLF              0x04
# define FPIPE              0x08
# define FNOINHERIT         0x10
# define FAPPEND            0x20
# define FDEV               0x40
# define FTEXT              0x80

# define MY_ObjectBasicInformation 0
typedef LONG (NTAPI * PFN_NtQueryObject)(HANDLE, int, void *, size_t, size_t *);

typedef struct MY_OBJECT_BASIC_INFORMATION
{
    ULONG           Attributes;
	ACCESS_MASK     GrantedAccess;
	ULONG           HandleCount;
	ULONG           PointerCount;
	ULONG           PagedPoolUsage;
	ULONG           NonPagedPoolUsage;
	ULONG           Reserved[3];
	ULONG           NameInformationLength;
	ULONG           TypeInformationLength;
	ULONG           SecurityDescriptorLength;
	LARGE_INTEGER   CreateTime;
} MY_OBJECT_BASIC_INFORMATION;

#endif /* K_OS_WINDOWS */


#ifdef SHFILE_IN_USE

/**
 * Close the specified native handle.
 *
 * @param   native      The native file handle.
 * @param   flags       The flags in case they might come in handy later.
 */
static void shfile_native_close(intptr_t native, unsigned flags)
{
#if K_OS == K_OS_WINDOWS
    BOOL fRc = CloseHandle((HANDLE)native);
    assert(fRc); (void)fRc;
#else
    int s = errno;
    close(native);
    errno = s;
#endif
    (void)flags;
}

/**
 * Inserts the file into the descriptor table.
 *
 * If we're out of memory and cannot extend the table, we'll close the
 * file, set errno to EMFILE and return -1.
 *
 * @returns The file descriptor number. -1 and errno on failure.
 * @param   pfdtab      The file descriptor table.
 * @param   native      The native file handle.
 * @param   oflags      The flags the it was opened/created with.
 * @param   shflags     The shell file flags.
 * @param   fdMin       The minimum file descriptor number, pass -1 if any is ok.
 * @param   who         Who we're doing this for (for logging purposes).
 */
static int shfile_insert(shfdtab *pfdtab, intptr_t native, unsigned oflags, unsigned shflags, int fdMin, const char *who)
{
    shmtxtmp tmp;
    int fd;
    int i;

    /*
     * Fend of bad stuff.
     */
    if (fdMin >= SHFILE_MAX)
    {
        errno = EMFILE;
        return -1;
    }

    shmtx_enter(&pfdtab->mtx, &tmp);

    /*
     * Search for a fitting unused location.
     */
    fd = -1;
    for (i = 0; (unsigned)i < pfdtab->size; i++)
        if (    i >= fdMin
            &&  pfdtab->tab[i].fd == -1)
        {
            fd = i;
            break;
        }
    if (fd == -1)
    {
        /*
         * Grow the descriptor table.
         */
        shfile     *new_tab;
        int         new_size = pfdtab->size + SHFILE_GROW;
        while (new_size < fdMin)
            new_size += SHFILE_GROW;
        new_tab = sh_realloc(shthread_get_shell(), pfdtab->tab, new_size * sizeof(shfile));
        if (new_tab)
        {
            for (i = pfdtab->size; i < new_size; i++)
            {
                new_tab[i].fd = -1;
                new_tab[i].oflags = 0;
                new_tab[i].shflags = 0;
                new_tab[i].native = -1;
            }

            fd = pfdtab->size;
            if (fd < fdMin)
                fd = fdMin;

            pfdtab->tab = new_tab;
            pfdtab->size = new_size;
        }
    }

    /*
     * Fill in the entry if we've found one.
     */
    if (fd != -1)
    {
        pfdtab->tab[fd].fd = fd;
        pfdtab->tab[fd].oflags = oflags;
        pfdtab->tab[fd].shflags = shflags;
        pfdtab->tab[fd].native = native;
    }
    else
        shfile_native_close(native, oflags);

    shmtx_leave(&pfdtab->mtx, &tmp);

    if (fd == -1)
        errno = EMFILE;
    (void)who;
    return fd;
}

/**
 * Gets a file descriptor and lock the file descriptor table.
 *
 * @returns Pointer to the file and table ownership on success,
 *          NULL and errno set to EBADF on failure.
 * @param   pfdtab      The file descriptor table.
 * @param   fd          The file descriptor number.
 * @param   ptmp        See shmtx_enter.
 */
static shfile *shfile_get(shfdtab *pfdtab, int fd, shmtxtmp *ptmp)
{
    shfile *file = NULL;
    if (    fd >= 0
        &&  (unsigned)fd < pfdtab->size)
    {
        shmtx_enter(&pfdtab->mtx, ptmp);
        if ((unsigned)fd < pfdtab->size
            &&  pfdtab->tab[fd].fd != -1)
            file = &pfdtab->tab[fd];
        else
            shmtx_leave(&pfdtab->mtx, ptmp);
    }
    if (!file)
        errno = EBADF;
    return file;
}

/**
 * Puts back a file descriptor and releases the table ownership.
 *
 * @param   pfdtab      The file descriptor table.
 * @param   file        The file.
 * @param   ptmp        See shmtx_leave.
 */
static void shfile_put(shfdtab *pfdtab, shfile *file, shmtxtmp *ptmp)
{
    shmtx_leave(&pfdtab->mtx, ptmp);
    assert(file);
    (void)file;
}

/**
 * Constructs a path from the current directory and the passed in path.
 *
 * @returns 0 on success, -1 and errno set to ENAMETOOLONG or EINVAL on failure.
 *
 * @param   pfdtab      The file descriptor table
 * @param   path        The path the caller supplied.
 * @param   buf         Where to put the path. This is assumed to be SHFILE_MAX_PATH
 *                      chars in size.
 */
int shfile_make_path(shfdtab *pfdtab, const char *path, char *buf)
{
    size_t path_len = strlen(path);
    if (path_len == 0)
    {
        errno = EINVAL;
        return -1;
    }
    if (path_len >= SHFILE_MAX_PATH)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (    *path == '/'
#if K_OS == K_OS_WINDOWS || K_OS == K_OS_OS2
        ||  *path == '\\'
        ||  (   *path
             && path[1] == ':'
             && (   (*path >= 'A' && *path <= 'Z')
                 || (*path >= 'a' && *path <= 'z')))
#endif
        )
    {
        memcpy(buf, path, path_len + 1);
    }
    else
    {
        size_t      cwd_len;
        shmtxtmp    tmp;

        shmtx_enter(&pfdtab->mtx, &tmp);

        cwd_len = strlen(pfdtab->cwd);
        memcpy(buf, pfdtab->cwd, cwd_len);

        shmtx_leave(&pfdtab->mtx, &tmp);

        if (cwd_len + path_len + 1 >= SHFILE_MAX_PATH)
        {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (    !cwd_len
            ||  buf[cwd_len - 1] != '/')
            buf[cwd_len++] = '/';
        memcpy(buf + cwd_len, path, path_len + 1);
    }

#if K_OS == K_OS_WINDOWS || K_OS == K_OS_OS2
    if (!strcmp(buf, "/dev/null"))
        strcpy(buf, "NUL");
#endif
    return 0;
}

# if K_OS == K_OS_WINDOWS

/**
 * Converts a DOS(/Windows) error code to errno,
 * assigning it to errno.
 *
 * @returns -1
 * @param   err     The DOS error.
 */
static int shfile_dos2errno(int err)
{
    switch (err)
    {
        case ERROR_BAD_ENVIRONMENT:         errno = E2BIG;      break;
        case ERROR_ACCESS_DENIED:           errno = EACCES;     break;
        case ERROR_CURRENT_DIRECTORY:       errno = EACCES;     break;
        case ERROR_LOCK_VIOLATION:          errno = EACCES;     break;
        case ERROR_NETWORK_ACCESS_DENIED:   errno = EACCES;     break;
        case ERROR_CANNOT_MAKE:             errno = EACCES;     break;
        case ERROR_FAIL_I24:                errno = EACCES;     break;
        case ERROR_DRIVE_LOCKED:            errno = EACCES;     break;
        case ERROR_SEEK_ON_DEVICE:          errno = EACCES;     break;
        case ERROR_NOT_LOCKED:              errno = EACCES;     break;
        case ERROR_LOCK_FAILED:             errno = EACCES;     break;
        case ERROR_NO_PROC_SLOTS:           errno = EAGAIN;     break;
        case ERROR_MAX_THRDS_REACHED:       errno = EAGAIN;     break;
        case ERROR_NESTING_NOT_ALLOWED:     errno = EAGAIN;     break;
        case ERROR_INVALID_HANDLE:          errno = EBADF;      break;
        case ERROR_INVALID_TARGET_HANDLE:   errno = EBADF;      break;
        case ERROR_DIRECT_ACCESS_HANDLE:    errno = EBADF;      break;
        case ERROR_WAIT_NO_CHILDREN:        errno = ECHILD;     break;
        case ERROR_CHILD_NOT_COMPLETE:      errno = ECHILD;     break;
        case ERROR_FILE_EXISTS:             errno = EEXIST;     break;
        case ERROR_ALREADY_EXISTS:          errno = EEXIST;     break;
        case ERROR_INVALID_FUNCTION:        errno = EINVAL;     break;
        case ERROR_INVALID_ACCESS:          errno = EINVAL;     break;
        case ERROR_INVALID_DATA:            errno = EINVAL;     break;
        case ERROR_INVALID_PARAMETER:       errno = EINVAL;     break;
        case ERROR_NEGATIVE_SEEK:           errno = EINVAL;     break;
        case ERROR_TOO_MANY_OPEN_FILES:     errno = EMFILE;     break;
        case ERROR_FILE_NOT_FOUND:          errno = ENOENT;     break;
        case ERROR_PATH_NOT_FOUND:          errno = ENOENT;     break;
        case ERROR_INVALID_DRIVE:           errno = ENOENT;     break;
        case ERROR_NO_MORE_FILES:           errno = ENOENT;     break;
        case ERROR_BAD_NETPATH:             errno = ENOENT;     break;
        case ERROR_BAD_NET_NAME:            errno = ENOENT;     break;
        case ERROR_BAD_PATHNAME:            errno = ENOENT;     break;
        case ERROR_FILENAME_EXCED_RANGE:    errno = ENOENT;     break;
        case ERROR_BAD_FORMAT:              errno = ENOEXEC;    break;
        case ERROR_ARENA_TRASHED:           errno = ENOMEM;     break;
        case ERROR_NOT_ENOUGH_MEMORY:       errno = ENOMEM;     break;
        case ERROR_INVALID_BLOCK:           errno = ENOMEM;     break;
        case ERROR_NOT_ENOUGH_QUOTA:        errno = ENOMEM;     break;
        case ERROR_DISK_FULL:               errno = ENOSPC;     break;
        case ERROR_DIR_NOT_EMPTY:           errno = ENOTEMPTY;  break;
        case ERROR_BROKEN_PIPE:             errno = EPIPE;      break;
        case ERROR_NOT_SAME_DEVICE:         errno = EXDEV;      break;
        default:                            errno = EINVAL;     break;
    }
    return -1;
}

DWORD shfile_query_handle_access_mask(HANDLE h, PACCESS_MASK pMask)
{
    static PFN_NtQueryObject        s_pfnNtQueryObject = NULL;
    MY_OBJECT_BASIC_INFORMATION     BasicInfo;
    LONG                            Status;

    if (!s_pfnNtQueryObject)
    {
        s_pfnNtQueryObject = (PFN_NtQueryObject)GetProcAddress(GetModuleHandle("NTDLL"), "NtQueryObject");
        if (!s_pfnNtQueryObject)
            return ERROR_NOT_SUPPORTED;
    }

    Status = s_pfnNtQueryObject(h, MY_ObjectBasicInformation, &BasicInfo, sizeof(BasicInfo), NULL);
    if (Status >= 0)
    {
        *pMask = BasicInfo.GrantedAccess;
        return NO_ERROR;
    }
    if (Status != STATUS_INVALID_HANDLE)
        return ERROR_GEN_FAILURE;
    return ERROR_INVALID_HANDLE;
}

# endif /* K_OS == K_OS_WINDOWS */

#endif /* SHFILE_IN_USE */

/**
 * Initializes a file descriptor table.
 *
 * @returns 0 on success, -1 and errno on failure.
 * @param   pfdtab      The table to initialize.
 * @param   inherit     File descriptor table to inherit from. If not specified
 *                      we will inherit from the current process as it were.
 */
int shfile_init(shfdtab *pfdtab, shfdtab *inherit)
{
    int rc;

    pfdtab->cwd  = NULL;
    pfdtab->size = 0;
    pfdtab->tab  = NULL;
    rc = shmtx_init(&pfdtab->mtx);
    if (!rc)
    {
#ifdef SHFILE_IN_USE
        char buf[SHFILE_MAX_PATH];
        if (getcwd(buf, sizeof(buf)))
        {
            pfdtab->cwd = sh_strdup(NULL, buf);
            if (!inherit)
            {
# if K_OS == K_OS_WINDOWS
                static const struct
                {
                    DWORD dwStdHandle;
                    unsigned fFlags;
                } aStdHandles[3] =
                {
                    { STD_INPUT_HANDLE,   _O_RDONLY },
                    { STD_OUTPUT_HANDLE,  _O_WRONLY },
                    { STD_ERROR_HANDLE,   _O_WRONLY }
                };
                int             i;
                STARTUPINFO     Info;
                ACCESS_MASK     Mask;
                DWORD           dwErr;

                rc = 0;

                /* Try pick up the Visual C++ CRT file descriptor info. */
                __try {
                    GetStartupInfo(&Info);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    memset(&Info, 0, sizeof(Info));
                }

                if (    Info.cbReserved2 > sizeof(int)
                    &&  (uintptr_t)Info.lpReserved2 >= 0x1000
                    &&  (i = *(int *)Info.lpReserved2) >= 1
                    &&  i <= 2048
                    &&  (   Info.cbReserved2 == i * 5 + 4
                         //|| Info.cbReserved2 == i * 5 + 1 - check the cygwin sources.
                         || Info.cbReserved2 == i * 9 + 4))
                {
                    uint8_t    *paf    = (uint8_t *)Info.lpReserved2 + sizeof(int);
                    int         dwPerH = 1 + (Info.cbReserved2 == i * 9 + 4);
                    DWORD      *ph     = (DWORD *)(paf + i) + dwPerH * i;
                    HANDLE      aStdHandles2[3];
                    int         j;

                    //if (Info.cbReserved2 == i * 5 + 1) - check the cygwin sources.
                    //    i--;

                    for (j = 0; j < 3; j++)
                        aStdHandles2[j] = GetStdHandle(aStdHandles[j].dwStdHandle);

                    while (i-- > 0)
                    {
                        ph -= dwPerH;

                        if (    (paf[i] & (FOPEN | FNOINHERIT)) == FOPEN
                            &&  *ph != (uint32_t)INVALID_HANDLE_VALUE)
                        {
                            HANDLE  h = (HANDLE)(intptr_t)*ph;
                            int     fd2;
                            int     fFlags;
                            int     fFlags2;

                            if (    h == aStdHandles2[j = 0]
                                ||  h == aStdHandles2[j = 1]
                                ||  h == aStdHandles2[j = 2])
                                fFlags = aStdHandles[j].fFlags;
                            else
                            {
                                dwErr = shfile_query_handle_access_mask(h, &Mask);
                                if (dwErr == ERROR_INVALID_HANDLE)
                                    continue;
                                else if (dwErr == NO_ERROR)
                                {
                                    fFlags = 0;
                                    if (    (Mask & (GENERIC_READ | FILE_READ_DATA))
                                        &&  (Mask & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)))
                                        fFlags |= O_RDWR;
                                    else if (Mask & (GENERIC_READ | FILE_READ_DATA))
                                        fFlags |= O_RDONLY;
                                    else if (Mask & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA))
                                        fFlags |= O_WRONLY;
                                    else
                                        fFlags |= O_RDWR;
                                    if ((Mask & (FILE_WRITE_DATA | FILE_APPEND_DATA)) == FILE_APPEND_DATA)
                                        fFlags |= O_APPEND;
                                }
                                else
                                    fFlags = O_RDWR;
                            }

                            if (paf[i] & FPIPE)
                                fFlags2 = SHFILE_FLAGS_PIPE;
                            else if (paf[i] & FDEV)
                                fFlags2 = SHFILE_FLAGS_TTY;
                            else
                                fFlags2 = 0;

                            fd2 = shfile_insert(pfdtab, (intptr_t)h, fFlags, fFlags2, i, "shtab_init");
                            assert(fd2 == i); (void)fd2;
                            if (fd2 != i)
                                rc = -1;
                        }
                    }
                }

                /* Check the three standard handles. */
                for (i = 0; i < 3; i++)
                    if (    (unsigned)i >= pfdtab->size
                        ||  pfdtab->tab[i].fd == -1)
                    {
                        HANDLE hFile = GetStdHandle(aStdHandles[i].dwStdHandle);
                        if (hFile != INVALID_HANDLE_VALUE)
                        {
                            int fd2 = shfile_insert(pfdtab, (intptr_t)hFile, aStdHandles[i].fFlags, 0, i, "shtab_init");
                            assert(fd2 == i); (void)fd2;
                            if (fd2 != i)
                                rc = -1;
                        }
                    }
# else
# endif
            }
            else
            {
                /** @todo */
                errno = ENOSYS;
                rc = -1;
            }
        }
        else
            rc = -1;
#endif
    }
    return rc;
}

#if K_OS == K_OS_WINDOWS && defined(SHFILE_IN_USE)

/**
 * Helper for shfork.
 *
 * @param   pfdtab  The file descriptor table.
 * @param   set     Whether to make all handles inheritable (1) or
 *                  to restore them to the rigth state (0).
 * @param   hndls   Where to store the three standard handles.
 */
void shfile_fork_win(shfdtab *pfdtab, int set, intptr_t *hndls)
{
    shmtxtmp tmp;
    unsigned i;
    DWORD fFlag = set ? HANDLE_FLAG_INHERIT : 0;

    shmtx_enter(&pfdtab->mtx, &tmp);
    TRACE2((NULL, "shfile_fork_win:\n"));

    i = pfdtab->size;
    while (i-- > 0)
    {
        if (pfdtab->tab[i].fd == i)
        {
            HANDLE hFile = (HANDLE)pfdtab->tab[i].native;
            if (set)
                TRACE2((NULL, "  #%d: native=%#x oflags=%#x shflags=%#x\n",
                        i, pfdtab->tab[i].oflags, pfdtab->tab[i].shflags, hFile));
            if (!SetHandleInformation(hFile, HANDLE_FLAG_INHERIT, fFlag))
            {
                DWORD err = GetLastError();
                assert(0);
            }
        }
    }

    if (hndls)
    {
        for (i = 0; i < 3; i++)
        {
            if (    pfdtab->size > i
                &&  pfdtab->tab[i].fd == 0)
                hndls[i] = pfdtab->tab[i].native;
            else
                hndls[i] = (intptr_t)INVALID_HANDLE_VALUE;
        }
    }

    shmtx_leave(&pfdtab->mtx, &tmp);
}

/**
 * Helper for sh_execve.
 *
 * This is called before and after CreateProcess. On the first call it
 * will mark the non-close-on-exec handles as inheritable and produce
 * the startup info for the CRT. On the second call, after CreateProcess,
 * it will restore the handle inheritability properties.
 *
 * @returns Pointer to CRT data if prepare is 1, NULL if prepare is 0.
 * @param   pfdtab  The file descriptor table.
 * @param   prepare Which call, 1 if before and 0 if after.
 * @param   sizep   Where to store the size of the returned data.
 * @param   hndls   Where to store the three standard handles.
 */
void *shfile_exec_win(shfdtab *pfdtab, int prepare, unsigned short *sizep, intptr_t *hndls)
{
    void       *pvRet;
    shmtxtmp    tmp;
    unsigned    count;
    unsigned    i;

    shmtx_enter(&pfdtab->mtx, &tmp);
    TRACE2((NULL, "shfile_fork_win:\n"));

    count  = pfdtab->size < (0x10000-4) / (1 + sizeof(HANDLE))
           ? pfdtab->size
           : (0x10000-4) / (1 + sizeof(HANDLE));
    while (count > 3 && pfdtab->tab[count].fd == -1)
        count--;

    if (prepare)
    {
        size_t      cbData = sizeof(int) + count * (1 + sizeof(HANDLE));
        uint8_t    *pbData = sh_malloc(shthread_get_shell(), cbData);
        uint8_t    *paf = pbData + sizeof(int);
        HANDLE     *pah = (HANDLE *)(paf + count);

        *(int *)pbData = count;

        i = count;
        while (i-- > 0)
        {
            if (    pfdtab->tab[i].fd == i
                &&  !(pfdtab->tab[i].shflags & SHFILE_FLAGS_CLOSE_ON_EXEC))
            {
                HANDLE hFile = (HANDLE)pfdtab->tab[i].native;
                TRACE2((NULL, "  #%d: native=%#x oflags=%#x shflags=%#x\n",
                        i, pfdtab->tab[i].oflags, pfdtab->tab[i].shflags, hFile));

                if (!SetHandleInformation(hFile, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
                {
                    DWORD err = GetLastError();
                    assert(0);
                }
                paf[i] = FOPEN;
                if (pfdtab->tab[i].oflags & _O_APPEND)
                    paf[i] |= FAPPEND;
                if (pfdtab->tab[i].oflags & _O_TEXT)
                    paf[i] |= FTEXT;
                switch (pfdtab->tab[i].shflags & SHFILE_FLAGS_TYPE_MASK)
                {
                    case SHFILE_FLAGS_TTY:  paf[i] |= FDEV; break;
                    case SHFILE_FLAGS_PIPE: paf[i] |= FPIPE; break;
                }
                pah[i] = hFile;
            }
            else
            {
                paf[i] = 0;
                pah[i] = INVALID_HANDLE_VALUE;
            }
        }

        for (i = 0; i < 3; i++)
        {
            if (    count > i
                &&  pfdtab->tab[i].fd == 0)
                hndls[i] = pfdtab->tab[i].native;
            else
                hndls[i] = (intptr_t)INVALID_HANDLE_VALUE;
        }

        *sizep = (unsigned short)cbData;
        pvRet = pbData;
    }
    else
    {
        assert(!hndls);
        assert(!sizep);
        i = count;
        while (i-- > 0)
        {
            if (    pfdtab->tab[i].fd == i
                &&  !(pfdtab->tab[i].shflags & SHFILE_FLAGS_CLOSE_ON_EXEC))
            {
                HANDLE hFile = (HANDLE)pfdtab->tab[i].native;
                if (!SetHandleInformation(hFile, HANDLE_FLAG_INHERIT, 0))
                {
                    DWORD err = GetLastError();
                    assert(0);
                }
            }
        }
        pvRet = NULL;
    }

    shmtx_leave(&pfdtab->mtx, &tmp);
    return pvRet;
}

#endif /* K_OS_WINDOWS */


/**
 * open().
 */
int shfile_open(shfdtab *pfdtab, const char *name, unsigned flags, mode_t mode)
{
    int fd;
#ifdef SHFILE_IN_USE
    char    absname[SHFILE_MAX_PATH];
# if K_OS == K_OS_WINDOWS
    HANDLE  hFile;
    DWORD   dwDesiredAccess;
    DWORD   dwShareMode;
    DWORD   dwCreationDisposition;
    DWORD   dwFlagsAndAttributes;
    SECURITY_ATTRIBUTES SecurityAttributes;

# ifndef _O_ACCMODE
#  define _O_ACCMODE	(_O_RDONLY|_O_WRONLY|_O_RDWR)
# endif
    switch (flags & (_O_ACCMODE | _O_APPEND))
    {
        case _O_RDONLY:             dwDesiredAccess = GENERIC_READ; break;
        case _O_RDONLY | _O_APPEND: dwDesiredAccess = GENERIC_READ; break;
        case _O_WRONLY:             dwDesiredAccess = GENERIC_WRITE; break;
        case _O_WRONLY | _O_APPEND: dwDesiredAccess = (FILE_GENERIC_WRITE & ~FILE_WRITE_DATA); break;
        case _O_RDWR:               dwDesiredAccess = GENERIC_READ | GENERIC_WRITE; break;
        case _O_RDWR   | _O_APPEND: dwDesiredAccess = GENERIC_READ | (FILE_GENERIC_WRITE & ~FILE_WRITE_DATA); break;

        default:
            RETURN_ERROR(-1, EINVAL, "invalid mode");
    }

    dwShareMode = FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE;

    SecurityAttributes.nLength = sizeof(SecurityAttributes);
    SecurityAttributes.lpSecurityDescriptor = NULL;
    SecurityAttributes.bInheritHandle = FALSE;

    if (flags & _O_CREAT)
    {
        if ((flags & (_O_EXCL | _O_TRUNC)) == (_O_EXCL | _O_TRUNC))
            RETURN_ERROR(-1, EINVAL, "_O_EXCL | _O_TRUNC");

        if (flags & _O_TRUNC)
            dwCreationDisposition = CREATE_ALWAYS; /* not 100%, but close enough */
        else if (flags & _O_EXCL)
            dwCreationDisposition = CREATE_NEW;
        else
            dwCreationDisposition = OPEN_ALWAYS;
    }
    else if (flags & _O_TRUNC)
        dwCreationDisposition = TRUNCATE_EXISTING;
    else
        dwCreationDisposition = OPEN_EXISTING;

    if (!(flags & _O_CREAT) || (mode & 0222))
        dwFlagsAndAttributes = FILE_ATTRIBUTE_NORMAL;
    else
        dwFlagsAndAttributes = FILE_ATTRIBUTE_READONLY;

    fd = shfile_make_path(pfdtab, name, &absname[0]);
    if (!fd)
    {
        SetLastError(0);
        hFile = CreateFileA(absname,
                            dwDesiredAccess,
                            dwShareMode,
                            &SecurityAttributes,
                            dwCreationDisposition,
                            dwFlagsAndAttributes,
                            NULL /* hTemplateFile */);
        if (hFile != INVALID_HANDLE_VALUE)
            fd = shfile_insert(pfdtab, (intptr_t)hFile, flags, 0, -1, "shfile_open");
        else
            fd = shfile_dos2errno(GetLastError());
    }

# else  /* K_OS != K_OS_WINDOWS */
    fd = shfile_make_path(pfdtab, name, &absname[0]);
    if (!fd)
    {
        fd = open(absname, flag, mode);
        if (fd != -1)
        {
            int native = fcntl(fd, F_DUPFD, SHFILE_UNIX_MIN_FD);
            int s = errno;
            close(fd);
            errno = s;
            if (native != -1)
                fd = shfile_insert(pfdtab, native, flags, 0, -1, "shfile_open");
            else
                fd = -1;
        }
    }

# endif /* K_OS != K_OS_WINDOWS */

#else
    fd = open(name, flags, mode);
#endif

    TRACE2((NULL, "shfile_open(%p:{%s}, %#x, 0%o) -> %d [%d]\n", name, name, flags, mode, fd, errno));
    return fd;
}

int shfile_pipe(shfdtab *pfdtab, int fds[2])
{
    int rc;
#ifdef SHFILE_IN_USE
# if K_OS == K_OS_WINDOWS
    HANDLE hRead  = INVALID_HANDLE_VALUE;
    HANDLE hWrite = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES SecurityAttributes;

    SecurityAttributes.nLength = sizeof(SecurityAttributes);
    SecurityAttributes.lpSecurityDescriptor = NULL;
    SecurityAttributes.bInheritHandle = FALSE;

    fds[1] = fds[0] = -1;
    if (CreatePipe(&hRead, &hWrite, &SecurityAttributes, 4096))
    {
        fds[0] = shfile_insert(pfdtab, (intptr_t)hRead, O_RDONLY, SHFILE_FLAGS_PIPE, -1, "shfile_pipe");
        if (fds[0] != -1)
        {
            fds[1] = shfile_insert(pfdtab, (intptr_t)hWrite, O_WRONLY, SHFILE_FLAGS_PIPE, -1, "shfile_pipe");
            if (fds[1] != -1)
                rc = 0;
        }

# else
    int native_fds[2];

    fds[1] = fds[0] = -1;
    if (!pipe(native_fds))
    {
        fds[0] = shfile_insert(pfdtab, native_fds[0], O_RDONLY, SHFILE_FLAGS_PIPE, -1, "shfile_pipe");
        if (fds[0] != -1)
        {
            fds[1] = shfile_insert(pfdtab, native_fds[1], O_WRONLY, SHFILE_FLAGS_PIPE, -1, "shfile_pipe");
            if (fds[1] != -1)
                rc = 0;
        }
# endif
        if (fds[1] == -1)
        {
            int s = errno;
            if (fds[0] != -1)
            {
                shmtxtmp tmp;
                shmtx_enter(&pfdtab->mtx, &tmp);
                rc = fds[0];
                pfdtab->tab[rc].fd = -1;
                pfdtab->tab[rc].oflags = 0;
                pfdtab->tab[rc].shflags = 0;
                pfdtab->tab[rc].native = -1;
                shmtx_leave(&pfdtab->mtx, &tmp);
            }

# if K_OS == K_OS_WINDOWS
            CloseHandle(hRead);
            CloseHandle(hWrite);
# else
            close(native_fds[0]);
            close(native_fds[1]);
# endif
            fds[0] = fds[1] = -1;
            errno = s;
            rc = -1;
        }
    }
    else
    {
# if K_OS == K_OS_WINDOWS
        errno = shfile_dos2errno(GetLastError());
# endif
        rc = -1;
    }

#else
    rc = pipe(fds);
#endif

    TRACE2((NULL, "shfile_pipe() -> %d{%d,%d} [%d]\n", rc, fds[0], fds[1], errno));
    return rc;
}

/**
 * dup().
 */
int shfile_dup(shfdtab *pfdtab, int fd)
{
    return shfile_fcntl(pfdtab,fd, F_DUPFD, 0);
}

/**
 * close().
 */
int shfile_close(shfdtab *pfdtab, unsigned fd)
{
    int         rc;
#ifdef SHFILE_IN_USE
    shmtxtmp    tmp;
    shfile     *file = shfile_get(pfdtab, fd, &tmp);
    if (file)
    {
        shfile_native_close(file->native, file->oflags);

        file->fd = -1;
        file->oflags = 0;
        file->shflags = 0;
        file->native = -1;

        shfile_put(pfdtab, file, &tmp);
        rc = 0;
    }
    else
        rc = -1;

#else
    rc = close(fd);
#endif

    TRACE2((NULL, "shfile_close(%d) -> %d [%d]\n", fd, rc, errno));
    return rc;
}

/**
 * read().
 */
long shfile_read(shfdtab *pfdtab, int fd, void *buf, size_t len)
{
    long        rc;
#ifdef SHFILE_IN_USE
    shmtxtmp    tmp;
    shfile     *file = shfile_get(pfdtab, fd, &tmp);
    if (file)
    {
# if K_OS == K_OS_WINDOWS
        DWORD dwRead = 0;
        if (ReadFile((HANDLE)file->native, buf, (DWORD)len, &dwRead, NULL))
            rc = dwRead;
        else
            rc = shfile_dos2errno(GetLastError());
# else
        rc = read(file->native, buf, len);
# endif

        shfile_put(pfdtab, file, &tmp);
    }
    else
        rc = -1;

#else
    rc = read(fd, buf, len);
#endif
    return rc;
}

/**
 * write().
 */
long shfile_write(shfdtab *pfdtab, int fd, const void *buf, size_t len)
{
    long        rc;
#ifdef SHFILE_IN_USE
    shmtxtmp    tmp;
    shfile     *file = shfile_get(pfdtab, fd, &tmp);
    if (file)
    {
# if K_OS == K_OS_WINDOWS
        DWORD dwWritten = 0;
        if (WriteFile((HANDLE)file->native, buf, (DWORD)len, &dwWritten, NULL))
            rc = dwWritten;
        else
            rc = shfile_dos2errno(GetLastError());
# else
        rc = write(file->native, buf, len);
# endif

        shfile_put(pfdtab, file, &tmp);
    }
    else
        rc = -1;

#else
    rc = write(fd, buf, len);
#endif
    return rc;
}

/**
 * lseek().
 */
long shfile_lseek(shfdtab *pfdtab, int fd, long off, int whench)
{
    long        rc;
#ifdef SHFILE_IN_USE
    shmtxtmp    tmp;
    shfile     *file = shfile_get(pfdtab, fd, &tmp);
    if (file)
    {
# if K_OS == K_OS_WINDOWS
        assert(SEEK_SET == FILE_BEGIN);
        assert(SEEK_CUR == FILE_CURRENT);
        assert(SEEK_END == FILE_END);
        rc = SetFilePointer((HANDLE)file->native, off, NULL, whench);
        if (rc == INVALID_SET_FILE_POINTER)
            rc = shfile_dos2errno(GetLastError());
# else
        rc = lseek(file->native, off, whench);
# endif

        shfile_put(pfdtab, file, &tmp);
    }
    else
        rc = -1;

#else
    rc = lseek(fd, off, whench);
#endif

    return rc;
}

int shfile_fcntl(shfdtab *pfdtab, int fd, int cmd, int arg)
{
    int rc;
#ifdef SHFILE_IN_USE
    shmtxtmp    tmp;
    shfile     *file = shfile_get(pfdtab, fd, &tmp);
    if (file)
    {
        switch (cmd)
        {
            case F_GETFL:
                rc = file->oflags;
                break;

            case F_SETFL:
            {
                unsigned mask = O_NONBLOCK | O_APPEND | O_BINARY | O_TEXT;
# ifdef O_DIRECT
                mask |= O_DIRECT;
# endif
# ifdef O_ASYNC
                mask |= O_ASYNC;
# endif
# ifdef O_SYNC
                mask |= O_SYNC;
# endif
                if ((file->oflags & mask) == (arg & mask))
                    rc = 0;
                else
                {
# if K_OS == K_OS_WINDOWS
                    assert(0);
                    errno = EINVAL;
                    rc = -1;
# else
                    rc = fcntl(file->native, F_SETFL, arg);
                    if (rc != -1)
                        file->flags = (file->flags & ~mask) | (arg & mask);
# endif
                }
                break;
            }

            case F_DUPFD:
            {
# if K_OS == K_OS_WINDOWS
                HANDLE hNew = INVALID_HANDLE_VALUE;
                if (DuplicateHandle(GetCurrentProcess(),
                                    (HANDLE)file->native,
                                    GetCurrentProcess(),
                                    &hNew,
                                    0,
                                    FALSE /* bInheritHandle */,
                                    DUPLICATE_SAME_ACCESS))
                    rc = shfile_insert(pfdtab, (intptr_t)hNew, file->oflags, file->shflags, arg, "shfile_fcntl");
                else
                    rc = shfile_dos2errno(GetLastError());
# else
                int nativeNew = fcntl(file->native F_DUPFD, SHFILE_UNIX_MIN_FD);
                if (nativeNew != -1)
                    rc = shfile_insert(pfdtab, nativeNew, file->oflags, file->shflags, arg, "shfile_fcntl");
# endif
                break;
            }

            default:
                errno = -EINVAL;
                rc = -1;
                break;
        }

        shfile_put(pfdtab, file, &tmp);
    }
    else
        rc = -1;

#else
    rc = fcntl(fd, cmd, arg);
#endif

    switch (cmd)
    {
        case F_GETFL: TRACE2((NULL, "shfile_fcntl(%d,F_GETFL,ignored=%d) -> %d [%d]\n", fd, arg, rc, errno));  break;
        case F_SETFL: TRACE2((NULL, "shfile_fcntl(%d,F_SETFL,newflags=%#x) -> %d [%d]\n", fd, arg, rc, errno)); break;
        case F_DUPFD: TRACE2((NULL, "shfile_fcntl(%d,F_DUPFS,minfd=%d) -> %d [%d]\n", fd, arg, rc, errno)); break;
        default:  TRACE2((NULL, "shfile_fcntl(%d,%d,%d) -> %d [%d]\n", fd, cmd, arg, rc, errno)); break;
    }
    return rc;
}

int shfile_stat(shfdtab *pfdtab, const char *path, struct stat *pst)
{
#ifdef SHFILE_IN_USE
    char    abspath[SHFILE_MAX_PATH];
    int     rc;
    rc = shfile_make_path(pfdtab, path, &abspath[0]);
    if (!rc)
    {
# if K_OS == K_OS_WINDOWS
        rc = stat(abspath, pst); /** @todo re-implement stat. */
# else
        rc = stat(abspath, pst);
# endif
    }
    TRACE2((NULL, "shfile_stat(,%s,) -> %d [%d]\n", path, rc, errno));
    return rc;
#else
    return stat(path, pst);
#endif
}

int shfile_lstat(shfdtab *pfdtab, const char *path, struct stat *pst)
{
    int     rc;
#ifdef SHFILE_IN_USE
    char    abspath[SHFILE_MAX_PATH];

    rc = shfile_make_path(pfdtab, path, &abspath[0]);
    if (!rc)
    {
# if K_OS == K_OS_WINDOWS
        rc = stat(abspath, pst); /** @todo implement lstat. */
# else
        rc = lstat(abspath, pst);
# endif
    }
#else
    rc = stat(path, pst);
#endif
    TRACE2((NULL, "shfile_stat(,%s,) -> %d [%d]\n", path, rc, errno));
    return rc;
}

/**
 * chdir().
 */
int shfile_chdir(shfdtab *pfdtab, const char *path)
{
    shinstance *psh = shthread_get_shell();
    int         rc;
#ifdef SHFILE_IN_USE
    char        abspath[SHFILE_MAX_PATH];

    rc = shfile_make_path(pfdtab, path, &abspath[0]);
    if (!rc)
    {
        char *abspath_copy = sh_strdup(psh, abspath);
        char *free_me = abspath_copy;
        rc = chdir(path);
        if (!rc)
        {
            shmtxtmp    tmp;
            shmtx_enter(&pfdtab->mtx, &tmp);

            free_me = pfdtab->cwd;
            pfdtab->cwd = abspath_copy;

            shmtx_leave(&pfdtab->mtx, &tmp);
        }
        sh_free(psh, free_me);
    }
    else
        rc = -1;
#else
    rc = chdir(path);
#endif

    TRACE2((psh, "shfile_chdir(,%s) -> %d [%d]\n", path, rc, errno));
    return rc;
}

/**
 * getcwd().
 */
char *shfile_getcwd(shfdtab *pfdtab, char *buf, int size)
{
    char       *ret;
#ifdef SHFILE_IN_USE

    ret = NULL;
    if (buf && !size)
        errno = -EINVAL;
    else
    {
        size_t      cwd_size;
        shmtxtmp    tmp;
        shmtx_enter(&pfdtab->mtx, &tmp);

        cwd_size = strlen(pfdtab->cwd) + 1;
        if (buf)
        {
            if (cwd_size <= (size_t)size)
                ret = memcpy(buf, pfdtab->cwd, cwd_size);
            else
                errno = ERANGE;
        }
        else
        {
            if (size < cwd_size)
                size = (int)cwd_size;
            ret = sh_malloc(shthread_get_shell(), size);
            if (ret)
                ret = memcpy(ret, pfdtab->cwd, cwd_size);
            else
                errno = ENOMEM;
        }

        shmtx_leave(&pfdtab->mtx, &tmp);
    }
#else
    ret = getcwd(buf, size);
#endif

    TRACE2((NULL, "shfile_getcwd(,%p,%d) -> %s [%d]\n", buf, size, ret, errno));
    return ret;
}

/**
 * access().
 */
int shfile_access(shfdtab *pfdtab, const char *path, int type)
{
    int         rc;
#ifdef SHFILE_IN_USE
    char        abspath[SHFILE_MAX_PATH];

    rc = shfile_make_path(pfdtab, path, &abspath[0]);
    if (!rc)
    {
# ifdef _MSC_VER
        if (type & X_OK)
            type = (type & ~X_OK) | R_OK;
# endif
        rc = access(abspath, type);
    }
#else
# ifdef _MSC_VER
    if (type & X_OK)
        type = (type & ~X_OK) | R_OK;
# endif
    rc = access(path, type);
#endif

    TRACE2((NULL, "shfile_access(,%s,%#x) -> %d [%d]\n", path, type, rc, errno));
    return rc;
}

/**
 * isatty()
 */
int shfile_isatty(shfdtab *pfdtab, int fd)
{
    int         rc;
#ifdef SHFILE_IN_USE
    shmtxtmp    tmp;
    shfile     *file = shfile_get(pfdtab, fd, &tmp);
    if (file)
    {
# if K_OS == K_OS_WINDOWS
        rc = (file->shflags & SHFILE_FLAGS_TYPE_MASK) == SHFILE_FLAGS_TTY;
# else
        rc = isatty(file->native);
# endif
        shfile_put(pfdtab, file, &tmp);
    }
    else
        rc = 0;
#else
    rc = isatty(fd);
#endif

    TRACE2((NULL, "isatty(%d) -> %d [%d]\n", fd, rc, errno));
    return rc;
}

/**
 * fcntl F_SETFD / FD_CLOEXEC.
 */
int shfile_cloexec(shfdtab *pfdtab, int fd, int closeit)
{
    int         rc;
#ifdef SHFILE_IN_USE
    shmtxtmp    tmp;
    shfile     *file = shfile_get(pfdtab, fd, &tmp);
    if (file)
    {
        if (closeit)
            file->shflags |= SHFILE_FLAGS_CLOSE_ON_EXEC;
        else
            file->shflags &= ~SHFILE_FLAGS_CLOSE_ON_EXEC;
        shfile_put(pfdtab, file, &tmp);
    }
    else
        rc = -1;
#else
    rc = fcntl(fd, F_SETFD, fcntl(fd, F_GETFD, 0)
                          | (closeit ? FD_CLOEXEC : 0));
#endif

    TRACE2((NULL, "shfile_cloexec(%d, %d) -> %d [%d]\n", fd, closeit, rc, errno));
    return rc;
}


int shfile_ioctl(shfdtab *pfdtab, int fd, unsigned long request, void *buf)
{
    int         rc;
#ifdef SHFILE_IN_USE
    shmtxtmp    tmp;
    shfile     *file = shfile_get(pfdtab, fd, &tmp);
    if (file)
    {
# if K_OS == K_OS_WINDOWS
        rc = -1;
        errno = ENOSYS;
# else
        rc = ioctl(file->native, request, buf);
# endif
        shfile_put(pfdtab, file, &tmp);
    }
    else
        rc = -1;
#else
    rc = ioctl(fd, request, buf);
#endif

    TRACE2((NULL, "ioctl(%d, %#x, %p) -> %d\n", fd, request, buf, rc));
    return rc;
}


mode_t shfile_get_umask(shfdtab *pfdtab)
{
    /** @todo */
    return 022;
}

void shfile_set_umask(shfdtab *pfdtab, mode_t mask)
{
    /** @todo */
    (void)mask;
}


shdir *shfile_opendir(shfdtab *pfdtab, const char *dir)
{
#ifdef SHFILE_IN_USE
    errno = ENOSYS;
    return NULL;
#else
    return (shdir *)opendir(dir);
#endif
}

shdirent *shfile_readdir(struct shdir *pdir)
{
#ifdef SHFILE_IN_USE
    errno = ENOSYS;
    return NULL;
#else
    struct dirent *pde = readdir((DIR *)pdir);
    return pde ? (shdirent *)&pde->d_name[0] : NULL;
#endif
}

void shfile_closedir(struct shdir *pdir)
{
#ifdef SHFILE_IN_USE
    errno = ENOSYS;
#else
    closedir((DIR *)pdir);
#endif
}

