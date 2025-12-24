#include <drivers/vga.h>
#include <lib/string.h>

#include <mm/heap.h>
#include <mm/pmm.h>

#include "monitor_task.h"
#include "window.h"
#include "proc.h"

extern volatile uint32_t timer_ticks;
extern uint32_t heap_current_limit;

#define C_PANEL_BG   0x252526 
#define C_ACCENT     0x007ACC 
#define C_TEXT_MAIN  0xD4D4D4 
#define C_TEXT_DIM   0x808080 
#define C_BAR_BG     0x111111 
#define C_GREEN      0x4EC9B0 
#define C_ORANGE     0xCE9178 
#define C_RED        0xF44747 

#define HISTORY_MAX 52
#define HEAP_BASE_ADDR 0xD0000000

typedef struct {
    uint32_t history[HISTORY_MAX];
    int history_idx;
    uint32_t last_tick;
} monitor_internal_t;

static char* itoa(uint32_t n) {
    static char buf[16];
    int i = 14; buf[15] = '\0';
    if (n == 0) return "0";
    while (n > 0 && i > 0) { buf[i--] = (n % 10) + '0'; n /= 10; }
    return &buf[i + 1];
}

static void draw_stat_bar(int x, int y, int w, int h, int val, int max, uint32_t color) {
    vga_draw_rect(x, y, w, h, C_BAR_BG);
    if (max <= 0) return;
    int fill_w = (val * (w - 2)) / max;
    if (fill_w > w - 2) fill_w = w - 2;
    if (fill_w < 0) fill_w = 0;
    vga_draw_rect(x + 1, y + 1, fill_w, h - 2, color);
}

static void draw_section_frame(int x, int y, int w, int h, const char* title) {
    vga_draw_rect(x, y, w, h, C_PANEL_BG);
    vga_draw_rect(x, y, w, 1, 0x3d3d3d); 
    vga_print_at(title, x + 5, y - 10, C_ACCENT);
}

static void monitor_cleanup(window_t* win) {
    if (win->user_data) {
        kfree(win->user_data);
    }
}

static void monitor_draw_handler(window_t* self, int x, int y) {
    monitor_internal_t* internal = (monitor_internal_t*)self->user_data;
    if (!internal) return;

    extern volatile uint32_t system_uptime_seconds;
    uint32_t sec = system_uptime_seconds; 
    uint32_t h = sec / 3600;
    uint32_t m = (sec % 3600) / 60;
    uint32_t s = sec % 60;

    vga_print_at("KERNEL UPTIME:", x, y, C_TEXT_DIM);
    vga_print_at(itoa(h), x + 115, y, C_TEXT_MAIN); vga_print_at("h", x + 130, y, C_TEXT_DIM);
    vga_print_at(itoa(m), x + 150, y, C_TEXT_MAIN); vga_print_at("m", x + 165, y, C_TEXT_DIM);
    vga_print_at(itoa(s), x + 185, y, C_TEXT_MAIN); vga_print_at("s", x + 200, y, C_TEXT_DIM);

    draw_section_frame(x, y + 30, 270, 90, "PHYSICAL RAM");
    
    uint32_t u_blocks = pmm_get_used_blocks();
    uint32_t f_blocks = pmm_get_free_blocks();
    uint32_t total_kb = (u_blocks + f_blocks) * 4;
    uint32_t used_kb  = u_blocks * 4;
    uint32_t load_pct = (total_kb > 0) ? (used_kb * 100) / total_kb : 0;

    vga_draw_rect(x + 10, y + 42, 106, 45, 0x0F0F0F);
    for (int i = 0; i < HISTORY_MAX - 1; i++) {
        int idx = (internal->history_idx + i) % HISTORY_MAX;
        int h_val = (internal->history[idx] * 40) / 100;
        if (h_val > 40) h_val = 40;
        vga_draw_rect(x + 12 + (i * 2), y + 85 - h_val, 1, h_val, C_GREEN);
    }
    vga_print_at("Load 5s", x + 10, y + 90, 0x444444);

    int tx = x + 130;
    vga_print_at("Load:", tx, y + 42, C_TEXT_DIM);
    vga_print_at(itoa(load_pct), tx + 50, y + 42, (load_pct > 80) ? C_RED : C_GREEN);
    vga_print_at("%", tx + 80, y + 42, C_TEXT_DIM);

    vga_print_at("Used:", tx, y + 58, C_TEXT_DIM);
    vga_print_at(itoa(used_kb), tx + 50, y + 58, C_TEXT_MAIN);
    vga_print_at("KB", tx + 105, y + 58, 0x444444);

    vga_print_at("Free:", tx, y + 74, C_TEXT_DIM);
    vga_print_at(itoa(f_blocks * 4), tx + 50, y + 74, C_TEXT_MAIN);

    draw_section_frame(x, y + 140, 270, 45, "KERNEL VIRTUAL HEAP");
    uint32_t heap_committed = (heap_current_limit - HEAP_BASE_ADDR);
    
    vga_print_at("Committed:", x + 10, y + 152, C_TEXT_DIM);
    vga_print_at(itoa(heap_committed / 1024), x + 100, y + 152, C_ORANGE);
    vga_print_at("KB", x + 160, y + 152, 0x444444);
    
    draw_stat_bar(x + 10, y + 170, 250, 7, heap_committed / 1024, 32768, C_ORANGE);

    vga_print_at("ID   TASK NAME         MEM      STATUS", x + 5, y + 200, C_ACCENT);
    vga_draw_rect(x + 5, y + 210, 305, 1, 0x333333);

    int row = 0;
    for (uint32_t i = 0; i < proc_task_count(); i++) {
        task_t* t = proc_task_at(i);
        if (t && row < 15) {
            int ry = y + 220 + (row * 13);
            vga_print_at(itoa(t->pid), x + 5, ry, C_TEXT_DIM);
            vga_print_at(t->name, x + 40, ry, C_TEXT_MAIN);
            vga_print_at(itoa(t->mem_pages * 4), x + 180, ry, C_GREEN);
            vga_print_at("KB", x + 215, ry, 0x444444);
            
            const char* status = "WAIT";
            uint32_t s_col = C_ORANGE;
            if (t->state == TASK_RUNNING) { status = "ACTIVE"; s_col = C_GREEN; }
            vga_print_at(status, x + 250, ry, s_col);
            
            row++;
        }
    }
}

void monitor_task(void* arg) {
    (void)arg;

    monitor_internal_t* internal = kmalloc(sizeof(monitor_internal_t));
    if (!internal) return;
    memset(internal, 0, sizeof(monitor_internal_t));

    window_t* win = window_create(180, 50, 320, 430, "System Architecture Monitor", monitor_draw_handler);
    if (!win) {
        kfree(internal);
        return;
    }
    win->user_data = internal;
    win->on_close = monitor_cleanup;

    while (win->is_active) {
        uint32_t u = pmm_get_used_blocks();
        uint32_t f = pmm_get_free_blocks();
        uint32_t pct = (u * 50) / (u + f);
        
        internal->history[internal->history_idx] = pct;
        internal->history_idx = (internal->history_idx + 1) % HISTORY_MAX;

        win->is_dirty = 1;
        __asm__ volatile("int $0x80" : : "a"(7), "b"(50));
    }
    
    win->on_close = 0;
    kfree(internal);
}