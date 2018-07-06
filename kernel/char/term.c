
#include <exos/common/vga_textmode_defs.h>
#include <exos/common/string_util.h>
#include <exos/common/utils.h>

#include <exos/kernel/hal.h>
#include <exos/kernel/term.h>
#include <exos/kernel/serial.h>
#include <exos/kernel/ringbuf.h>
#include <exos/kernel/kmalloc.h>
#include <exos/kernel/interrupts.h>

#include "term_int.h"

static bool term_initialized;
static bool term_use_serial;
static int term_tab_size = 8;

static u16 term_cols;
static u16 term_rows;
static u16 current_row;
static u16 current_col;
static u16 term_col_offset;
static u8 current_color;

static const video_interface *vi;

static u16 *buffer;
static u32 scroll;
static u32 max_scroll;
static u32 total_buffer_rows;
static u32 extra_buffer_rows;
static u16 failsafe_buffer[80 * 25];
static bool *term_tabs;

static ringbuf term_ringbuf;
static term_action term_actions_buf[32];

static term_filter_func filter;
static void *filter_ctx;

#if TERM_PERF_METRICS
static u32 scroll_count;
static u64 scroll_cycles;
static u32 sc_one_line_count;
static u64 sc_one_line_cycles;
#endif

u32 term_get_tab_size(void)
{
   return term_tab_size;
}

u32 term_get_rows(void)
{
   return term_rows;
}

u32 term_get_cols(void)
{
   return term_cols;
}

u32 term_get_curr_row(void)
{
   return current_row;
}

u32 term_get_curr_col(void)
{
   return current_col;
}

static ALWAYS_INLINE void buffer_set_entry(int row, int col, u16 e)
{
   buffer[(row + scroll) % total_buffer_rows * term_cols + col] = e;
}

static ALWAYS_INLINE bool ts_is_at_bottom(void)
{
   return scroll == max_scroll;
}

static void ts_set_scroll(u32 requested_scroll)
{
   /*
    * 1. scroll cannot be > max_scroll
    * 2. scroll cannot be < max_scroll - extra_buffer_rows, where
    *    extra_buffer_rows = total_buffer_rows - VIDEO_ROWS.
    *    In other words, if for example total_buffer_rows is 26, and max_scroll is
    *    1000, scroll cannot be less than 1000 + 25 - 26 = 999, which means
    *    exactly 1 scroll row (extra_buffer_rows == 1).
    */

   const u32 min_scroll =
      max_scroll > extra_buffer_rows
         ? max_scroll - extra_buffer_rows
         : 0;

   requested_scroll = MIN(MAX(requested_scroll, min_scroll), max_scroll);

   if (requested_scroll == scroll)
      return; /* nothing to do */

   scroll = requested_scroll;

   fpu_context_begin();

   for (u32 row = 0; row < term_rows; row++) {
      u32 buffer_row = (scroll + row) % total_buffer_rows;
      vi->set_row(row, &buffer[term_cols * buffer_row], true);
   }

   fpu_context_end();
}

static ALWAYS_INLINE void ts_scroll_up(u32 lines)
{
   if (lines > scroll)
      ts_set_scroll(0);
   else
      ts_set_scroll(scroll - lines);
}

static ALWAYS_INLINE void ts_scroll_down(u32 lines)
{
   ts_set_scroll(scroll + lines);
}

static ALWAYS_INLINE void ts_scroll_to_bottom(void)
{
   if (scroll != max_scroll) {
      ts_set_scroll(max_scroll);
   }
}

static void ts_clear_row(int row_num, u8 color)
{
   u16 *rowb = buffer + term_cols * ((row_num + scroll) % total_buffer_rows);
   memset16(rowb, make_vgaentry(' ', color), term_cols);
   vi->clear_row(row_num, color);
}

/* ---------------- term actions --------------------- */

#if TERM_PERF_METRICS

void debug_term_print_scroll_cycles(void)
{
   printk("\n");

   if (sc_one_line_count) {
      printk("Avg. cycles per 1-line fast term scroll: %llu K [%u scrolls]\n",
             (sc_one_line_cycles / sc_one_line_count) / 1000,
             sc_one_line_count);
   } else {
      printk("No 1-line fast term scrolls yet.\n");
   }

   if (scroll_count) {
      printk("Avg. cycles per term scroll: %llu K [%u scrolls]\n",
             (scroll_cycles / scroll_count) / 1000, scroll_count);
   } else {
      printk("No term scrolls yet.\n");
   }
}

#else

void debug_term_print_scroll_cycles(void) { }

#endif

static void term_action_set_color(u8 color)
{
   current_color = color;
}

