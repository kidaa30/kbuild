#include "mscfakes.h"
#include "getopt.h"
#include "process.h"

#define _SC_CLK_TCK 0x101
long _sysconf(int);
pid_t fork(void);
pid_t getpid(void);

