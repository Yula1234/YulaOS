#include <drivers/vga.h>
#include <kernel/proc.h>
#include <lib/string.h>
#include <hal/lock.h>
#include <mm/heap.h>

#include "window.h"

window_t window_list[MAX_WINDOWS];
int window_z_order[MAX_WINDOWS];
int focused_window_pid = 0;

static uint32_t active_gradient[28];
static uint32_t inactive_gradient[28];
static spinlock_t window_lock;


void window_precompute_gradients() {
    uint32_t a1 = 0x3E3E42, a2 = 0x2D2D30;
    uint32_t i1 = 0x2D2D2D, i2 = 0x1E1E1E;

    for (int i = 0; i < 28; i++) {
        uint8_t r_a = (((a1 >> 16) & 0xFF) * (28 - i) + ((a2 >> 16) & 0xFF) * i) / 28;
        uint8_t g_a = (((a1 >> 8) & 0xFF) * (28 - i) + ((a2 >> 8) & 0xFF) * i) / 28;
        uint8_t b_a = ((a1 & 0xFF) * (28 - i) + (a2 & 0xFF) * i) / 28;
        active_gradient[i] = (r_a << 16) | (g_a << 8) | b_a;

        uint8_t r_i = (((i1 >> 16) & 0xFF) * (28 - i) + ((i2 >> 16) & 0xFF) * i) / 28;
        uint8_t g_i = (((i1 >> 8) & 0xFF) * (28 - i) + ((i2 >> 8) & 0xFF) * i) / 28;
        uint8_t b_i = ((i1 & 0xFF) * (28 - i) + (i2 & 0xFF) * i) / 28;
        inactive_gradient[i] = (r_i << 16) | (g_i << 8) | b_i;
    }
}


void window_init_system() {
    memset(window_list, 0, sizeof(window_list));
    for(int i = 0; i < MAX_WINDOWS; i++) window_z_order[i] = -1;
    spinlock_init(&window_lock);
    window_precompute_gradients();
}

void window_push_event(window_t* win, int type, int a1, int a2, int a3) {
    int next = (win->evt_head + 1) % MAX_WIN_EVENTS;
    if (next != win->evt_tail) {
        win->event_queue[win->evt_head].type = type;
        win->event_queue[win->evt_head].arg1 = a1;
        win->event_queue[win->evt_head].arg2 = a2;
        win->event_queue[win->evt_head].arg3 = a3;
        win->evt_head = next;
    }
}

int window_pop_event(window_t* win, yula_event_t* out_ev) {
    if (win->evt_head == win->evt_tail) return 0;
    
    *out_ev = win->event_queue[win->evt_tail];
    win->evt_tail = (win->evt_tail + 1) % MAX_WIN_EVENTS;
    return 1;
}

static void window_bring_to_front_nolock(int window_index) {
    int pos = -1;
    for(int i = 0; i < MAX_WINDOWS; i++) {
        if(window_z_order[i] == window_index) {
            pos = i;
            break;
        }
    }
    if(pos == -1) return;

    for(int i = pos; i < MAX_WINDOWS - 1; i++) {
        window_z_order[i] = window_z_order[i+1];
    }
    window_z_order[MAX_WINDOWS - 1] = window_index;
    
    focused_window_pid = window_list[window_index].focused_pid;
}

void window_bring_to_front(int window_index) {
    uint32_t flags = spinlock_acquire_safe(&window_lock);
    window_bring_to_front_nolock(window_index);
    spinlock_release_safe(&window_lock, flags);
}

window_t* window_create(int x, int y, int w, int h, const char* title, window_draw_handler_t handler) {
    uint32_t flags = spinlock_acquire_safe(&window_lock);
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!window_list[i].is_active) {
            
            int canvas_w = w - 12; 
            int canvas_h = h - 44;
            
            if (canvas_w <= 0 || canvas_h <= 0) {
                spinlock_release_safe(&window_lock, flags);
                return 0;
            }

            window_list[i].canvas = (uint32_t*)kmalloc_a(canvas_w * canvas_h * sizeof(uint32_t));
            if (window_list[i].canvas) memset(window_list[i].canvas, 0xFF, canvas_w * canvas_h * 4);
            
            if (window_list[i].canvas) {
                int limit = canvas_w * canvas_h;
                for(int j = 0; j < limit; j++) {
                    window_list[i].canvas[j] = 0x1E1E1E;
                }
            }
            window_list[i].is_dirty = 1;
            
            window_list[i].target_x = x;
            window_list[i].target_y = y;
            window_list[i].target_w = w;
            window_list[i].target_h = h;

            window_list[i].w = 30; 
            window_list[i].h = 30;
            window_list[i].x = x + (w / 2) - 15;
            window_list[i].y = y + (h / 2) - 15;

            window_list[i].is_animating = 1;
            window_list[i].anim_mode = 0;

            window_list[i].on_draw = handler;
            window_list[i].is_active = 1;
            window_list[i].is_minimized = 0;

            window_list[i].evt_head = 0;
            window_list[i].evt_tail = 0;

            strlcpy(window_list[i].title, title, 32);
            
            task_t* curr = proc_current();
            window_list[i].owner_pid = curr ? curr->pid : 0;
            window_list[i].focused_pid = window_list[i].owner_pid;

            for(int j = 0; j < MAX_WINDOWS; j++) {
                if(window_z_order[j] == -1) {
                    window_z_order[j] = i;
                    window_bring_to_front_nolock(i);
                    break;
                }
            }
            spinlock_release_safe(&window_lock, flags);
            return &window_list[i];
        }
    }
    spinlock_release_safe(&window_lock, flags);
    return 0;
}