static void term_action_scroll_up(u32 lines)
{
#if TERM_PERF_METRICS
   u64 start, end;
   start = RDTSC();
#endif

   ts_scroll_up(lines);

   if (!ts_is_at_bottom()) {
      vi->disable_cursor();
   } else {
      vi->enable_cursor();
      vi->move_cursor(current_row, current_col);
   }

   if (vi->flush_buffers)
      vi->flush_buffers();

#if TERM_PERF_METRICS
   end = RDTSC();
   scroll_cycles += (end - start);
   scroll_count++;
#endif
}

static void term_action_scroll_down(u32 lines)
{
#if TERM_PERF_METRICS
   u64 start, end;
   start = RDTSC();
#endif

   ts_scroll_down(lines);

   if (ts_is_at_bottom()) {
      vi->enable_cursor();
      vi->move_cursor(current_row, current_col);
   }

   if (vi->flush_buffers)
      vi->flush_buffers();

#if TERM_PERF_METRICS
   end = RDTSC();
   scroll_cycles += (end - start);
   scroll_count++;
#endif
}

static void term_internal_incr_row(void)
{
   term_col_offset = 0;

   if (current_row < term_rows - 1) {
      ++current_row;
      return;
   }

   max_scroll++;

   if (vi->scroll_one_line_up) {
      scroll++;
      vi->scroll_one_line_up();
   } else {
      ts_set_scroll(max_scroll);
   }

   ts_clear_row(term_rows - 1, current_color);
}

static void term_internal_write_printable_char(char c, u8 color)
{
   u16 entry = make_vgaentry(c, color);
   buffer_set_entry(current_row, current_col, entry);
   vi->set_char_at(current_row, current_col, entry);
   current_col++;
}

static void term_internal_write_tab(u8 color)
{
   int rem = term_cols - current_col - 1;

   if (!term_tabs) {

      if (rem)
         term_internal_write_printable_char(' ', color);

      return;
   }

   int tab_col =
      MIN(round_up_at(current_col+1, term_tab_size), (u32)term_cols - 2);
   term_tabs[current_row * term_cols + tab_col] = 1;
   current_col = tab_col + 1;
}

static void term_internal_write_backspace(u8 color)
{
   if (!current_col || current_col <= term_col_offset)
      return;

   const u16 space_entry = make_vgaentry(' ', color);
   current_col--;

   if (!term_tabs || !term_tabs[current_row * term_cols + current_col]) {
      buffer_set_entry(current_row, current_col, space_entry);
      vi->set_char_at(current_row, current_col, space_entry);
      return;
   }

   /* we hit the end of a tab */
   term_tabs[current_row * term_cols + current_col] = 0;

   for (int i = term_tab_size - 1; i >= 0; i--) {

      if (!current_col || current_col == term_col_offset)
         break;

      if (term_tabs[current_row * term_cols + current_col - 1])
         break; /* we hit the previous tab */

      if (i)
         current_col--;
   }
}

static void term_internal_write_char2(char c, u8 color)
{
   if (term_use_serial)
      serial_write(c);

   switch (c) {

      case '\n':
         current_col = 0;
         term_internal_incr_row();
         break;

      case '\r':
         current_col = 0;
         break;

      case '\t':
         term_internal_write_tab(color);
         break;

      case '\b':
         term_internal_write_backspace(color);
         break;

      default:

         term_internal_write_printable_char(c, color);

         if (current_col == term_cols) {
            current_col = 0;
            term_internal_incr_row();
         }

         break;
   }
}

static void term_action_write2(char *buf, u32 len, u8 color)
{
   ts_scroll_to_bottom();

#if TERM_PERF_METRICS
   bool has_new_line = false;
   u64 start, end;
   start = RDTSC();
#endif

   vi->enable_cursor();

   for (u32 i = 0; i < len; i++) {

#if TERM_PERF_METRICS
      if (buf[i] == '\n')
         has_new_line = true;
#endif

      if (filter) {

         char c = buf[i];
         u8 color_to_use = color;

         int ret = filter(&c,
                          &color_to_use,
                          &term_internal_write_char2,
                          filter_ctx);

         if (ret == TERM_FILTER_FUNC_RET_WRITE_C)
            term_internal_write_char2(c, color_to_use);

      } else {
         term_internal_write_char2(buf[i], color);
      }

   }

   vi->move_cursor(current_row, current_col);

   if (vi->flush_buffers)
      vi->flush_buffers();

#if TERM_PERF_METRICS
   end = RDTSC();
   if (has_new_line && current_row == term_rows - 1) {
      sc_one_line_cycles += (end - start);
      sc_one_line_count++;
   }
#endif
}

static void term_action_move_ch_and_cur(int row, int col)
{
   current_row = row;
   current_col = col;
   vi->move_cursor(row, col);

   if (vi->flush_buffers)
      vi->flush_buffers();
}

static void term_action_set_col_offset(u32 off)
{
   term_col_offset = off;
}

/* ---------------- term action engine --------------------- */

static const actions_table_item actions_table[] = {
   [a_write2] = {(action_func)term_action_write2, 3},
   [a_move_ch_and_cur] = {(action_func)term_action_move_ch_and_cur, 2},
   [a_scroll_up] = {(action_func)term_action_scroll_up, 1},
   [a_scroll_down] = {(action_func)term_action_scroll_down, 1},
   [a_set_color] = {(action_func)term_action_set_color, 1},
   [a_set_col_offset] = {(action_func)term_action_set_col_offset, 1}
};

static void term_execute_action(term_action a)
{
   const actions_table_item *it = &actions_table[a.type3];

   switch (it->args_count) {
      case 3:
         it->func(a.ptr, a.len, a.col);
         break;
      case 2:
         it->func(a.arg1, a.arg2);
         break;
      case 1:
         it->func(a.arg);
         break;
      default:
         NOT_REACHED();
      break;
   }
}


void term_execute_or_enqueue_action(term_action a)
{
   bool written;
   bool was_empty;

   written = ringbuf_write_elem_ex(&term_ringbuf, &a, &was_empty);

   /*
    * written would be false only if the ringbuf was full. In order that to
    * happen, we'll need ARRAY_SIZE(term_actions_buf) nested interrupts and
    * all of them need to issue a term_* call. Virtually "impossible".
    */
   VERIFY(written);

   if (was_empty) {

      while (ringbuf_read_elem(&term_ringbuf, &a))
         term_execute_action(a);

   }
}

/* ---------------- term interface --------------------- */

void term_write2(const char *buf, u32 len, u8 color)
{
   ASSERT(len < MB);

   term_action a = {
      .type3 = a_write2,
      .ptr = (uptr)buf,
      .len = MIN(len, MB - 1),
      .col = color
   };

   term_execute_or_enqueue_action(a);
}

void term_move_ch_and_cur(u32 row, u32 col)
{
   term_action a = {
      .type2 = a_move_ch_and_cur,
      .arg1 = row,
      .arg2 = col
   };

   term_execute_or_enqueue_action(a);
}

void term_scroll_up(u32 lines)
{
   term_action a = {
      .type1 = a_scroll_up,
      .arg = lines
   };

   term_execute_or_enqueue_action(a);
}

void term_scroll_down(u32 lines)
{
   term_action a = {
      .type1 = a_scroll_down,
      .arg = lines
   };

   term_execute_or_enqueue_action(a);
}

void term_set_color(u8 color)
{
   term_action a = {
      .type1 = a_set_color,
      .arg = color
   };

   term_execute_or_enqueue_action(a);
}

void term_set_col_offset(u32 off)
{
   term_action a = {
      .type1 = a_set_col_offset,
      .arg = off
   };

   term_execute_or_enqueue_action(a);
}

/* ---------- wrappers ------------ */

void term_write(const char *buf, u32 len)
{
   term_write2(buf, len, current_color);
}

/* ---------------- term non-action interface funcs --------------------- */

void term_set_filter_func(term_filter_func func, void *ctx)
{
   filter = func;
   filter_ctx = ctx;
}

bool term_is_initialized(void)
{
   return term_initialized;
}

void
init_term(const video_interface *intf,
          int rows,
          int cols,
          u8 default_color,
          bool use_serial_port)
{
   ASSERT(!are_interrupts_enabled());

   term_use_serial = use_serial_port;

   vi = intf;
   term_cols = cols;
   term_rows = rows;

   ringbuf_init(&term_ringbuf,
                ARRAY_SIZE(term_actions_buf),
                sizeof(term_action),
                term_actions_buf);

   if (!in_panic()) {
      extra_buffer_rows = 9 * term_rows;
      total_buffer_rows = term_rows + extra_buffer_rows;

      if (kmalloc_initialized)
         buffer = kmalloc(2 * total_buffer_rows * term_cols);
   }

   if (buffer) {

      term_tabs = kzmalloc(term_cols * term_rows);

      if (!term_tabs)
         printk("WARNING: unable to allocate the term_tabs buffer\n");

   } else {

      /* We're in panic or we were unable to allocate the buffer */
      term_cols = MIN(80, term_cols);
      term_rows = MIN(25, term_rows);

      extra_buffer_rows = 0;
      total_buffer_rows = term_rows;
      buffer = failsafe_buffer;

      if (!in_panic())
         printk("ERROR: unable to allocate the term buffer.\n");
   }

   vi->enable_cursor();
   term_action_move_ch_and_cur(0, 0);
   term_action_set_color(default_color);

   for (int i = 0; i < term_rows; i++)
      ts_clear_row(i, default_color);

   term_initialized = true;
   printk_flush_ringbuf();
}
