#include <arch/i386/idt.h>
#include <drivers/vga.h>
#include <kernel/proc.h>
#include <shell/shell.h>
#include <lib/string.h>
#include <hal/io.h>
#include <mm/heap.h>
#include <hal/lock.h> 
#include <kernel/sched.h> // для sem_wait / sem_init

#include "gui_task.h"
#include "window.h"

extern uint32_t fb_width, fb_height;
extern int mouse_x, mouse_y, mouse_buttons;
extern volatile uint32_t timer_ticks;
extern uint32_t* fb_ptr;

static window_t* dragged_window = 0;
static int drag_off_x = 0, drag_off_y = 0;
static int last_mouse_buttons = 0;

extern int dirty_x1, dirty_y1, dirty_x2, dirty_y2;

#define C_TASKBAR_BG    0x000000
#define C_BTN_ACTIVE    0x2D2D2D 
#define C_BTN_MINIMIZED 0x1A1A1A 
#define C_ACCENT_BLUE   0x007ACC 
#define C_CLOSE_RED     0x9A1010 

#define max(a,b) ((a) > (b) ? (a) : (b))

static inline void sys_usleep(uint32_t us) {
    __asm__ volatile("int $0x80" : : "a"(11), "b"(us));
}

void proc_kill_by_pid(int pid);

static void itoa(uint32_t n, char* str) {
    int i = 0;
    if (n == 0) { str[i++] = '0'; str[i] = '\0'; return; }
    while (n > 0) { str[i++] = (n % 10) + '0'; n /= 10; }
    str[i] = '\0';
    for (int j = 0; j < i / 2; j++) {
        char t = str[j]; str[j] = str[i - 1 - j]; str[i - 1 - j] = t;
    }
}

static int is_rtc_updating() { outb(0x70, 0x0A); return (inb(0x71) & 0x80); }
static uint8_t get_rtc_register(int reg) { outb(0x70, reg); return inb(0x71); }

void get_time_string(char* buf) {
    if (is_rtc_updating()) return;
    uint8_t s = get_rtc_register(0x00), m = get_rtc_register(0x02), h = get_rtc_register(0x04);
    s = (s & 0x0F) + ((s / 16) * 10); m = (m & 0x0F) + ((m / 16) * 10); h = (h & 0x0F) + ((h / 16) * 10);
    h = (h + 5) % 24;
    buf[0] = (h / 10) + '0'; buf[1] = (h % 10) + '0'; buf[2] = ':';
    buf[3] = (m / 10) + '0'; buf[4] = (m % 10) + '0'; buf[5] = ':';
    buf[6] = (s / 10) + '0'; buf[7] = (s % 10) + '0'; buf[8] = '\0';
}

static uint8_t last_rtc_sec = 0xFF;

void update_system_uptime() {
    if (is_rtc_updating()) return;
    uint8_t s = get_rtc_register(0x00);
    
    if (s != last_rtc_sec) {
        extern volatile uint32_t system_uptime_seconds;
        system_uptime_seconds++;
        last_rtc_sec = s;
    }
}


extern uint32_t icon_terminal[256];
extern uint32_t icon_monitor[256];
    
typedef struct {
    int x, y, w, h;
    const char* name;
    uint32_t* sprite;
    void (*launch_func)(void*);
    int is_hovered;
    uint32_t last_click_tick;
} desktop_item_t;

extern void shell_task(void* arg);
extern void monitor_task(void* arg);

desktop_item_t desktop_icons[] = {
    {40, 60, 36, 36, "Terminal", icon_terminal, shell_task, 0, 0},
    {120, 60, 36, 36, "Monitor", icon_monitor, monitor_task, 0, 0}
};

#define ICON_COUNT 2

