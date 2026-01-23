// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <drivers/vga.h>
#include <lib/string.h>
#include <kernel/cpu.h>
#include <kernel/proc.h>
#include <mm/heap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <arch/i386/idt.h>
#include <kernel/gui_task.h>

#include "monitor_task.h"
#include "window.h"

#define C_BG            0x1E1E1E 
#define C_PANEL_BG      0x252526 
#define C_HEADER_BG     0x333333 
#define C_BORDER        0x3F3F46 

#define C_TEXT_MAIN     0xE0E0E0 
#define C_TEXT_DIM      0x808080 
#define C_TEXT_ACCENT   0x4EC9B0 
#define C_TEXT_WARN     0xF44747

#define C_GRAPH_BG      0x111111 
#define C_CPU_GRAPH     0x4EC9B0 
#define C_RAM_GRAPH     0xCE9178 

#define MAX_VISIBLE_CPUS 8
#define HISTORY_MAX      256     

typedef struct {
    uint8_t cpu_history[MAX_VISIBLE_CPUS][HISTORY_MAX];
    uint8_t ram_history[HISTORY_MAX];
    int head_idx;
} monitor_state_t;

extern volatile uint32_t system_uptime_seconds;
extern volatile uint32_t timer_ticks;
extern volatile int ap_running_count;

static char* itoa_p(uint32_t n) {
    static char buf[16];
    int i = 14; buf[15] = 0;
    if (n == 0) return "0";
    while (n > 0 && i > 0) { buf[i--] = (n % 10) + '0'; n /= 10; }
    return &buf[i + 1];
}

static void draw_chart(int x, int y, int w, int h, uint8_t* data, int head, uint32_t color) {
    vga_draw_rect(x, y, w, h, C_GRAPH_BG);
    vga_draw_rect(x, y, w, 1, C_BORDER);
    vga_draw_rect(x, y + h - 1, w, 1, C_BORDER);
    vga_draw_rect(x, y, 1, h, C_BORDER);
    vga_draw_rect(x + w - 1, y, 1, h, C_BORDER);

    int usable_w = w - 2;
    int usable_h = h - 2;
    int start_x = x + w - 2;
    int bottom_y = y + h - 2;

    int limit = (usable_w > HISTORY_MAX) ? HISTORY_MAX : usable_w;

    for (int i = 0; i < limit; i++) {
        int data_idx = (head - 1 - i);
        while (data_idx < 0) data_idx += HISTORY_MAX;
        
        int val = data[data_idx];
        if (val > 100) val = 100;

        int bar_h = (val * usable_h) / 100;
        
        if (bar_h == 0 && val > 0) bar_h = 1;

        if (bar_h > 0) {
            vga_draw_rect(start_x - i, bottom_y - bar_h + 1, 1, bar_h, color);
        }
    }
}

static void monitor_cleanup(window_t* win) {
    if (win->user_data) kfree(win->user_data);
}

