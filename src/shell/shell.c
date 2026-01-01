// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <lib/string.h>
#include <hal/lock.h>

#include <kernel/clipboard.h>
#include <kernel/window.h>
#include <kernel/proc.h>

#include <drivers/keyboard.h>
#include <drivers/vga.h>

#include <fs/yulafs.h>
#include <fs/pipe.h>
#include <fs/vfs.h>

#include <mm/heap.h>
#include <mm/pmm.h>

#include "shell.h"

#define LINE_MAX 256
#define TOK_MAX 16
#define HIST_MAX 16 

#define C_BG      0x141414  
#define C_TEXT    0xD4D4D4  
#define C_PROMPT  0x4EC9B0  
#define C_PATH    0x569CD6  
#define C_BAR_BG  0x1E1E1E  
#define C_BAR_TXT 0x808080  
#define C_ACCENT  0x007ACC 
#define C_ERROR   0xF44747 

#define C_DIR       0x569CD6 
#define C_EXE       0xB5CEA8 
#define C_SRC       0xCE9178 
#define C_TXT       0x9CDCFE 
#define C_SIZE      0x606060 

#define C_RUNNING   0x6A9955 
#define C_WAITING   0xDCDCAA 
#define C_ZOMBIE    0xF44747 

typedef struct {
    char lines[HIST_MAX][LINE_MAX];
    int head;  
    int count; 
    int view_idx; 
    char temp_line[LINE_MAX]; 
} shell_history_t;

typedef struct {
    term_instance_t* term;
    shell_history_t* hist;
    spinlock_t lock;
} shell_context_t;

extern void wake_up_gui();

extern volatile uint32_t timer_ticks;
#define TICKS_PER_SEC 15000 

static char* itoa(uint32_t n) {
    static char buf[12];
    int i = 10; buf[11] = '\0';
    if (n == 0) return "0";
    while (n > 0) { buf[i--] = (n % 10) + '0'; n /= 10; }
    return &buf[i + 1];
}

int atoi(const char* str) {
    int res = 0; int i = 0;
    while (str[i] == ' ') i++;
    while (str[i] >= '0' && str[i] <= '9') { res = res * 10 + (str[i] - '0'); i++; }
    return res;
}

static inline void sys_exit() { __asm__ volatile("int $0x80" : : "a"(0), "b"(0)); }

static int shell_dup(int oldfd) {
    task_t* curr = proc_current();
    int newfd = -1;
    for(int i=0; i<MAX_PROCESS_FDS; i++) {
        if(!curr->fds[i].used) { newfd = i; break; }
    }
    if (newfd == -1) return -1;
    
    curr->fds[newfd] = curr->fds[oldfd];
    if (curr->fds[newfd].node) __sync_fetch_and_add(&curr->fds[newfd].node->refs, 1);
    return newfd;
}

static int shell_dup2(int oldfd, int newfd) {
    task_t* curr = proc_current();
    if (curr->fds[newfd].used) {
        vfs_close(newfd);
    }
    curr->fds[newfd] = curr->fds[oldfd];
    if (curr->fds[newfd].node) __sync_fetch_and_add(&curr->fds[newfd].node->refs, 1);
    return newfd;
}

static int shell_create_pipe(int fds[2]) {
    vfs_node_t *r, *w;
    if (vfs_create_pipe(&r, &w) != 0) return -1;
    
    task_t* curr = proc_current();
    int r_fd = -1, w_fd = -1;
    
    for(int i=0; i<MAX_PROCESS_FDS; i++) if(!curr->fds[i].used) { r_fd = i; break; }
    for(int i=0; i<MAX_PROCESS_FDS; i++) if(!curr->fds[i].used && i != r_fd) { w_fd = i; break; }
    
    if (r_fd == -1 || w_fd == -1) {
        return -1; 
    }
    
    curr->fds[r_fd].node = r; curr->fds[r_fd].offset = 0; curr->fds[r_fd].used = 1;
    curr->fds[w_fd].node = w; curr->fds[w_fd].offset = 0; curr->fds[w_fd].used = 1;
    
    fds[0] = r_fd;
    fds[1] = w_fd;
    return 0;
}


