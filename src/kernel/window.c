// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <drivers/vga.h>
#include <kernel/proc.h>
#include <kernel/gui_task.h>
#include <lib/string.h>
#include <hal/lock.h>
#include <mm/heap.h>

#include "window.h"

static volatile int window_system_ready = 0;

dlist_head_t window_list;
int focused_window_pid = 0;
int next_window_id = 1;

static uint32_t active_gradient[28];
static uint32_t inactive_gradient[28];

semaphore_t window_list_lock;

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
    dlist_init(&window_list);
    sem_init(&window_list_lock, 1);
    window_precompute_gradients();
    next_window_id = 1;
    window_system_ready = 1;
}

int window_system_is_ready(void) {
    return window_system_ready != 0;
}

void window_push_event(window_t* win, int type, int a1, int a2, int a3) {
    if (!window_system_ready) return;
    uint32_t flags = spinlock_acquire_safe(&win->event_lock);
    
    int next = (win->evt_head + 1) % MAX_WIN_EVENTS;
    if (next != win->evt_tail) {
        win->event_queue[win->evt_head].type = type;
        win->event_queue[win->evt_head].arg1 = a1;
        win->event_queue[win->evt_head].arg2 = a2;
        win->event_queue[win->evt_head].arg3 = a3;
        win->evt_head = next;
    }

    win->is_dirty = 1;
    
    spinlock_release_safe(&win->event_lock, flags);

    wake_up_gui();
}

int window_pop_event(window_t* win, yula_event_t* out_ev) {
    if (!window_system_ready) return 0;
    uint32_t flags = spinlock_acquire_safe(&win->event_lock);
    
    if (win->evt_head == win->evt_tail) {
        spinlock_release_safe(&win->event_lock, flags);
        return 0;
    }
    
    *out_ev = win->event_queue[win->evt_tail];
    win->evt_tail = (win->evt_tail + 1) % MAX_WIN_EVENTS;
    
    spinlock_release_safe(&win->event_lock, flags);
    return 1;
}

window_t* window_find_by_id(int window_id) {
    if (window_id <= 0) return 0;
    if (!window_system_ready) return 0;
    
    sem_wait(&window_list_lock);
    window_t* win;
    window_t* result = 0;
    
    dlist_for_each_entry(win, &window_list, list) {
        if (win->window_id == window_id && win->is_active) {
            result = win;
            break;
        }
    }
    
    sem_signal(&window_list_lock);
    return result;
}

window_t* window_find_by_pid(int pid) {
    if (pid <= 0) return 0;
    if (!window_system_ready) return 0;
    
    sem_wait(&window_list_lock);
    window_t* win;
    window_t* result = 0;
    
    dlist_for_each_entry(win, &window_list, list) {
        if (win->owner_pid == pid && win->is_active) {
            result = win;
            break;
        }
    }
    
    sem_signal(&window_list_lock);
    return result;
}

void window_bring_to_front_nolock(window_t* win) {
    if (!window_system_ready) return;
    if (!win || !win->is_active) return;
    dlist_del(&win->list);
    dlist_add_tail(&win->list, &window_list);
    focused_window_pid = win->focused_pid;
}

void window_bring_to_front(window_t* win) {
    if (!window_system_ready) return;
    sem_wait(&window_list_lock);
    window_bring_to_front_nolock(win);
    sem_signal(&window_list_lock);
}

window_t* window_create(int x, int y, int w, int h, const char* title, window_draw_handler_t handler) {
    if (!window_system_ready) return 0;
    sem_wait(&window_list_lock);
    
    int count = 0;
    window_t* tmp;
    dlist_for_each_entry(tmp, &window_list, list) {
        if (tmp->is_active) count++;
    }
    
    window_t* win = (window_t*)kmalloc(sizeof(window_t));
    if (!win) {
        sem_signal(&window_list_lock);
        return 0;
    }
    
    memset(win, 0, sizeof(window_t));
    
    sem_init(&win->lock, 1);
    spinlock_init(&win->event_lock);
    dlist_init(&win->list);
    
    sem_wait(&win->lock);

    int canvas_w = w - 12; 
    int canvas_h = h - 44;
    
    if (canvas_w <= 0 || canvas_h <= 0) {
        sem_signal(&win->lock);
        sem_signal(&window_list_lock);
        kfree(win);
        return 0;
    }

    win->canvas = (uint32_t*)kmalloc_a(canvas_w * canvas_h * sizeof(uint32_t));
    if (!win->canvas) {
        sem_signal(&win->lock);
        sem_signal(&window_list_lock);
        kfree(win);
        return 0;
    }
    
    int limit = canvas_w * canvas_h;
    for(int j = 0; j < limit; j++) {
        win->canvas[j] = 0x1E1E1E;
    }
    win->is_dirty = 1;
    
    win->target_x = x;
    win->target_y = y;
    win->target_w = w;
    win->target_h = h;

    win->w = 30; 
    win->h = 30;
    win->x = x + (w / 2) - 15;
    win->y = y + (h / 2) - 15;

    win->is_animating = 1;
    win->anim_mode = 0;

    win->on_draw = handler;
    win->is_active = 1;
    win->is_minimized = 0;

    win->evt_head = 0;
    win->evt_tail = 0;

    strlcpy(win->title, title, 32);
    
    task_t* curr = proc_current();
    win->owner_pid = curr ? curr->pid : 0;
    win->focused_pid = win->owner_pid;
    
    if (next_window_id <= 0 || next_window_id >= 0x7FFFFFFF) {
        next_window_id = 1;
    }
    win->window_id = next_window_id++;

    dlist_add_tail(&win->list, &window_list);
    window_bring_to_front_nolock(win);
    
    sem_signal(&win->lock);
    sem_signal(&window_list_lock);
    
    wake_up_gui();
    
    return win;
}

