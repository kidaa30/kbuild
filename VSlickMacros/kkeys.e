/* $Id: kkeys.e 2243 2009-01-10 02:24:02Z bird $ */
/** @file
 * Bird's key additions to Visual Slickedit.
 */

/*
 * Copyright (c) 2004-2009 knut st. osmundsen <bird-kBuild-spamix@anduin.net>
 *
 * This file is part of kBuild.
 *
 * kBuild is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * kBuild is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with kBuild.  If not, see <http://www.gnu.org/licenses/>
 *
 */

defeventtab default_keys
def  'A-UP'     = find_prev
def  'A-DOWN'   = find_next
def  'A-PGUP'   = prev_proc
def  'A-PGDN'   = next_proc
def  'A-d'      = delete_line
def  'A-o'      = kkeys_duplicate_line
def  'A-s'      = kkeys_switch_lines
def  'A-u'      = undo_cursor           /* will cursor movement in one undo step. */
def  'A-g'      = goto_line
def  'A-z'      = kkeys_fullscreen
def  'INS'      = boxer_paste
def  'S-INS'    = insert_toggle
def  'C-UP'     = kkeys_scroll_down
def  'C-DOWN'   = kkeys_scroll_up
def  'C-PGUP'   = prev_window
def  'C-PGDN'   = next_window
def  'C-DEL'    = kkeys_delete_right
/* For the mac (A/M mix, all except A-z): */
def  'M-UP'     = find_prev
def  'M-DOWN'   = find_next
def  'M-PGUP'   = prev_proc
def  'M-PGDN'   = next_proc
def  'M-d'      = delete_line
def  'M-o'      = kkeys_duplicate_line
def  'M-s'      = kkeys_switch_lines
def  'M-u'      = undo_cursor
def  'M-g'      = goto_line
/* Fixing brainfucked slickedit silliness: */
def  'M-v'      = paste

_command kkeys_switch_lines()
{
   /* Allocate a selection for copying the current line. */
   cursor_down();
   mark_id= _alloc_selection();
   if (mark_id>=0)
   {
      _select_line(mark_id);
      cursor_up();
      cursor_up();
      _move_to_cursor(mark_id);
      cursor_down();
      _free_selection(mark_id);
      // This selection can be freed because it is not the active selection.
   }
   else
      message(get_message(mark_id));
}

_command kkeys_duplicate_line()
{
   /* Allocate a selection for copying the current line. */
   mark_id= _alloc_selection();
   if (mark_id>=0)
   {
      _select_line(mark_id);
      _copy_to_cursor(mark_id);
      // This selection can be freed because it is not the active selection.
      _free_selection(mark_id);
      cursor_down();
   }
   else
       message(get_message(mark_id));
}

_command kkeys_delete_right()
{
   col=p_col
   search('[ \t]#|?|$|^','r+');
   if ( match_length()&& get_text(1,match_length('s'))=='' )
   {
      _nrseek(match_length('s'));
      _delete_text(match_length());
   }
   else
      delete_word();
   p_col=col
   //retrieve_command_results()

}

_command kkeys_delete_left()
{
   //denne virker ikkje som den skal!!!
   message "not implemented"
/*
   return;
   col=p_col
   search('[ \t]#|?|$|^','r-');
   if ( match_length()&& get_text(1,match_length('s'))=='' )
   {
      _nrseek(match_length('s'));
      _delete_text(match_length());
   }
   else
      delete_word();
   p_col=col
*/
}

_command kkeys_scroll_up()
{
   if (p_cursor_y == 0)
      down();
   set_scroll_pos(p_left_edge, p_cursor_y-1);
}

_command kkeys_scroll_down()
{
   if (p_cursor_y intdiv p_font_height == p_char_height-1)
      up()
   set_scroll_pos(p_left_edge, p_cursor_y+p_font_height);
}

_command boxer_paste()
{
   int rc;
   offset = _QROffset();
   message(offset);
   rc = paste();
   _GoToROffset(offset);
}

_command kkeys_fullscreen()
{
    fullscreen();
}


/* for later, not used yet. */

_command boxer_select()
{
   if (command_state())
      fSelected = (p_sel_length != 0);
   else
      fSelected = select_active();

   key = last_event();
   if (key :== name2event('s-down'))
   {
      if (!fSelected)
         select_line();
      else
         cursor_down();
   }
   else if (key :== name2event('s-up'))
   {
      if (!fSelected)
         select_line();
      else
         cursor_up();
   }
   else if (key :== name2event('s-left'))
   {
      if (!fSelected)
         select_char();
      else
         cursor_left();
   }
   else if (key :== name2event('s-right'))
   {
      if (!fSelected)
         select_char();
      else
         cursor_right();
   }
   else if (key :== name2event('s-home'))
   {
      if (!fSelected) select_char();
      begin_line_text_toggle();
   }
   else if (key :== name2event('s-end'))
   {
      if (!fSelected) select_char();
      end_line();
      if (p_col > 0) //this is not identical with boxer...
         cursor_left();
   }
   else if (key :== name2event('c-s-home'))
   {
      if (!fSelected) select_char();
      top_of_buffer();
   }
   else if (key :== name2event('c-s-end'))
   {
      if (!fSelected) select_char();
      bottom_of_buffer();
   }
   else if (key :== name2event('c-s-left'))
   {
      if (!fSelected)
      {
         cursor_left();
         select_char(); /* start this selection non-inclusive */
      }
      prev_word();
   }
   else if (key :== name2event('c-s-right'))
   {
      if (!fSelected)
      {
         select_char(); /* start this selection non-inclusive */
      }
      /* temporary hack */
      prevpos = p_col;
      prevline = p_line;
      p_col++;
      next_word();
      if ((p_line == prevline && p_col > prevpos + 1) || (p_line != prevline && p_col > 0))
         p_col--;
   }
}


void nop()
{

}