static void print_prompt_text(term_instance_t* term, const char* path) {
    term_print(term, "user@yulaos");
    term_print(term, ":");
    term_print(term, path);
    term_print(term, "$ ");
}

static int get_prompt_len(const char* path) {
    return 11 + 1 + strlen(path) + 2;
}

static void refresh_line(term_instance_t* term, const char* path, char* line, int cursor) {
    for (int i = 0; i < TERM_W; i++) {
        term->buffer[term->row * TERM_W + i] = ' ';
    }
    term->col = 0;
    print_prompt_text(term, path);
    term_print(term, line);
    
    int visual_cursor = get_prompt_len(path) + cursor;
    if (visual_cursor >= TERM_W) visual_cursor = TERM_W - 1;
    term->col = visual_cursor;
    
    int visible_rows = 14; 
    if (term->row < term->view_row || term->row >= term->view_row + visible_rows) {
         if (term->row >= visible_rows) 
             term->view_row = term->row - visible_rows + 1;
         else 
             term->view_row = 0;
    }
}

static void shell_cleanup_handler(window_t* win) {
    if (win->user_data) {
        shell_context_t* ctx = (shell_context_t*)win->user_data;
        if (ctx->term) kfree(ctx->term);
        if (ctx->hist) kfree(ctx->hist);
        kfree(ctx);
    }
}

static void shell_window_draw_handler(window_t* self, int x, int y) {
    shell_context_t* ctx = (shell_context_t*)self->user_data;
    if (!ctx || !ctx->term) return;
    term_instance_t* term = ctx->term;
    if(!term) return;

    uint32_t flags = spinlock_acquire_safe(&term->lock);
    
    int canvas_w = self->target_w - 12;
    int canvas_h = self->target_h - 44; 
    int status_bar_h = 22;              
    int text_area_h = canvas_h - status_bar_h;
    
    int visible_rows = text_area_h / 16; 

    vga_draw_rect(x, y, canvas_w, text_area_h, C_BG);

    for (int r = 0; r < visible_rows; r++) {
        int buf_row = term->view_row + r;
        if (buf_row >= TERM_HISTORY) break;

        for (int c = 0; c < TERM_W; c++) {
            int idx = buf_row * TERM_W + c;
            char ch = term->buffer[idx];
            uint32_t fg = term->fg_colors[idx];
            uint32_t bg = term->bg_colors[idx];

            if (bg != C_BG) vga_draw_rect(x + c * 8, y + r * 16, 8, 16, bg);
            if (ch != ' ') vga_draw_char_sse(x + c * 8, y + r * 16, ch, fg);
        }
    }

    int sb_x = x + canvas_w - 6;
    vga_draw_rect(sb_x, y, 6, text_area_h, 0x222222);
    int total_rows = term->max_row + 1;
    if (total_rows < visible_rows) total_rows = visible_rows;
    int thumb_h = (visible_rows * text_area_h) / total_rows;
    if (thumb_h < 10) thumb_h = 10;
    if (thumb_h > text_area_h) thumb_h = text_area_h;
    int scrollable_area_h = text_area_h - thumb_h;
    int scrollable_rows = total_rows - visible_rows;
    int thumb_y = 0;
    if (scrollable_rows > 0) thumb_y = (term->view_row * scrollable_area_h) / scrollable_rows;
    vga_draw_rect(sb_x + 1, y + thumb_y, 4, thumb_h, 0x666666);

    int bx = x;
    int by = y + text_area_h; 
    vga_draw_rect(bx, by, canvas_w, status_bar_h, C_BAR_BG);
    vga_draw_rect(bx, by, canvas_w, 1, 0x333333); 
    vga_print_at("PID:", bx + 10, by + 5, C_BAR_TXT);
    vga_print_at(itoa(self->owner_pid), bx + 45, by + 5, C_ACCENT);

    if (focused_window_pid == (int)self->owner_pid) {
        int rel_cursor_row = term->row - term->view_row;
        if (rel_cursor_row >= 0 && rel_cursor_row < visible_rows) {
            vga_draw_rect(x + term->col * 8, y + rel_cursor_row * 16 + 12, 8, 2, 0x00FF00);
        }
    }

    spinlock_release_safe(&term->lock, flags);
}