void draw_desktop_icon(desktop_item_t* item) {
    if (item->is_hovered) {
        vga_draw_rect_alpha(item->x - 4, item->y - 4, item->w + 8, item->h + 38, 0x007ACC, 80);
        vga_draw_rect(item->x - 4, item->y - 4, item->w + 8, 1, 0x00AAFF); 
    } else {
        vga_draw_rect_alpha(item->x + 2, item->y + 2, item->w, item->h, 0x000000, 100);
    }

    vga_draw_sprite_scaled_masked(item->x, item->y, 16, 16, 2, item->sprite, 0xFF00FF);
    
    int text_x = item->x + (item->w / 2) - (strlen(item->name) * 4);
    vga_print_at(item->name, text_x, item->y + 38, item->is_hovered ? 0xFFFFFF : 0xCCCCCC);
}

void vga_draw_wireframe(int x, int y, int w, int h, uint32_t color) {
    vga_draw_rect(x, y, w, 1, color);          
    vga_draw_rect(x, y + h - 1, w, 1, color);  
    vga_draw_rect(x, y, 1, h, color);        
    vga_draw_rect(x + w - 1, y, 1, h, color);  
}

semaphore_t gui_event_sem;

void wake_up_gui() {
    sem_signal(&gui_event_sem);
}