void window_mark_dirty_by_pid(int pid) {
    if (!window_system_ready) return;
    sem_wait(&window_list_lock);
    int found = 0;
    window_t* win;
    dlist_for_each_entry(win, &window_list, list) {
        if (win->is_active && win->owner_pid == pid) {
            win->is_dirty = 1;
            found = 1;
        }
    }
    sem_signal(&window_list_lock);
     
    if (found) {
        wake_up_gui();
    }
}

void window_mark_dirty_by_pid_pair(int pid1, int pid2) {
    if (pid1 <= 0 && pid2 <= 0) return;
    if (!window_system_ready) return;
 
    sem_wait(&window_list_lock);
    int found = 0;
    window_t* win;
    dlist_for_each_entry(win, &window_list, list) {
        if (!win->is_active) continue;

        int owner = win->owner_pid;
        if ((pid1 > 0 && owner == pid1) || (pid2 > 0 && owner == pid2)) {
            win->is_dirty = 1;
            found = 1;
        }
    }
    sem_signal(&window_list_lock);

    if (found) {
        wake_up_gui();
    }
}

void window_draw_all() {
    if (!window_system_ready) return;
    sem_wait(&window_list_lock);
    
    window_t* win;
    dlist_for_each_entry(win, &window_list, list) {

        sem_wait(&win->lock);

        int check_x = win->x - 20;
        int check_y = win->y - 20;
        int check_w = win->w + 40;
        int check_h = win->h + 40;
        
        if (check_x < 0) {
            check_w += check_x;
            check_x = 0;
        }
        if (check_y < 0) {
            check_h += check_y;
            check_y = 0;
        }
        
        if (check_w <= 0 || check_h <= 0 || 
            (!vga_is_rect_dirty(check_x, check_y, check_w, check_h) && !win->is_dirty)) {
            sem_signal(&win->lock);
            continue;
        }

        int showing_anim = (win->is_animating && win->anim_mode == 1);
        
        if (!win->is_active || (win->is_minimized && !showing_anim)) {
            sem_signal(&win->lock);
            continue;
        }

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
                if (win->canvas) {
                    vga_set_target(win->canvas, cw, ch);
                    win->on_draw(win, 0, 0);
                    vga_set_target(0, 0, 0);
                }
                win->is_dirty = 0;
            }

            vga_blit_canvas(win->x + 6, win->y + 34, win->canvas, cw, ch);

            int wx = win->x;
            int wy = win->y;
            int ww = win->w;
            int wh = win->h;
            
            extern uint32_t fb_width;
            extern uint32_t fb_height;

            for(int k=0; k<10; k++) {
                int px1 = wx + ww - 4 - k;
                int py1 = wy + wh - 4;
                int px2 = wx + ww - 4;
                int py2 = wy + wh - 4 - k;

                if (px1 >= 0 && px1 < (int)fb_width && py1 >= 0 && py1 < (int)fb_height) {
                    vga_put_pixel(px1, py1, 0x666666);
                }
                if (px2 >= 0 && px2 < (int)fb_width && py2 >= 0 && py2 < (int)fb_height) {
                    vga_put_pixel(px2, py2, 0x666666);
                }
            }
        }
        
        sem_signal(&win->lock);
    }
    
    vga_set_target(0, 0, 0);
    sem_signal(&window_list_lock);
}

void window_close_all_by_pid(int pid) {
    if (!window_system_ready) return;
    sem_wait(&window_list_lock);

    window_t* win, *n;
    dlist_for_each_entry_safe(win, n, &window_list, list) {
        if (win->is_active && win->owner_pid == pid) {
            
            sem_wait(&win->lock);

            if (win->canvas) {
                kfree(win->canvas);
                win->canvas = 0;
            }

            if (win->old_canvas) {
                kfree(win->old_canvas);
                win->old_canvas = 0;
            }

            if (win->on_close) {
                win->on_close(win);
            }

            vga_set_target(0, 0, 0);
            vga_mark_dirty(win->x - 24, win->y - 24,
                           win->w + 48, win->h + 48);
              
            sem_signal(&win->lock);
            
            dlist_del(&win->list);
            
            win->is_active = 0;
            win->on_draw = 0;
            win->user_data = 0;
            kfree(win);
        }
    }

    if (focused_window_pid == pid) {
        focused_window_pid = 0;
        window_t* last_win = 0;
        window_t* iter;
        dlist_for_each_entry(iter, &window_list, list) {
            if (iter->is_active) {
                last_win = iter;
            }
        }
        if (last_win) {
            focused_window_pid = last_win->owner_pid;
        }
    }

    wake_up_gui();

    sem_signal(&window_list_lock);
}