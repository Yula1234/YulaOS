#ifndef KERNEL_TERM_INTERNAL_H
#define KERNEL_TERM_INTERNAL_H

#include <hal/lock.h>
#include <stdint.h>

#define TERM_W 80
#define TERM_H 12

#ifdef __cplusplus
extern "C" {
#endif

typedef struct term_instance_t {
    char* buffer;

    uint32_t* fg_colors;
    uint32_t* bg_colors;

    uint64_t seq;
    uint64_t view_seq;

    int history_cap_rows;
    int history_rows;

    uint8_t* dirty_rows;
    int* dirty_x1;
    int* dirty_x2;
    int full_redraw;

    uint32_t curr_fg;
    uint32_t curr_bg;
    uint32_t def_fg;
    uint32_t def_bg;

    int cols;
    int view_rows;

    int col;
    int row;
    int view_row;
    int max_row;

    int saved_col;
    int saved_row;
    int esc_state;
    int csi_in_param;
    int csi_param_value;
    int csi_param_count;
    int csi_params[8];
    int ansi_bright;
    int ansi_inverse;

    spinlock_t lock;
} term_instance_t;

void term_init(term_instance_t* term);
void term_destroy(term_instance_t* term);
void term_putc(term_instance_t* term, char c);
void term_write(term_instance_t* term, const char* buf, uint32_t len);
void term_print(term_instance_t* term, const char* s);
void term_reflow(term_instance_t* term, int new_cols);
void term_clear_row(term_instance_t* term, int row);
void term_get_cell(term_instance_t* term, int row, int col, char* out_ch, uint32_t* out_fg, uint32_t* out_bg);
void term_set_cell(term_instance_t* term, int row, int col, char ch, uint32_t fg, uint32_t bg);

void term_invalidate_view(term_instance_t* term);

int term_dirty_extract_visible(term_instance_t* term, uint8_t* out_rows, int* out_x1, int* out_x2, int out_rows_cap, int* out_full_redraw);

void term_print_u32(term_instance_t* term, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif
