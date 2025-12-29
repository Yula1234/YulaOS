#include <drivers/vga.h>
#include <drivers/keyboard.h>
#include <hal/io.h>
#include <lib/string.h>
#include <kernel/proc.h>
#include <kernel/gui_task.h>
#include <kernel/window.h>
#include <kernel/sched.h>
#include <hal/io.h>
#include <drivers/pc_speaker.h>

#include "kdb.h"

extern uint32_t* fb_ptr;
extern uint32_t  fb_width;
extern uint32_t  fb_height;
extern const uint8_t font8x16_basic[128][16];

static char kdb_scancode_to_ascii(uint8_t sc) {
    static const char map[] = {
        0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 8, 9,
        'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 10, 0,
        'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
        'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
    };
    if (sc < sizeof(map)) return map[sc];
    return 0;
}

static int kdb_cursor_x = 0;
static int kdb_cursor_y = 0;

static void kdb_putc(char c) {
    if (c == '\n') {
        kdb_cursor_x = 0;
        kdb_cursor_y += 16;
        return;
    }
    if (c == 8) { // Backspace
        if (kdb_cursor_x >= 9) {
            kdb_cursor_x -= 9;
            for(int y=0; y<16; y++) 
                for(int x=0; x<9; x++) 
                    fb_ptr[(kdb_cursor_y+y)*fb_width + (kdb_cursor_x+x)] = 0xFF000000;
        }
        return;
    }

    if ((uint8_t)c > 127) return;
    const uint8_t* glyph = font8x16_basic[(uint8_t)c];
    
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 8; j++) {
            uint32_t color = (glyph[i] & (1 << (7 - j))) ? 0xFFFFFFFF : 0xFF000000;
            fb_ptr[(kdb_cursor_y + i) * fb_width + (kdb_cursor_x + j)] = color;
        }
    }
    kdb_cursor_x += 9;
    if (kdb_cursor_x >= (int)fb_width) {
        kdb_cursor_x = 0;
        kdb_cursor_y += 16;
    }
}

static void kdb_print(const char* s) {
    while (*s) kdb_putc(*s++);
}

static void kdb_clear_screen() {
    int total = fb_width * fb_height;
    for(int i=0; i<total; i++) fb_ptr[i] = 0xFF000000;
    kdb_cursor_x = 0;
    kdb_cursor_y = 0;
}

static char kdb_wait_key() {
    while (1) {
        if (inb(0x64) & 1) {
            uint8_t sc = inb(0x60);
            if (!(sc & 0x80)) {
                return kdb_scancode_to_ascii(sc);
            }
        }
        __asm__ volatile("pause");
    }
}

static char* itoa_hex(uint32_t val) {
    static char buf[16];
    const char* h = "0123456789ABCDEF";
    buf[0] = '0'; buf[1] = 'x';
    for(int i=0; i<8; i++) buf[9-i] = h[(val >> (i*4)) & 0xF];
    buf[10] = 0;
    return buf;
}

extern spinlock_t window_lock;

void kdb_restart_gui() {
    kdb_print("\n[KDB] Resetting window system...\n");
    
    window_list_lock.count = 1;
    window_list_lock.lock.locked = 0;
    kdb_print("[KDB] window_lock forced to UNLOCKED.\n");

    window_init_system();
    kdb_print("[KDB] Window list cleared.\n");

    proc_spawn_kthread("gui", PRIO_GUI, gui_task, (void*)1);
    kdb_print("[KDB] New 'gui' thread spawned.\n");
}

void kdb_enter(const char* reason, task_t* faulty_process) {
    __asm__ volatile("cli");

    uint8_t old_mask = inb(0x21);

    outb(0x21, 0xFF); 

    while (inb(0x64) & 1) inb(0x60);

    pc_speaker_error();

    kdb_clear_screen();
    
    kdb_print("================================================================\n");
    kdb_print("                     SAFE SHELL                                 \n");
    kdb_print("================================================================\n\n");
    
    kdb_print("CRITICAL ERROR: "); kdb_print(reason); kdb_print("\n");
    if (faulty_process) {
        kdb_print("FAULTY PROCESS: "); kdb_print(faulty_process->name); 
        kdb_print(" (PID: "); kdb_print(itoa_hex(faulty_process->pid)); kdb_print(")\n");
    }
    
    kdb_print("\nAvailable commands:\n");
    kdb_print("  help    - Show this menu\n");
    kdb_print("  restart - Restart GUI subsystem (Force Unlock)\n");
    kdb_print("  reboot  - Hard reboot\n");
    kdb_print("  exit    - Kill process and try to continue (Risky)\n");

    char cmd_buf[64];
    int cmd_pos = 0;

    while (1) {
        kdb_print("\nKDB> ");
        
        cmd_pos = 0;
        memset(cmd_buf, 0, 64);

        while (1) {
            char c = kdb_wait_key();
            if (c == 10) { // Enter
                kdb_putc('\n');
                break;
            }
            if (c == 8) { // Backspace
                if (cmd_pos > 0) {
                    cmd_pos--;
                    cmd_buf[cmd_pos] = 0;
                    kdb_putc(8);
                }
                continue;
            }
            if (c) {
                if (cmd_pos < 63) {
                    cmd_buf[cmd_pos++] = c;
                    kdb_putc(c);
                }
            }
        }

        if (strcmp(cmd_buf, "help") == 0) {
            kdb_print("Commands: restart, reboot, exit\n");
        } 
        else if (strcmp(cmd_buf, "reboot") == 0) {
            outb(0x64, 0xFE);
        }
        else if (strcmp(cmd_buf, "restart") == 0) {
            if (faulty_process && strcmp(faulty_process->name, "gui") == 0) {
                kdb_restart_gui();
                kdb_print("[KDB] Returning to scheduler. Fingers crossed!\n");

                outb(0x21, old_mask);
                return; 
            } else {
                kdb_print("Error: Faulty process is not GUI. Cannot restart generic process yet.\n");
            }
        }
        else if (strcmp(cmd_buf, "exit") == 0) {
            outb(0x21, old_mask);
            return;
        }
        else {
            kdb_print("Unknown command.\n");
        }
    }
}