static void print_padded(term_instance_t* term, const char* text, int width, uint32_t color) {
    term->curr_fg = color;
    term_print(term, text);
    int len = strlen(text);
    term->curr_fg = C_BG;
    while (len < width) { term_print(term, " "); len++; }
}

static uint32_t get_file_color(const char* name) {
    int len = strlen(name);
    if (len > 4) {
        const char* ext = name + len - 4;
        if (strcmp(ext, ".exe") == 0) return C_EXE;
        if (strcmp(ext, ".asm") == 0) return C_SRC;
        if (strcmp(ext, ".txt") == 0) return C_TXT;
    }
    if (len > 2) {
        if (strcmp(name + len - 2, ".c") == 0) return C_SRC;
        if (strcmp(name + len - 2, ".h") == 0) return C_SRC;
    }
    return C_TEXT;
}

static void shell_ls(term_instance_t* term, const char* arg, uint32_t cwd_inode) {
    uint32_t target_inode = cwd_inode;
    if (arg && strlen(arg) > 0) {
        if (strcmp(arg, "/dev") == 0 || strcmp(arg, "dev") == 0) {
            term->curr_fg = C_SIZE; term_print(term, "TYPE  NAME\n");
            print_padded(term, "[CHR]", 6, C_EXE); print_padded(term, "kbd", 10, C_EXE); term_print(term, "\n");
            print_padded(term, "[CHR]", 6, C_EXE); print_padded(term, "console", 10, C_EXE); term_print(term, "\n");
            return;
        }
        int lookup = yulafs_lookup(arg);
        if (lookup > 0) target_inode = (uint32_t)lookup;
        else { term->curr_fg = C_ERROR; term_print(term, "ls: directory not found\n"); return; }
    }

    term->curr_fg = C_SIZE;
    print_padded(term, "MOD", 5, C_SIZE);
    print_padded(term, "SIZE", 10, C_SIZE);
    print_padded(term, "NAME", 20, C_SIZE);
    term_print(term, "\n");

    yfs_dirent_t entries[8]; 
    int offset = 0;
    
    while (1) {
        int bytes = yulafs_read(target_inode, (uint8_t*)entries, offset, 512);
        if (bytes <= 0) break;

        int count = bytes / sizeof(yfs_dirent_t);
        
        for (int i = 0; i < count; i++) {
            if (entries[i].inode > 0) {
                yfs_inode_t info;
                yulafs_stat(entries[i].inode, &info);
                
                if (info.type == YFS_TYPE_DIR) print_padded(term, "DIR", 5, C_DIR);
                else print_padded(term, "FILE", 5, C_SIZE);

                term->curr_fg = C_SIZE;
                if (info.type == YFS_TYPE_DIR) {
                    print_padded(term, "-", 10, C_SIZE);
                } else {
                    char sz_buf[16];
                    int sz = info.size;
                    if (sz < 1024) { char* s = itoa(sz); int l = strlen(s); memcpy(sz_buf, s, l); sz_buf[l] = 'B'; sz_buf[l+1] = 0; } 
                    else { char* s = itoa(sz/1024); int l = strlen(s); memcpy(sz_buf, s, l); sz_buf[l] = 'K'; sz_buf[l+1] = 0; }
                    print_padded(term, sz_buf, 10, C_SIZE);
                }

                uint32_t name_col = (info.type == YFS_TYPE_DIR) ? C_DIR : get_file_color(entries[i].name);
                print_padded(term, entries[i].name, 20, name_col);
                term_print(term, "\n");
            }
        }
        offset += bytes;
    }
    
    term->curr_fg = C_TEXT; term->curr_bg = C_BG;
}

