// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <arch/i386/idt.h>
#include <fs/vfs.h>

#include <kernel/sched.h>
#include <kernel/window.h>
#include <kernel/proc.h>

#include <hal/io.h>
#include <hal/irq.h>
#include <hal/lock.h>


#include "keyboard.h"

static int shift_pressed = 0;
static int ctrl_pressed = 0;
static int e0_flag = 0; 

static const char map_norm[] = { 0,27,'1','2','3','4','5','6','7','8','9','0','-','=', '\b','\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',0,'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c','v','b','n','m',',','.','/',0,'*',0,' ' };
static const char map_shift[] = { 0,27,'!','@','#','$','%','^','&','*','(',')','_','+', '\b','\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,'A','S','D','F','G','H','J','K','L',':','\"','~',0,'|','Z','X','C','V','B','N','M','<','>','?',0,'*',0,' ' };

#define KBD_BUF_SIZE 128
static char kbd_buffer[KBD_BUF_SIZE];
static uint32_t kbd_head = 0, kbd_tail = 0;

static semaphore_t kbd_sem;

static void kbd_put_char(char c) {
    uint32_t next = (kbd_head + 1) % KBD_BUF_SIZE;
    if (next != kbd_tail) {
        kbd_buffer[kbd_head] = c;
        kbd_head = next;
        sem_signal(&kbd_sem); 
    }
}

extern void wake_up_gui();
extern volatile uint32_t timer_ticks;

static void send_key_to_focused(char code) {
    task_t* focused_task = 0;
    
    if (focused_window_pid > 0) {
        focused_task = proc_find_by_pid(focused_window_pid);
        
        window_t* win;
        dlist_for_each_entry(win, &window_list, list) {
            if (win->is_active && win->owner_pid == focused_window_pid) {
                window_push_event(win, YULA_EVENT_KEY_DOWN, code, 0, 0);
                break; 
            }
        }
    }
    
    int should_write_to_buffer = 0;
    
    if (!focused_task) {
        should_write_to_buffer = 1;
    } else {
        if (focused_task->term_mode == 1) {
            should_write_to_buffer = 1;
        }
    }

    if (should_write_to_buffer) {
        kbd_put_char(code);
    } else {
        sem_signal(&kbd_sem); 
    }
    wake_up_gui();
}

void keyboard_irq_handler(registers_t* regs) {
    (void)regs;
    uint8_t scancode = inb(0x60);

    if (scancode == 0xE0) { e0_flag = 1; return; }

    if (e0_flag) {
        e0_flag = 0;
        if (!(scancode & 0x80)) {
            switch (scancode) {
                case 0x4B: // Left
                    if (ctrl_pressed && shift_pressed) send_key_to_focused(0x86); // Shift + Ctrl + Left (NEW)
                    else if (ctrl_pressed) send_key_to_focused(0x84);      // Ctrl + Left
                    else if (shift_pressed) send_key_to_focused(0x82);     // Shift + Left
                    else send_key_to_focused(0x11);
                    break; 

                case 0x4D: // Right
                    if (ctrl_pressed && shift_pressed) send_key_to_focused(0x87); // Shift + Ctrl + Right (NEW)
                    else if (ctrl_pressed) send_key_to_focused(0x85);      // Ctrl + Right
                    else if (shift_pressed) send_key_to_focused(0x83);     // Shift + Right
                    else send_key_to_focused(0x12);
                    break;

                case 0x48: // Up
                    if (shift_pressed) send_key_to_focused(0x80); 
                    else send_key_to_focused(0x13);
                    break;
                case 0x50: // Down
                    if (shift_pressed) send_key_to_focused(0x81); 
                    else send_key_to_focused(0x14); 
                    break;
            }
        }
        return;
    }

    if (scancode == 0x1D) { ctrl_pressed = 1; return; }
    if (scancode == 0x9D) { ctrl_pressed = 0; return; }
    if (scancode == 0x2A || scancode == 0x36) { shift_pressed = 1; return; }
    if (scancode == 0xAA || scancode == 0xB6) { shift_pressed = 0; return; }

    // Ctrl+C
    if (ctrl_pressed && scancode == 0x2E) { 
        task_t* target_task = proc_find_by_pid(focused_window_pid);

        if (target_task) {
            if (target_task->term_mode == 0) {
                send_key_to_focused(0x03); 
                return;
            }

            if (target_task->term_mode == 1) {
                if (target_task->wait_for_pid > 0) {
                    int child_pid = target_task->wait_for_pid;
                    task_t* t = proc_find_by_pid(child_pid);
                    proc_kill(t);
                    target_task->state = TASK_RUNNABLE;
                    target_task->wait_for_pid = 0;
                } else {
                    target_task->pending_signals |= (1 << 2);
                    if (target_task->state == TASK_WAITING) target_task->state = TASK_RUNNABLE;
                }
                send_key_to_focused(0x03); 
            } 
        }
        return;
    }

    if (!(scancode & 0x80)) {
        if (ctrl_pressed) {
            if (scancode == 0x1F) { send_key_to_focused(0x15); return; } // Ctrl+S
            if (scancode == 0x10) { send_key_to_focused(0x17); return; } // Ctrl+Q
            if (scancode == 0x2F) { send_key_to_focused(0x16); return; } // Ctrl+V
        }

        char c = shift_pressed ? map_shift[scancode] : map_norm[scancode];
        if (c) {
            send_key_to_focused(c);
        }
    }
}

int kbd_try_read_char(char* out) {
    if (kbd_head == kbd_tail) return 0; 
    *out = kbd_buffer[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    return 1;
}

void kbd_reboot(void) {
    uint8_t s = inb(0x64);
    while (s & 2) s = inb(0x64);
    outb(0x64, 0xFE);
    for (;;) cpu_hlt();
}

static int kbd_vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, void* buffer) {
    (void)node; (void)offset; (void)size;
    char* b = (char*)buffer;
    task_t* curr = proc_current();

    while (1) {
        if (curr->pending_signals & (1 << 2)) { // SIGINT
            curr->pending_signals &= ~(1 << 2);
            return -2; // EINTR
        }

        while (1) {
            task_t* focused_task = 0;
            if (focused_window_pid > 0) {
                focused_task = proc_find_by_pid((uint32_t)focused_window_pid);
            }

            int block_for_focus = 0;
            if (focused_task &&
                focused_task->term_mode == 1 &&
                curr->term_mode == 1 &&
                focused_task->terminal &&
                curr->terminal &&
                focused_task->terminal != curr->terminal) {
                block_for_focus = 1;
            }

            if (!block_for_focus) {
                break;
            }

            uint32_t target = timer_ticks + 5;
            proc_sleep_add(curr, target);

            if (curr->pending_signals & (1 << 2)) {
                curr->pending_signals &= ~(1 << 2);
                return -2;
            }
        }

        sem_wait(&kbd_sem);

        char c;
        if (!kbd_try_read_char(&c)) {
            continue;
        }

        task_t* focused_task = 0;
        if (focused_window_pid > 0) {
            focused_task = proc_find_by_pid((uint32_t)focused_window_pid);
        }

        int block_for_focus = 0;
        if (focused_task &&
            focused_task->term_mode == 1 &&
            curr->term_mode == 1 &&
            focused_task->terminal &&
            curr->terminal &&
            focused_task->terminal != curr->terminal) {
            block_for_focus = 1;
        }

        if (block_for_focus) {
            kbd_put_char(c);

            uint32_t target = timer_ticks + 5;
            proc_sleep_add(curr, target);

            if (curr->pending_signals & (1 << 2)) {
                curr->pending_signals &= ~(1 << 2);
                return -2;
            }

            continue;
        }

        b[0] = c;
        return 1;
    }
}

static vfs_ops_t kbd_ops = { .read = kbd_vfs_read };
static vfs_node_t kbd_node = { .name = "kbd", .ops = &kbd_ops };

void kbd_vfs_init() {
    devfs_register(&kbd_node);
}

void kbd_init(void) {
    sem_init(&kbd_sem, 0);
    irq_install_handler(1, keyboard_irq_handler);
}