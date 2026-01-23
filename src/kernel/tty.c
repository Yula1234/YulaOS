#include <kernel/tty.h>

#include <hal/lock.h>
 #include <drivers/fbdev.h>

extern uint32_t fb_width;
extern uint32_t fb_height;

static spinlock_t tty_lock;
static term_instance_t* tty_term;

void tty_set_terminal(term_instance_t* term) {
    uint32_t flags = spinlock_acquire_safe(&tty_lock);
    tty_term = term;
    spinlock_release_safe(&tty_lock, flags);
}

static void tty_render_once(term_instance_t* term) {
    if (!term) return;

    vga_set_target(0, 0, 0);

    uint32_t bg = term->curr_bg;
    vga_draw_rect(0, 0, (int)fb_width, (int)fb_height, bg);

    vga_render_terminal_instance(term, 0, 0);

    int cols = term->cols;
    if (cols <= 0) cols = TERM_W;

    int view_rows = term->view_rows;
    if (view_rows <= 0) view_rows = TERM_H;

    int rel_row = term->row - term->view_row;
    if (rel_row >= 0 && rel_row < view_rows) {
        int cx = term->col;
        if (cx < 0) cx = 0;
        if (cx >= cols) cx = cols - 1;
        vga_draw_rect(cx * 8, rel_row * 16 + 14, 8, 2, COLOR_LIGHT_GREEN);
    }

    vga_mark_dirty(0, 0, (int)fb_width, (int)fb_height);
    vga_flip_dirty();
    vga_reset_dirty();
}

static void tty_render_fallback(void) {
    vga_set_target(0, 0, 0);
    vga_draw_rect(0, 0, (int)fb_width, (int)fb_height, 0x000000);
    vga_print_at("TTY: waiting for shell...", 16, 16, COLOR_LIGHT_GREY);
    vga_mark_dirty(0, 0, (int)fb_width, (int)fb_height);
    vga_flip_dirty();
    vga_reset_dirty();
}

void tty_task(void* arg) {
    (void)arg;

    spinlock_init(&tty_lock);

    while (1) {
        if (!fb_kernel_can_render()) {
            __asm__ volatile("int $0x80" : : "a"(11), "b"(10000));
            continue;
        }

        uint32_t flags = spinlock_acquire_safe(&tty_lock);
        term_instance_t* term = tty_term;
        spinlock_release_safe(&tty_lock, flags);

        if (term) {
            spinlock_acquire(&term->lock);
            tty_render_once(term);
            spinlock_release(&term->lock);
        } else {
            tty_render_fallback();
        }

        __asm__ volatile("int $0x80" : : "a"(11), "b"(10000));
    }
}