static void monitor_draw(window_t* win, int x, int y) {
    monitor_state_t* st = (monitor_state_t*)win->user_data;
    if (!st) return;

    int w = win->target_w - 12;
    int h = win->target_h - 44;

    vga_draw_rect(x, y, w, h, C_BG);

    int left_w = (w * 45) / 100;
    int right_x = x + left_w + 10;
    int right_w = w - left_w - 10;

    int cur_y = y + 10;

    vga_print_at("CPU HISTORY", x + 10, cur_y, C_TEXT_DIM);
    cur_y += 20;

    int total_cpus = ap_running_count + 1;

    for (int i = 0; i < total_cpus && i < MAX_VISIBLE_CPUS; i++) {
        int usage = cpus[i].load_percent;

        st->cpu_history[i][st->head_idx] = (uint8_t)usage;

        char label[16];
        strlcpy(label, "CPU ", 16); strlcat(label, itoa_p(i), 16);
        vga_print_at(label, x + 10, cur_y, C_TEXT_MAIN);

        char pct[8];
        strlcpy(pct, itoa_p(usage), 8); strlcat(pct, "%", 8);
        vga_print_at(pct, x + left_w - 30, cur_y, (usage > 80) ? C_TEXT_WARN : C_TEXT_MAIN);

        draw_chart(x + 10, cur_y + 12, left_w - 10, 24, st->cpu_history[i], st->head_idx, C_CPU_GRAPH);
        
        cur_y += 45;
    }

    cur_y += 10;
    
    uint32_t used_mem = pmm_get_used_blocks() * 4;
    uint32_t total_mem = pmm_get_total_blocks() * 4;
    int mem_pct = (total_mem > 0) ? (used_mem * 100) / total_mem : 0;
    
    st->ram_history[st->head_idx] = (uint8_t)mem_pct;

    vga_print_at("MEMORY USAGE", x + 10, cur_y, C_TEXT_DIM);
    
    char mem_s[64];
    char* m = itoa_p(used_mem / 1024);
    int mi = 0; while(m[mi]) { mem_s[mi] = m[mi]; mi++; }
    
    mem_s[mi++] = ' '; mem_s[mi++] = '/'; mem_s[mi++] = ' ';
    
    char* t = itoa_p(total_mem / 1024);
    int ti = 0; while(t[ti]) { mem_s[mi++] = t[ti++]; }
    
    mem_s[mi++] = ' '; mem_s[mi++] = 'M'; mem_s[mi++] = 'B'; mem_s[mi] = 0;
    
    vga_print_at(mem_s, x + left_w - (mi * 8), cur_y, C_TEXT_MAIN);
    
    draw_chart(x + 10, cur_y + 12, left_w - 10, 24, st->ram_history, st->head_idx, C_RAM_GRAPH);
    
    int ov_y = y + h - 60;
    vga_draw_rect(x + 10, ov_y - 10, left_w - 10, 1, C_BORDER);
    
    vga_print_at("SYSTEM UPTIME", x + 10, ov_y, C_TEXT_DIM);
    char up_s[32];
    strlcpy(up_s, itoa_p(system_uptime_seconds), 32); strlcat(up_s, " sec", 32);
    vga_print_at(up_s, x + left_w - 70, ov_y, C_TEXT_ACCENT);
    
    ov_y += 16;
    vga_print_at("TASKS RUNNING", x + 10, ov_y, C_TEXT_DIM);
    vga_print_at(itoa_p(proc_task_count()), x + left_w - 70, ov_y, C_TEXT_MAIN);

    int tbl_y = y + 10;
    vga_print_at("PROCESSES", right_x, tbl_y, C_TEXT_DIM);
    tbl_y += 20;

    vga_draw_rect(right_x, tbl_y, right_w, 20, C_HEADER_BG);
    vga_print_at("ID",   right_x + 5,   tbl_y + 5, C_TEXT_MAIN);
    vga_print_at("NAME", right_x + 40,  tbl_y + 5, C_TEXT_MAIN);
    vga_print_at("CPU",  right_x + 140, tbl_y + 5, C_TEXT_MAIN);
    vga_print_at("MEM",  right_x + 180, tbl_y + 5, C_TEXT_MAIN);
    
    tbl_y += 20;

    int row_h = 18;
    int max_rows = (h - (tbl_y - y)) / row_h;
    int printed = 0;
    
    uint32_t t_count = proc_task_count();
    
    for (uint32_t i = 0; i < t_count; i++) {
        task_t* t = proc_task_at(i);
        if (!t || t->state == TASK_UNUSED || t->state == TASK_ZOMBIE) continue;
        
        if (printed >= max_rows) break;

        int ry = tbl_y + (printed * row_h);
        
        if (printed % 2 == 0) vga_draw_rect(right_x, ry, right_w, row_h, C_PANEL_BG);
        else vga_draw_rect(right_x, ry, right_w, row_h, C_BG);

        vga_print_at(itoa_p(t->pid), right_x + 5, ry + 4, C_TEXT_DIM);
        
        char name_buf[16];
        strlcpy(name_buf, t->name, 12);
        uint32_t name_col = (t->pid < 5) ? C_TEXT_ACCENT : C_TEXT_MAIN;
        vga_print_at(name_buf, right_x + 40, ry + 4, name_col);
        
        char cpu_s[8];
        strlcpy(cpu_s, " ", 8);
        strlcat(cpu_s, itoa_p(t->assigned_cpu), 8);
        vga_print_at(cpu_s, right_x + 145, ry + 4, C_TEXT_DIM);
        
        char mem_sz[16];
        uint32_t total_mem_kb = (t->mem_pages * 4) + (t->kstack_size / 1024);
        strlcpy(mem_sz, itoa_p(total_mem_kb), 16);
        strlcat(mem_sz, "K", 16);
        vga_print_at(mem_sz, right_x + 180, ry + 4, C_TEXT_DIM);

        printed++;
    }
    
    vga_draw_rect(right_x - 10, y + 10, 1, h - 20, C_BORDER);
}

void monitor_task(void* arg) {
    (void)arg;

    monitor_state_t* st = (monitor_state_t*)kmalloc(sizeof(monitor_state_t));
    if (!st) return;
    memset(st, 0, sizeof(monitor_state_t));

    window_t* win = window_create(100, 80, 600, 450, "System Architecture Monitor", monitor_draw);
    if (!win) { kfree(st); return; }
    
    win->user_data = st;
    win->on_close = monitor_cleanup;

    yula_event_t ev;
    while (win->is_active) {
        while (window_pop_event(win, &ev)); 

        st->head_idx = (st->head_idx + 1) % HISTORY_MAX;
        
        win->is_dirty = 1;
        wake_up_gui();
        
        __asm__ volatile("int $0x80" : : "a"(11), "b"(10000));
    }
}