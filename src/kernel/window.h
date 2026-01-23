// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef KERNEL_WINDOW_H
#define KERNEL_WINDOW_H

#include <hal/lock.h>
#include <lib/dlist.h>

#include <stdint.h>

#define YULA_EVENT_NONE       0
#define YULA_EVENT_MOUSE_MOVE 1
#define YULA_EVENT_MOUSE_DOWN 2
#define YULA_EVENT_MOUSE_UP   3
#define YULA_EVENT_KEY_DOWN   4
#define YULA_EVENT_RESIZE     5

typedef struct {
    int type;
    int arg1;
    int arg2;
    int arg3;
} yula_event_t;

#define MAX_WIN_EVENTS 64

struct window;
typedef void (*window_draw_handler_t)(struct window* self, int rel_x, int rel_y);
typedef void (*window_close_handler_t)(struct window* self);

typedef struct window {
    dlist_head_t list;
    
    int window_id;
    
    int x, y, w, h;
    char title[32];
    int is_active;
    int owner_pid;
    void* user_data;
    window_draw_handler_t on_draw;
    window_close_handler_t on_close;
    int focused_pid;
    int is_minimized;

    int is_animating;
    int anim_mode; 
    int target_x, target_y; 
    int target_w, target_h;

    uint32_t* canvas; 
    uint32_t* old_canvas;
    int is_dirty;     

    yula_event_t event_queue[MAX_WIN_EVENTS];
    int evt_head;
    int evt_tail;

    int is_resizing;
    int ghost_w, ghost_h;

    semaphore_t lock;
    spinlock_t event_lock; 

} window_t;

#define MAX_WINDOWS 16

extern dlist_head_t window_list;
extern int focused_window_pid;
extern int next_window_id;

extern semaphore_t window_list_lock;

window_t* window_find_by_id(int window_id);
window_t* window_find_by_pid(int pid);

void window_init_system();
window_t* window_create(int x, int y, int w, int h, const char* title, window_draw_handler_t handler);
void window_draw_all();
void window_bring_to_front(window_t* win);
void window_close_all_by_pid(int pid);
void window_mark_dirty_by_pid(int pid);
void window_mark_dirty_by_pid_pair(int pid1, int pid2);
int window_system_is_ready(void);

void window_push_event(window_t* win, int type, int a1, int a2, int a3);
int window_pop_event(window_t* win, yula_event_t* out_ev);
void window_bring_to_front_nolock(window_t* win);

#endif