void window_mark_dirty_by_pid(int pid) {
    uint32_t flags = spinlock_acquire_safe(&window_lock);
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (window_list[i].is_active && window_list[i].owner_pid == pid) {
            window_list[i].is_dirty = 1;
        }
    }
    spinlock_release_safe(&window_lock, flags);
}

void window_draw_all() {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        int idx = window_z_order[i];
        if (idx == -1) continue;
        window_t* win = &window_list[idx];

        int showing_anim = (win->is_animating && win->anim_mode == 1);
        if (!win->is_active || (win->is_minimized && !showing_anim)) continue;

        vga_set_target(0, 0, 0);

        if (!win->is_animating) {
            vga_draw_rect_alpha(win->x + 5, win->y + 5, win->w, win->h, 0x000000, 110);
        }

        vga_draw_rect(win->x, win->y, win->w, win->h, 0x1E1E1E);

        if (win->h >= 28) {
            uint32_t* cache = (focused_window_pid == win->owner_pid) ? active_gradient : inactive_gradient;
            for (int j = 0; j < 28; j++) {
                vga_draw_rect(win->x, win->y + j, win->w, 1, cache[j]);
            }
            
            uint32_t accent = (focused_window_pid == win->owner_pid) ? 0x007ACC : 0x444444;
            vga_draw_rect(win->x, win->y, win->w, 1, accent);

            if (win->w > 60) {
                vga_print_at(win->title, win->x + 10, win->y + 9, 0xD4D4D4);
                
                vga_print_at("_", win->x + win->w - 42, win->y + 5, 0xAAAAAA);
                
                vga_print_at("x", win->x + win->w - 20, win->y + 9, 0xAAAAAA);
            }
        }

        if (!win->is_animating && !win->is_minimized) {
            int cw = win->target_w - 12;
            int ch = win->target_h - 44;

            if (win->is_dirty && win->on_draw) {
                vga_set_target(win->canvas, cw, ch);
                vga_clear(0x1E1E1E);
                win->on_draw(win, 0, 0);
                vga_set_target(0, 0, 0);
                win->is_dirty = 0;
            }

            vga_blit_canvas(win->x + 6, win->y + 34, win->canvas, cw, ch);
        }
    }
    
    vga_set_target(0, 0, 0);
}

void window_close_all_by_pid(int pid) {
    uint32_t flags = spinlock_acquire_safe(&window_lock);

    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (window_list[i].is_active && window_list[i].owner_pid == pid) {
            
            if (window_list[i].canvas) {
                kfree(window_list[i].canvas);
                window_list[i].canvas = 0;
            }

            if (window_list[i].on_close) {
                window_list[i].on_close(&window_list[i]);
            }

            window_list[i].is_active = 0;
            window_list[i].on_draw = 0;
            window_list[i].user_data = 0; 

            vga_mark_dirty(window_list[i].x - 10, window_list[i].y - 10, 
                           window_list[i].w + 25, window_list[i].h + 25);
            
            int z_idx = -1;
            for (int j = 0; j < MAX_WINDOWS; j++) {
                if (window_z_order[j] == i) {
                    z_idx = j;
                    break;
                }
            }

            if (z_idx != -1) {
                for (int j = z_idx; j < MAX_WINDOWS - 1; j++) {
                    window_z_order[j] = window_z_order[j + 1];
                }
                window_z_order[MAX_WINDOWS - 1] = -1;
            }
        }
    }

    if (focused_window_pid == pid) {
        focused_window_pid = 0;
        for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
            int idx = window_z_order[i];
            if (idx != -1 && window_list[idx].is_active) {
                focused_window_pid = window_list[idx].owner_pid;
                break;
            }
        }
    }

    spinlock_release_safe(&window_lock, flags);
}