static void shell_cd(term_instance_t* term, const char* new_path, uint32_t* cwd_inode, char* path_str) {
    int inode = yulafs_lookup(new_path);
    if (inode == -1) { term_print(term, "cd: no such directory\n"); return; }
    
    yfs_inode_t info; 
    yulafs_stat(inode, &info);
    
    if (info.type != YFS_TYPE_DIR) { term_print(term, "cd: not a directory\n"); return; }
    
    *cwd_inode = (uint32_t)inode;
    if (new_path[0] == '/') strlcpy(path_str, new_path, 64);
    else {
        if (strcmp(new_path, "..") == 0) {
            int len = strlen(path_str);
            if (len > 1) for (int i = len - 1; i >= 0; i--) if (path_str[i] == '/') { path_str[i == 0 ? 1 : i] = '\0'; break; }
        } else {
            if (strlen(path_str) > 1) strlcat(path_str, "/", 64);
            strlcat(path_str, new_path, 64);
        }
    }
}

static int parse_args(char* line, char** args) {
    int count = 0; char* ptr = line; int in_quote = 0;
    while (*ptr && count < TOK_MAX) {
        while (*ptr == ' ') *ptr++ = 0;
        if (!*ptr) break;
        if (*ptr == '"') { in_quote = 1; ptr++; args[count++] = ptr; } else args[count++] = ptr;
        while (*ptr) {
            if (in_quote) { if (*ptr == '"') { *ptr++ = 0; in_quote = 0; break; } }
            else { if (*ptr == ' ') break; }
            ptr++;
        }
    }
    return count;
}

static void hist_init(shell_history_t* h) { memset(h, 0, sizeof(shell_history_t)); h->view_idx = -1; }
static void hist_add(shell_history_t* h, const char* cmd) {
    if (strlen(cmd) == 0) return;
    int last = (h->head - 1 + HIST_MAX) % HIST_MAX;
    if (h->count > 0 && strcmp(h->lines[last], cmd) == 0) return;
    strlcpy(h->lines[h->head], cmd, LINE_MAX);
    h->head = (h->head + 1) % HIST_MAX;
    if (h->count < HIST_MAX) h->count++;
    h->view_idx = -1;
}
static const char* hist_get_prev(shell_history_t* h) {
    if (h->count == 0) return 0;
    if (h->view_idx == -1) h->view_idx = (h->head - 1 + HIST_MAX) % HIST_MAX;
    else {
        int oldest = (h->head - h->count + HIST_MAX) % HIST_MAX;
        if (h->view_idx == oldest) return 0;
        h->view_idx = (h->view_idx - 1 + HIST_MAX) % HIST_MAX;
    }
    return h->lines[h->view_idx];
}
static const char* hist_get_next(shell_history_t* h) {
    if (h->count == 0 || h->view_idx == -1) return 0;
    int newest = (h->head - 1 + HIST_MAX) % HIST_MAX;
    if (h->view_idx == newest) { h->view_idx = -1; return ""; }
    h->view_idx = (h->view_idx + 1) % HIST_MAX;
    return h->lines[h->view_idx];
}

static task_t* spawn_command(const char* cmd, int argc, char** argv) {
    task_t* child = proc_spawn_elf(cmd, argc, argv);
    if (child) return child;

    char tmp[64];
    strlcpy(tmp, cmd, 64); strlcat(tmp, ".exe", 64);
    child = proc_spawn_elf(tmp, argc, argv);
    if (child) return child;

    if (cmd[0] != '/') {
        strlcpy(tmp, "/bin/", 64); strlcat(tmp, cmd, 64);
        child = proc_spawn_elf(tmp, argc, argv);
        if (child) return child;

        strlcat(tmp, ".exe", 64);
        child = proc_spawn_elf(tmp, argc, argv);
    }
    return child;
}

