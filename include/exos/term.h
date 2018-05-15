
#pragma once
#include <common/basic_defs.h>

typedef struct {

   /* Main functions */
   void (*set_char_at)(char c, u8 color, int row, int col);
   void (*clear_row)(int row_num, u8 color);

   /* Scrolling */
   void (*scroll_up)(u32 lines);
   void (*scroll_down)(u32 lines);
   bool (*is_at_bottom)(void);
   void (*scroll_to_bottom)(void);
   void (*add_row_and_scroll)(u8 color);

   /* Cursor management */
   void (*move_cursor)(int row, int col);
   void (*enable_cursor)(void);
   void (*disable_cursor)(void);

} video_interface;


void init_term(const video_interface *vi, int rows, int cols, u8 default_color);
bool term_is_initialized(void);
void term_write_char(char c);
void term_write_char2(char c, u8 color);
void term_move_ch(int row, int col);
void term_scroll_up(u32 lines);
void term_scroll_down(u32 lines);
void term_set_color(u8 color);
