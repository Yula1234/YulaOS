// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <arch/i386/idt.h>
#include <fs/vfs.h>

#include <kernel/sched.h>
#include <kernel/proc.h>
#include <kernel/input_focus.h>
#include <kernel/poll_waitq.h>
#include <drivers/fbdev.h>

#include <hal/io.h>
#include <hal/irq.h>
#include <hal/lock.h>

#include "keyboard.h"

static int shift_pressed = 0;
static int ctrl_pressed = 0;
static int alt_pressed = 0;
static int e0_flag = 0; 

static uint8_t super_mask;
 
static spinlock_t kbd_scancode_lock;

static const char map_norm[] = { 0,27,'1','2','3','4','5','6','7','8','9','0','-','=', '\b','\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',0,'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c','v','b','n','m',',','.','/',0,'*',0,' ' };
static const char map_shift[] = { 0,27,'!','@','#','$','%','^','&','*','(',')','_','+', '\b','\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,'A','S','D','F','G','H','J','K','L',':','\"','~',0,'|','Z','X','C','V','B','N','M','<','>','?',0,'*',0,' ' };

#define KBD_BUF_SIZE 128
static char kbd_buffer[KBD_BUF_SIZE];
static uint32_t kbd_head = 0, kbd_tail = 0;

static semaphore_t kbd_sem;
static poll_waitq_t kbd_poll_waitq;

static spinlock_t kbd_buf_lock;

static void kbd_put_char(char c) {
    uint32_t flags = spinlock_acquire_safe(&kbd_buf_lock);
    uint32_t next = (kbd_head + 1) % KBD_BUF_SIZE;
    const int was_full = (next == kbd_tail);
    if (was_full) {
        kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
        next = (kbd_head + 1) % KBD_BUF_SIZE;
    }

    kbd_buffer[kbd_head] = c;
    kbd_head = next;
    if (!was_full) {
        sem_signal(&kbd_sem);
    }
    poll_waitq_wake_all(&kbd_poll_waitq);
    spinlock_release_safe(&kbd_buf_lock, flags);
}

int kbd_poll_ready(task_t* task) {
    if (!task) return 0;

    uint32_t owner_pid = fb_get_owner_pid();
    if (owner_pid != 0) {
        if (task->pid != owner_pid) {
            return 0;
        }
    } else {
        uint32_t focus_pid = input_focus_get_pid();
        if (focus_pid > 0 && task->pid != focus_pid) {
            return 0;
        }
    }

    uint32_t flags = spinlock_acquire_safe(&kbd_buf_lock);
    int ready = (kbd_head != kbd_tail);
    spinlock_release_safe(&kbd_buf_lock, flags);
    return ready;
}

int kbd_poll_waitq_register(poll_waiter_t* w, task_t* task) {
    if (!w || !task) return -1;
    return poll_waitq_register(&kbd_poll_waitq, w, task);
}

void kbd_poll_notify_focus_change(void) {
    poll_waitq_wake_all(&kbd_poll_waitq);
}

extern volatile uint32_t timer_ticks;

static void send_key_to_focused(char code) {
    kbd_put_char(code);
}

void kbd_handle_scancode(uint8_t scancode) {
    char send_code = 0;
    char send_code_term0 = 0;
    int do_ctrl_c = 0;

    uint32_t flags = spinlock_acquire_safe(&kbd_scancode_lock);

    if (scancode == 0xE0) { e0_flag = 1; goto out_unlock; }

    if (e0_flag) {
        e0_flag = 0;
        uint8_t sc = (uint8_t)(scancode & 0x7Fu);
        int is_break = (scancode & 0x80u) ? 1 : 0;

        if (sc == 0x1D) { ctrl_pressed = is_break ? 0 : 1; goto out_unlock; }
        if (sc == 0x38) { alt_pressed = is_break ? 0 : 1; goto out_unlock; }
        if (sc == 0x5B) {
            uint8_t prev = super_mask;
            if (is_break) super_mask &= (uint8_t)~1u;
            else super_mask |= 1u;
            if (!is_break && prev == 0 && super_mask) send_code = (char)0xC0;
            if (is_break && prev && super_mask == 0) send_code = (char)0xC1;
            goto out_unlock;
        }
        if (sc == 0x5C) {
            uint8_t prev = super_mask;
            if (is_break) super_mask &= (uint8_t)~2u;
            else super_mask |= 2u;
            if (!is_break && prev == 0 && super_mask) send_code = (char)0xC0;
            if (is_break && prev && super_mask == 0) send_code = (char)0xC1;
            goto out_unlock;
        }

        if (!is_break) {
            switch (sc) {
                case 0x4B: // Left
                    if (super_mask) send_code = (char)0xB1;
                    else if (ctrl_pressed && shift_pressed) send_code = 0x86; // Shift + Ctrl + Left (NEW)
                    else if (ctrl_pressed) send_code = 0x84;      // Ctrl + Left
                    else if (shift_pressed) send_code = 0x82;     // Shift + Left
                    else send_code = 0x11;
                    break; 

                case 0x4D: // Right
                    if (super_mask) send_code = (char)0xB2;
                    else if (ctrl_pressed && shift_pressed) send_code = 0x87; // Shift + Ctrl + Right (NEW)
                    else if (ctrl_pressed) send_code = 0x85;      // Ctrl + Right
                    else if (shift_pressed) send_code = 0x83;     // Shift + Right
                    else send_code = 0x12;
                    break;

                case 0x48: // Up
                    if (super_mask) send_code = (char)0xB3;
                    else if (shift_pressed) send_code = 0x80;
                    else send_code = 0x13;
                    break;
                case 0x50: // Down
                    if (super_mask) send_code = (char)0xB4;
                    else if (shift_pressed) send_code = 0x81;
                    else send_code = 0x14;
                    break;
            }
        }
        goto out_unlock;
    }

    if (scancode == 0x1D) { ctrl_pressed = 1; goto out_unlock; }
    if (scancode == 0x9D) { ctrl_pressed = 0; goto out_unlock; }
    if (scancode == 0x38) { alt_pressed = 1; goto out_unlock; }
    if (scancode == 0xB8) { alt_pressed = 0; goto out_unlock; }
    if (scancode == 0x2A || scancode == 0x36) { shift_pressed = 1; goto out_unlock; }
    if (scancode == 0xAA || scancode == 0xB6) { shift_pressed = 0; goto out_unlock; }

    // Ctrl+Shift+C (copy)
    if (ctrl_pressed && shift_pressed && scancode == 0x2E) {
        send_code = 0x04;
        goto out_unlock;
    }

    // Ctrl+C
    if (ctrl_pressed && scancode == 0x2E) { 
        do_ctrl_c = 1;
        goto out_unlock;
    }

    if (!(scancode & 0x80)) {
        if (super_mask) {
            if (scancode >= 0x02 && scancode <= 0x06) {
                uint8_t k = (uint8_t)(scancode - 0x02);
                send_code = shift_pressed ? (char)(0xA0u + k) : (char)(0x90u + k);
                goto out_unlock;
            }

            if (scancode == 0x0B) {
                send_code = shift_pressed ? (char)0xA5 : (char)0x95;
                goto out_unlock;
            }

            if (scancode == 0x10) { send_code = (char)0xA8; goto out_unlock; }
            if (scancode == 0x2E) { send_code = (char)0xA9; goto out_unlock; }
            if (scancode == 0x12) { send_code = (char)0xAA; goto out_unlock; }
            if (scancode == 0x13) { send_code = (char)0xAB; goto out_unlock; }
            if (scancode == 0x2F) { send_code = (char)0xAC; goto out_unlock; }
            if (scancode == 0x32) { send_code = (char)0xAD; goto out_unlock; }
            if (scancode == 0x19) { send_code = (char)0xAE; goto out_unlock; }
            if (scancode == 0x24) { send_code = (char)0xAF; goto out_unlock; }
        }

        if (ctrl_pressed) {
            if (scancode == 0x1F) { send_code = 0x15; goto out_unlock; } // Ctrl+S
            if (scancode == 0x10) { send_code = 0x17; goto out_unlock; } // Ctrl+Q
            if (scancode == 0x2F) { send_code = 0x16; goto out_unlock; } // Ctrl+V
            if (scancode == 0x16) { send_code = (char)0x88; goto out_unlock; }
            if (scancode == 0x25) { send_code = (char)0x89; goto out_unlock; }

            if (scancode == 0x21) { send_code_term0 = 0x06; } // Ctrl+F
            if (scancode == 0x22) { send_code_term0 = 0x07; } // Ctrl+G
            if (scancode == 0x2C) { send_code_term0 = 0x1A; } // Ctrl+Z
            if (scancode == 0x15) { send_code_term0 = 0x19; } // Ctrl+Y
            if (scancode == 0x31) { send_code_term0 = 0x0E; } // Ctrl+N
        }

        char c = shift_pressed ? map_shift[scancode] : map_norm[scancode];
        if (c) {
            send_code = c;
        }
    }

out_unlock:
    spinlock_release_safe(&kbd_scancode_lock, flags);

    if (do_ctrl_c) {
        uint32_t focus_pid = input_focus_get_pid();
        task_t* target_task = proc_find_by_pid(focus_pid);

        if (target_task) {
            if (target_task->term_mode == 0) {
                send_key_to_focused(0x03);
                return;
            }

            if (target_task->term_mode == 1) {
                if (target_task->wait_for_pid > 0) {
                    int child_pid = target_task->wait_for_pid;
                    task_t* t = proc_find_by_pid(child_pid);
                    if (t) {
                        t->pending_signals |= (1 << 2);
                        proc_wake(t);
                    }
                } else {
                    target_task->pending_signals |= (1 << 2);
                    proc_wake(target_task);
                }
                send_key_to_focused(0x03);
            }
        }
        return;
    }

    if (send_code_term0) {
        uint32_t focus_pid = input_focus_get_pid();
        task_t* target_task = proc_find_by_pid(focus_pid);
        if (target_task && target_task->term_mode == 0) {
            send_key_to_focused(send_code_term0);
            return;
        }
    }

    if (send_code) {
        send_key_to_focused(send_code);
    }
}

void keyboard_irq_handler(registers_t* regs) {
    (void)regs;
    uint8_t scancode = inb(0x60);
    kbd_handle_scancode(scancode);
}

int kbd_try_read_char(char* out) {
    if (!out) return 0;
    uint32_t flags = spinlock_acquire_safe(&kbd_buf_lock);
    if (kbd_head == kbd_tail) {
        spinlock_release_safe(&kbd_buf_lock, flags);
        return 0;
    }
    *out = kbd_buffer[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    spinlock_release_safe(&kbd_buf_lock, flags);
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
    int focus_pid = 0;

    while (1) {
        if (curr->pending_signals & (1 << 2)) { // SIGINT
            curr->pending_signals &= ~(1 << 2);
            return -2; // EINTR
        }

        uint32_t owner_pid = fb_get_owner_pid();
        if (owner_pid != 0) {
            if (!curr || curr->pid != owner_pid) {
                uint32_t target = timer_ticks + 5;
                proc_sleep_add(curr, target);
                continue;
            }
        } else {
            focus_pid = (int)input_focus_get_pid();
            if (focus_pid > 0 && curr && (int)curr->pid != focus_pid) {
                uint32_t target = timer_ticks + 5;
                proc_sleep_add(curr, target);
                continue;
            }
        }

        if (owner_pid == 0) {
            while (1) {
                task_t* focused_task = 0;
                focus_pid = (int)input_focus_get_pid();
                if (focus_pid > 0) {
                    focused_task = proc_find_by_pid((uint32_t)focus_pid);
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
        }

        sem_wait(&kbd_sem);

        char c;
        if (!kbd_try_read_char(&c)) {
            continue;
        }

        if (owner_pid == 0) {
            task_t* focused_task = 0;
            focus_pid = (int)input_focus_get_pid();
            if (focus_pid > 0) {
                focused_task = proc_find_by_pid((uint32_t)focus_pid);
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
    poll_waitq_init(&kbd_poll_waitq);
    spinlock_init(&kbd_scancode_lock);
    spinlock_init(&kbd_buf_lock);
    irq_install_handler(1, keyboard_irq_handler);
}