void gui_task(void* arg) {
    (void)arg;

    uint32_t frames = 0, last_fps_tick = 0, current_fps = 0;
    char fps_str[16], time_str[16];
    static int old_mx = 0, old_my = 0;

    static uint32_t ticks_per_100ms = 1500;
    static uint32_t last_tick_count = 0;

    int first_frame = 1;
    sem_init(&gui_event_sem, 0);

    vga_reset_dirty();

    while (1) {
        frames++;

        update_system_uptime();

        vga_mark_dirty(old_mx, old_my, 16, 16);
        vga_mark_dirty(mouse_x, mouse_y, 16, 16);

        for (int i = 0; i < MAX_WINDOWS; i++) {
            window_t* win = &window_list[i];
            if (win->is_active && (win->is_animating || dragged_window == win)) {
                vga_mark_dirty(win->x - 8, win->y - 8, win->w + 18, win->h + 18);
            }
        }

        if (first_frame) {
            vga_mark_dirty(0, 0, fb_width, fb_height);
            first_frame = 0;
        }

        if (timer_ticks - last_fps_tick >= ticks_per_100ms) {
            current_fps = frames * 10;
            frames = 0;
            last_fps_tick = timer_ticks;
        }

        if (!is_rtc_updating()) {
            uint8_t s = get_rtc_register(0x00);
            static uint8_t last_s = 0xFF;

            if (s != last_s) {
                uint32_t ticks_passed = timer_ticks - last_tick_count;
                
                if (ticks_passed > 0) {
                    ticks_per_100ms = ticks_passed / 10;
                }

                last_tick_count = timer_ticks;
                last_s = s;
            }
        }

        int active_wins = 0;
        for (int i = 0; i < MAX_WINDOWS; i++) if (window_list[i].is_active) active_wins++;

        int tb_start_x = 100;
        int tb_avail_w = fb_width - 280;
        int btn_w = 130;
        if (active_wins > 0) {
            btn_w = tb_avail_w / active_wins;
            if (btn_w > 130) btn_w = 130;
            if (btn_w < 50)  btn_w = 50;
        }

        int left_click = mouse_buttons & 1;
        static int last_left_click = 0;
        int just_pressed = left_click && !(last_mouse_buttons & 1);

        for (int i = 0; i < ICON_COUNT; i++) {
            desktop_item_t* item = &desktop_icons[i];
            
            int currently_hovered = (mouse_x >= item->x && mouse_x <= item->x + item->w &&
                                     mouse_y >= item->y && mouse_y <= item->y + item->h);
            
            if (currently_hovered != item->is_hovered) {
                item->is_hovered = currently_hovered;
                vga_mark_dirty(item->x - 10, item->y - 10, item->w + 20, item->h + 50);
            }

            if (item->is_hovered) {
                vga_mark_dirty(item->x - 5, item->y - 5, item->w + 10, item->h + 45);
            }

            if (currently_hovered && left_click && !last_left_click) {
                uint32_t current_tick = timer_ticks;
                
                if (current_tick - item->last_click_tick < 7500) {
                    proc_spawn_kthread(item->name, PRIO_USER, item->launch_func, 0);
                    item->last_click_tick = 0;
                } else {
                    item->last_click_tick = current_tick;
                }
            }
        }
        last_left_click = left_click;

        for(int i = 0; i < MAX_WINDOWS; i++) {
            window_t* win = &window_list[i];
            if (win->is_active && win->is_animating) {
                if (win->anim_mode == 0) { 
                    int dx = win->target_x - win->x;
                    int dy = win->target_y - win->y;
                    int dw = win->target_w - win->w;
                    int dh = win->target_h - win->h;

                    if (dx == 0 && dy == 0 && dw == 0 && dh == 0) {
                        win->is_animating = 0;
                    } else {
                        int step_x = dx / 4; if (step_x == 0 && dx != 0) step_x = (dx > 0) ? 1 : -1;
                        int step_y = dy / 4; if (step_y == 0 && dy != 0) step_y = (dy > 0) ? 1 : -1;
                        int step_w = dw / 4; if (step_w == 0 && dw != 0) step_w = (dw > 0) ? 1 : -1;
                        int step_h = dh / 4; if (step_h == 0 && dh != 0) step_h = (dh > 0) ? 1 : -1;

                        win->x += step_x;
                        win->y += step_y;
                        win->w += step_w;
                        win->h += step_h;
                    }
                } else {
                    int dh = win->h - 20;
                    if (dh <= 2) {
                        win->is_animating = 0;
                        win->is_minimized = 1;
                    } else {
                        win->w -= (win->w - 60) / 4 + 1;
                        win->h -= (win->h - 20) / 4 + 1;
                        win->y -= (win->y - 0) / 4 + 1;
                    }
                }
            }
        }
        
        if (just_pressed) {
            int hit = 0;

            if (mouse_y <= 26 && mouse_x >= tb_start_x && mouse_x < tb_start_x + (active_wins * (btn_w + 2))) {
                int clicked_btn_idx = (mouse_x - tb_start_x) / (btn_w + 2);
                int offset_in_btn = (mouse_x - tb_start_x) % (btn_w + 2);
                int current_btn = 0;
                for (int i = 0; i < MAX_WINDOWS; i++) {
                    if (window_list[i].is_active) {
                        if (current_btn == clicked_btn_idx) {
                            if (offset_in_btn > btn_w - 20) {
                                proc_kill_by_pid(window_list[i].owner_pid);
                            } else {
                                if (window_list[i].is_minimized) {
                                    window_list[i].is_minimized = 0;
                                    window_list[i].is_animating = 1;
                                    window_list[i].anim_mode = 0;
                                    window_list[i].y = 0; 
                                    window_bring_to_front(i);
                                } else {
                                    window_list[i].target_x = window_list[i].x;
                                    window_list[i].target_y = window_list[i].y;
                                    window_list[i].is_animating = 1;
                                    window_list[i].anim_mode = 1;
                                }
                            }
                            hit = 1; break;
                        }
                        current_btn++;
                    }
                }
            }

            if (!hit) {
                for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
                    int idx = window_z_order[i];
                    if (idx == -1) continue;
                    window_t* win = &window_list[idx];
                    if (!win->is_active || win->is_minimized) continue;

                    if (mouse_x >= win->x && mouse_x <= win->x + win->w && mouse_y >= win->y && mouse_y <= win->y + win->h) {
                        hit = 1;
                        if (mouse_x >= win->x + win->w - 20 && mouse_y >= win->y + win->h - 20) {
                            window_bring_to_front(idx);
                            dragged_window = win;
                            
                            win->is_resizing = 1;
                            win->ghost_w = win->w;
                            win->ghost_h = win->h;
                            
                        }
                        else if (mouse_x >= win->x + win->w - 26 && mouse_y <= win->y + 26) {
                            vga_mark_dirty(win->x - 10, win->y - 10, win->w + 25, win->h + 25);
                            proc_kill_by_pid(win->owner_pid);
                        } else if (mouse_x >= win->x + win->w - 50 && mouse_x < win->x + win->w - 26 && mouse_y <= win->y + 26) {
                            win->target_x = win->x;
                            win->target_y = win->y;
                            win->is_animating = 1;
                            win->anim_mode = 1;
                            win->is_minimized = 1;
                        } else {
                            window_bring_to_front(idx);
                            if (mouse_y <= win->y + 30) {
                                dragged_window = win; drag_off_x = mouse_x - win->x; drag_off_y = mouse_y - win->y;
                            }
                        }
                        break;
                    }
                }
            }
        }

        if (left_click && dragged_window) {
            if (dragged_window->is_resizing) {
                int new_w = mouse_x - dragged_window->x;
                int new_h = mouse_y - dragged_window->y;

                if (new_w < 150) new_w = 150;
                if (new_h < 100) new_h = 100;

                vga_mark_dirty(dragged_window->x - 5, dragged_window->y - 5, 
                               max(dragged_window->ghost_w, new_w) + 10, 
                               max(dragged_window->ghost_h, new_h) + 10);

                dragged_window->ghost_w = new_w;
                dragged_window->ghost_h = new_h;
            } else {
                int nx = mouse_x - drag_off_x;
                int ny = mouse_y - drag_off_y;

                if (nx < 0) nx = 0;
                if (ny < 26) ny = 26;
                
                vga_mark_dirty(dragged_window->x - 10, dragged_window->y - 10, 
                       dragged_window->w + 25, dragged_window->h + 25);


                if (nx + dragged_window->w > (int)fb_width) {
                    nx = fb_width - dragged_window->w;
                }
                if (ny + dragged_window->h > (int)fb_height) {
                    ny = fb_height - dragged_window->h;
                }

                nx &= ~3;

                dragged_window->x = nx;
                dragged_window->target_x = nx;
                dragged_window->y = ny;
                dragged_window->target_y = ny;

                vga_mark_dirty(dragged_window->x - 10, dragged_window->y - 10, 
                       dragged_window->w + 25, dragged_window->h + 25);
            }
        } else {
            if (dragged_window && dragged_window->is_resizing) {
                int new_w = dragged_window->ghost_w;
                int new_h = dragged_window->ghost_h;
                
                if (dragged_window->canvas) {
                    kfree(dragged_window->canvas);
                }

                int canvas_w = new_w - 12;
                int canvas_h = new_h - 44;

                dragged_window->canvas = (uint32_t*)kmalloc_a(canvas_w * canvas_h * 4);
                
                if (dragged_window->canvas) {
                    memset(dragged_window->canvas, 0x1E, canvas_w * canvas_h * 4);
                }

                dragged_window->w = new_w;
                dragged_window->target_w = new_w;
                dragged_window->h = new_h;
                dragged_window->target_h = new_h;
                
                dragged_window->is_resizing = 0;
                dragged_window->is_dirty = 1;
                
                vga_mark_dirty(dragged_window->x - 10, dragged_window->y - 10, new_w + 20, new_h + 20);
            }
            
            dragged_window = 0;
        }
        
        window_t* hover_win = 0;
        int win_rel_x = 0;
        int win_rel_y = 0;

        for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
            int idx = window_z_order[i];
            if (idx == -1) continue;
            window_t* w = &window_list[idx];
            if (!w->is_active || w->is_minimized) continue;

            if (mouse_x >= w->x && mouse_x < w->x + w->w &&
                mouse_y >= w->y && mouse_y < w->y + w->h) 
            {
                hover_win = w;
                win_rel_x = mouse_x - (w->x + 6);
                win_rel_y = mouse_y - (w->y + 34);
                break;
            }
        }

        if (hover_win) {
            if (mouse_x != old_mx || mouse_y != old_my) {
                if (hover_win != dragged_window) window_push_event(hover_win, YULA_EVENT_MOUSE_MOVE, win_rel_x, win_rel_y, mouse_buttons);
            }
            if (mouse_buttons != last_mouse_buttons) {
                if ((mouse_buttons & 1) != (last_mouse_buttons & 1)) {
                    int type = (mouse_buttons & 1) ? YULA_EVENT_MOUSE_DOWN : YULA_EVENT_MOUSE_UP;
                    window_push_event(hover_win, type, win_rel_x, win_rel_y, 1);
                }
            }
        }

        last_mouse_buttons = mouse_buttons;


        if (dirty_x2 >= dirty_x1) {
            vga_set_target(0, 0, 0);
            int dw = dirty_x2 - dirty_x1;
            int dh = dirty_y2 - dirty_y1;
            vga_draw_rect(dirty_x1, dirty_y1, dw, dh, 0x1A1A1B);
        }

        for (int i = 0; i < ICON_COUNT; i++) {
            draw_desktop_icon(&desktop_icons[i]);
        }

        vga_draw_rect(0, 0, fb_width, 26, C_TASKBAR_BG);
        vga_print_at("yulaOS", 12, 8, C_ACCENT_BLUE);

        int cur_x = tb_start_x;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (window_list[i].is_active) {
                __attribute__((unused)) int is_focused = (focused_window_pid == window_list[i].owner_pid && !window_list[i].is_minimized);
                uint32_t bg = window_list[i].is_minimized ? C_BTN_MINIMIZED : C_BTN_ACTIVE;
                
                vga_draw_rect(cur_x, 1, btn_w, 24, bg);

                uint32_t* icon = icon_terminal;
                if (strcmp(window_list[i].title, "System Architecture Monitor") == 0) {
                    icon = icon_monitor;
                }

                vga_draw_sprite_masked(cur_x + 6, 5, 16, 16, icon, 0xFF00FF);

                char title_short[16];
                strlcpy(title_short, window_list[i].title, 12);
                vga_print_at(title_short, cur_x + 26, 8, 0xCCCCCC);

                cur_x += btn_w + 2;
            }
        }

        itoa(current_fps, fps_str);
        vga_print_at("FPS ", fb_width - 158, 8, 0x00FF00);
        vga_print_at(fps_str, fb_width - 125, 8, 0x00FF00);
        get_time_string(time_str);
        vga_print_at(time_str, fb_width - 80, 8, 0xD4D4D4);


        window_draw_all();

        if (dragged_window && dragged_window->is_resizing) {
            vga_draw_wireframe(dragged_window->x, dragged_window->y, 
                               dragged_window->ghost_w, dragged_window->ghost_h, 
                               0xAAAAAA);
        }

        vga_set_target(0, 0, 0);
        extern uint32_t mouse_cursor_classic[144];
        vga_draw_sprite_masked(mouse_x, mouse_y, 12, 12, mouse_cursor_classic, 0xFF00FF);

        old_mx = mouse_x; old_my = mouse_y;

        vga_flip_dirty();

        vga_reset_dirty(); 

        int any_animations = 0;
        
        for(int i = 0; i < MAX_WINDOWS; i++) {
            if (window_list[i].is_active && window_list[i].is_animating) {
                any_animations = 1;
                break;
            }
        }
        
        extern window_t* dragged_window;
        if (dragged_window) any_animations = 1;

        if (any_animations) {
            sys_usleep(500);
            
            uint32_t flags = spinlock_acquire_safe(&gui_event_sem.lock);
            gui_event_sem.count = 0;
            spinlock_release_safe(&gui_event_sem.lock, flags);
        } else {
            sem_wait(&gui_event_sem);
        }
    }
}

void proc_kill_by_pid(int pid) {
    task_t* t = proc_find_by_pid((uint32_t)pid);
    if (t) {
        proc_kill(t);
    }
}