static void shell_ps(term_instance_t* term) {
    term->curr_fg = C_SIZE;
    print_padded(term, "PID", 6, C_SIZE);
    print_padded(term, "MEM", 10, C_SIZE);
    print_padded(term, "STATE", 10, C_SIZE);
    print_padded(term, "NAME", 20, C_SIZE);
    term_print(term, "\n");

    task_t* curr = proc_get_list_head();
    while (curr) {
        uint32_t state_col = C_TEXT;
        const char* state_str = "?";
        switch (curr->state) {
            case TASK_RUNNING:  state_str = "RUN";  state_col = C_RUNNING; break;
            case TASK_RUNNABLE: state_str = "READY";state_col = C_WAITING; break;
            case TASK_WAITING:  state_str = "WAIT"; state_col = C_SIZE;    break;
            case TASK_ZOMBIE:   state_str = "DEAD"; state_col = C_ZOMBIE;  break;
            default: break;
        }
        print_padded(term, itoa(curr->pid), 6, C_ACCENT);
        char mem_buf[16]; char* m = itoa(curr->mem_pages * 4);
        int l = strlen(m); memcpy(mem_buf, m, l); mem_buf[l] = 'K'; mem_buf[l+1] = 0;
        print_padded(term, mem_buf, 10, C_SIZE);
        print_padded(term, state_str, 10, state_col);
        print_padded(term, curr->name, 20, C_TEXT);
        term_print(term, "\n");
        curr = curr->next;
    }
    term->curr_fg = C_TEXT; term->curr_bg = C_BG;
}

