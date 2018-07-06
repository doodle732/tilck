
#pragma once
#include <exos/common/basic_defs.h>

typedef struct {

   /* Main functions */
   void (*set_char_at)(int row, int col, u16 entry);
   void (*set_row)(int row, u16 *data, bool flush); // NOTE: set_row() can
                                                    // safely assume that it has
                                                    // been called in a FPU
                                                    // context.
   void (*clear_row)(int row_num, u8 color);

   /* Cursor management */
   void (*move_cursor)(int row, int col);
   void (*enable_cursor)(void);
   void (*disable_cursor)(void);

   /* Other (optional) */
   void (*scroll_one_line_up)(void);
   void (*flush_buffers)(void);

} video_interface;


void init_term(const video_interface *vi,
               int rows,
               int cols,
               u8 default_color,
               bool use_serial_port);

bool term_is_initialized(void);

u32 term_get_tab_size(void);
u32 term_get_rows(void);
u32 term_get_cols(void);

u32 term_get_curr_row(void);
u32 term_get_curr_col(void);

void term_write(const char *buf, u32 len);
void term_write2(const char *buf, u32 len, u8 color);
void term_move_ch_and_cur(u32 row, u32 col);
void term_scroll_up(u32 lines);
void term_scroll_down(u32 lines);
void term_set_color(u8 color);
void term_set_col_offset(u32 off);

#define TERM_FILTER_FUNC_RET_WRITE_C   0
#define TERM_FILTER_FUNC_RET_BLANK     1

typedef void (*term_int_write_char_func)(char c, u8 color);

typedef int (*term_filter_func)(char *c    /* in/out */,
                                u8 *color  /* in/out */,
                                term_int_write_char_func int_write_char,
                                void *ctx);

void term_set_filter_func(term_filter_func func, void *ctx);

/* --- debug funcs --- */
void debug_term_print_scroll_cycles(void);