void shell_task(void* arg) {
    (void)arg;
    shell_context_t* ctx = kzalloc(sizeof(shell_context_t));
    if (!ctx) return;
    term_instance_t* my_term = kzalloc(sizeof(term_instance_t));
    shell_history_t* my_hist = kzalloc(sizeof(shell_history_t));
    
    if (!my_term || !my_hist) {
        if(ctx) kfree(ctx);
        if(my_term) kfree(my_term);
        if(my_hist) kfree(my_hist);
        return;
    }

    spinlock_init(&my_term->lock);
    spinlock_init(&ctx->lock);

    ctx->term = my_term; ctx->hist = my_hist;
    hist_init(my_hist);
    proc_current()->terminal = my_term;
    proc_current()->term_mode = 1;
    memset(my_term->buffer, ' ', TERM_W * TERM_H);
    my_term->curr_fg = C_TEXT; my_term->curr_bg = C_BG;
    for(int i=0; i<TERM_W * TERM_HISTORY; i++) {
        my_term->buffer[i] = ' '; my_term->fg_colors[i] = my_term->curr_fg; my_term->bg_colors[i] = my_term->curr_bg;
    }
    my_term->col = 0; 
    my_term->row = 0;
    my_term->view_row = 0;
    my_term->max_row = 0;

    char line[LINE_MAX]; memset(line, 0, LINE_MAX);
    int line_len = 0; int cursor_pos = 0;
    char path[64] = "/home";
    uint32_t cwd_inode = yulafs_lookup("/home");
    if ((int)cwd_inode == -1) { cwd_inode = 1; strlcpy(path, "/", 64); }

    window_t* win = window_create(100, 100, 652, 265, "shell", shell_window_draw_handler);
    if (!win) { kfree(my_term); kfree(my_hist); return; }
    win->user_data = ctx; win->on_close = shell_cleanup_handler;

    int kbd_fd = vfs_open("/dev/kbd", 0);
    vfs_open("/dev/console", 1); 
    vfs_open("/dev/console", 1); 

    print_prompt_text(my_term, path);
    if (yulafs_lookup("/bin") == -1) yulafs_mkdir("/bin");
    if (yulafs_lookup("/home") == -1) yulafs_mkdir("/home");

    while (win->is_active) {
        proc_current()->cwd_inode = cwd_inode;
        char c = 0;
        int bytes_read = vfs_read(kbd_fd, &c, 1);

        if (bytes_read > 0) {

            uint32_t flags = spinlock_acquire_safe(&ctx->lock);

            if (c == '\n') {
                
                my_term->col = get_prompt_len(path) + line_len;
                if (my_term->col >= TERM_W) my_term->col = TERM_W - 1;

                term_putc(my_term, '\n');

                line[line_len] = '\0';
                hist_add(my_hist, line);
                
                char* args[TOK_MAX];
                int arg_count = parse_args(line, args);

                spinlock_release_safe(&ctx->lock, flags);

                int measure_time = 0;
                uint32_t start_ticks = 0;

                if (arg_count > 0 && strcmp(args[0], "time") == 0) {
                    if (arg_count < 2) {
                        term_print(my_term, "Usage: time <command>\n");
                        goto loop_end; 
                    }
                    
                    measure_time = 1;
                    start_ticks = timer_ticks;

                    for (int i = 0; i < arg_count - 1; i++) {
                        args[i] = args[i+1];
                    }
                    args[arg_count - 1] = 0;
                    arg_count--;
                }

                if (arg_count > 0) {
                    int pipe_idx = -1;
                    for(int i=0; i<arg_count; i++) {
                        if(strcmp(args[i], "|") == 0) { pipe_idx = i; break; }
                    }

                    if (pipe_idx != -1 && pipe_idx < arg_count - 1) {
                        args[pipe_idx] = 0;
                        
                        int pfds[2];
                        if (shell_create_pipe(pfds) == 0) {
                            int saved_stdout = shell_dup(1);
                            int saved_stdin  = shell_dup(0);

                            shell_dup2(pfds[1], 1); 
                            task_t* left_task = spawn_command(args[0], pipe_idx, args);
                            
                            shell_dup2(saved_stdout, 1);
                            vfs_close(pfds[1]); 

                            shell_dup2(pfds[0], 0);
                            task_t* right_task = spawn_command(args[pipe_idx+1], arg_count - pipe_idx - 1, &args[pipe_idx+1]);
                            
                            shell_dup2(saved_stdin, 0);
                            vfs_close(pfds[0]);

                            vfs_close(saved_stdout);
                            vfs_close(saved_stdin);

                            if (left_task) proc_wait(left_task->pid);
                            if (right_task) {
                                win->focused_pid = right_task->pid;
                                focused_window_pid = right_task->pid;
                                proc_wait(right_task->pid);
                                win->focused_pid = win->owner_pid;
                                focused_window_pid = win->owner_pid;
                            }
                        } else {
                            term_print(my_term, "Pipe creation failed\n");
                        }
                    } 
                    else {
                        if (strcmp(args[0], "help") == 0) term_print(my_term, "Commands: ls, cd, pwd, mkdir, run, clear, exit, ps, kill\n");
                        else if (strcmp(args[0], "ls") == 0) shell_ls(my_term, (arg_count > 1) ? args[1] : 0, cwd_inode);
                        else if (strcmp(args[0], "cd") == 0) shell_cd(my_term, (arg_count > 1) ? args[1] : "/", &cwd_inode, path);
                        else if (strcmp(args[0], "pwd") == 0) { term_print(my_term, path); term_print(my_term, "\n"); }
                        else if (strcmp(args[0], "clear") == 0) { 
                            term_putc(my_term, 0x0C); 
                        }
                        else if (strcmp(args[0], "mkdir") == 0 && arg_count > 1) yulafs_mkdir(args[1]);
                        else if (strcmp(args[0], "exit") == 0) break;
                        else if (strcmp(args[0], "ps") == 0) shell_ps(my_term);
                        else if (strcmp(args[0], "kill") == 0 && arg_count > 1) {
                            int ret; int pid = atoi(args[1]);
                            __asm__ volatile("int $0x80" : "=a"(ret) : "a"(9), "b"(pid));
                            term_print(my_term, (ret == 0) ? "Killed\n" : "Fail\n");
                        }
                        else if (strcmp(args[0], "rm") == 0 && arg_count > 1) {
                            int ret;
                            ret = yulafs_unlink(args[1]);
                            
                            if (ret == 0) term_print(my_term, "Deleted\n");
                            else term_print(my_term, "Fail\n");
                        }
                        else {
                            task_t* child = spawn_command(args[0], arg_count, args);
                            if (child) {
                                win->focused_pid = child->pid;
                                focused_window_pid = child->pid;
                                proc_wait(child->pid);
                                win->focused_pid = win->owner_pid;
                                focused_window_pid = win->owner_pid;
                            } else {
                                term_print(my_term, "Command not found: ");
                                term_print(my_term, args[0]);
                                term_print(my_term, "\n");
                            }
                        }
                    }
                }

                if (measure_time) {
                    uint32_t end_ticks = timer_ticks;
                    uint32_t diff = end_ticks - start_ticks;
                    
                    uint32_t sec = diff / TICKS_PER_SEC;
                    uint32_t sub_sec = diff % TICKS_PER_SEC;
                    
                    uint32_t ms = (sub_sec * 1000) / TICKS_PER_SEC;

                    char* s_sec = itoa(sec);
                    char buf_sec[16]; memcpy(buf_sec, s_sec, strlen(s_sec)+1);
                    
                    char* s_ms = itoa(ms);
                    char buf_ms[16]; memcpy(buf_ms, s_ms, strlen(s_ms)+1);
                    
                    char* s_ticks = itoa(diff);
                    
                    my_term->curr_fg = C_ACCENT;
                    term_print(my_term, "\n[TIME] ");
                    
                    my_term->curr_fg = C_TEXT;
                    term_print(my_term, "Real: ");
                    term_print(my_term, buf_sec);
                    term_print(my_term, ".");
                    
                    if (ms < 100) term_print(my_term, "0");
                    if (ms < 10) term_print(my_term, "0");
                    term_print(my_term, buf_ms);
                    term_print(my_term, "s (");
                    term_print(my_term, s_ticks);
                    term_print(my_term, " ticks)\n");
                }

                loop_end: 

                flags = spinlock_acquire_safe(&ctx->lock);

                my_term->curr_fg = C_TEXT; 
                my_term->curr_bg = C_BG;

                if (my_term->col > 0) {
                    term_putc(my_term, '\n');
                }
                
                line_len = 0; 
                cursor_pos = 0; 
                memset(line, 0, LINE_MAX);
                
                print_prompt_text(my_term, path);
                
                win->is_dirty = 1;
            } 
            else if (c == 0x13) { 
                const char* h_str = hist_get_prev(my_hist);
                if (h_str) {
                    if (my_hist->view_idx == (my_hist->head - 1 + HIST_MAX) % HIST_MAX) strlcpy(my_hist->temp_line, line, LINE_MAX);
                    strlcpy(line, h_str, LINE_MAX);
                    line_len = strlen(line); cursor_pos = line_len;
                    refresh_line(my_term, path, line, cursor_pos);
                }
            }
            else if (c == 0x14) { 
                const char* h_str = hist_get_next(my_hist);
                if (h_str) {
                    if (strlen(h_str) == 0 && my_hist->view_idx == -1) strlcpy(line, my_hist->temp_line, LINE_MAX);
                    else strlcpy(line, h_str, LINE_MAX);
                    line_len = strlen(line); cursor_pos = line_len;
                    refresh_line(my_term, path, line, cursor_pos);
                }
            }
            else if (c == 0x11) { if (cursor_pos > 0) { cursor_pos--; refresh_line(my_term, path, line, cursor_pos); } }
            else if (c == 0x12) { if (cursor_pos < line_len) { cursor_pos++; refresh_line(my_term, path, line, cursor_pos); } }
            else if (c == (char)0x80) { if (my_term->view_row > 0) { my_term->view_row--; win->is_dirty = 1; } }
            else if (c == (char)0x81) { 
                int visible_rows = (win->target_h - 44 - 22) / 16;
                if (my_term->view_row + visible_rows <= my_term->max_row) { my_term->view_row++; win->is_dirty = 1; }
            }
            else if (c == '\b') {
                if (cursor_pos > 0) {
                    for (int i = cursor_pos; i < line_len; i++) line[i-1] = line[i];
                    line_len--; cursor_pos--; line[line_len] = 0;
                    refresh_line(my_term, path, line, cursor_pos);
                }
            } 
            else if (line_len < LINE_MAX - 1 && (uint8_t)c >= 32) {
                for (int i = line_len; i > cursor_pos; i--) line[i] = line[i-1];
                line[cursor_pos] = c;
                line_len++; cursor_pos++; line[line_len] = 0;
                refresh_line(my_term, path, line, cursor_pos);
            }
            spinlock_release_safe(&ctx->lock, flags);
            win->is_dirty = 1;
            wake_up_gui();
        }
    }

    win->on_close = 0; win->on_draw = 0; win->user_data = 0;
    kfree(my_hist); kfree(my_term); kfree(ctx);
    sys_exit();
}