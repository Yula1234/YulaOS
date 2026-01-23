// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <lib/string.h>
#include <hal/lock.h>

#include <kernel/clipboard.h>
#include <kernel/proc.h>
#include <kernel/tty.h>
 #include <kernel/input_focus.h>

#include <drivers/keyboard.h>
#include <drivers/vga.h>

#include <fs/yulafs.h>
#include <fs/pipe.h>
#include <fs/vfs.h>

#include <mm/heap.h>
#include <mm/pmm.h>

#include "shell.h"

#define LINE_INIT_CAP 256
#define ARGV_INIT_CAP 16
#define HIST_INIT_CAP 16
#define PATH_INIT_CAP 64

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

#define SHELL_KEY_COPY 0x04
#define C_SEL_BG 0x264F78
#define C_SEL_FG 0xFFFFFF

typedef struct {
    char** lines;
    int count;
    int cap;
    int view_idx;
    char* temp_line;
    size_t temp_cap;
} shell_history_t;

typedef struct {
    term_instance_t* term;
    shell_history_t* hist;
    char* line;
    char* cwd_path;

    int cursor_pos;
    int input_start_row;
    int input_rows;

    int sel_active;
    int sel_selecting;
    int sel_start_row;
    int sel_start_col;
    int sel_end_row;
    int sel_end_col;

    spinlock_t lock;
} shell_context_t;

extern uint32_t fb_width;
extern uint32_t fb_height;

static void hist_init(shell_history_t* h);
static void hist_destroy(shell_history_t* h);

static void shell_sel_normalize(term_instance_t* term, int sr, int sc, int er, int ec, int* out_sr, int* out_sc, int* out_er, int* out_ec);
static int  shell_sel_contains(int sr, int sc, int er, int ec, int row, int col);
static int  shell_copy_selection(shell_context_t* ctx);

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
    file_t* of = proc_fd_get(curr, oldfd);
    if (!of || !of->used) return -1;

    file_t* nf = 0;
    int newfd = proc_fd_alloc(curr, &nf);
    if (newfd < 0 || !nf) return -1;

    *nf = *of;
    if (nf->node) __sync_fetch_and_add(&nf->node->refs, 1);
    return newfd;
}

static int shell_dup2(int oldfd, int newfd) {
    task_t* curr = proc_current();
    if (newfd < 0) return -1;
    file_t* of = proc_fd_get(curr, oldfd);
    if (!of || !of->used) return -1;

    if (proc_fd_get(curr, newfd)) {
        vfs_close(newfd);
    }

    file_t* nf = 0;
    if (proc_fd_add_at(curr, newfd, &nf) < 0 || !nf) return -1;
    *nf = *of;
    if (nf->node) __sync_fetch_and_add(&nf->node->refs, 1);
    return newfd;
}

static int shell_create_pipe(int fds[2]) {
    vfs_node_t *r, *w;
    if (vfs_create_pipe(&r, &w) != 0) return -1;
    
    task_t* curr = proc_current();
    file_t* fr = 0;
    file_t* fw = 0;

    int r_fd = proc_fd_alloc(curr, &fr);
    if (r_fd < 0 || !fr) return -1;
    int w_fd = proc_fd_alloc(curr, &fw);
    if (w_fd < 0 || !fw) {
        file_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        proc_fd_remove(curr, r_fd, &tmp);
        return -1;
    }

    fr->node = r; fr->offset = 0; fr->used = 1;
    fw->node = w; fw->offset = 0; fw->used = 1;

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

static int line_ensure_cap(char** line, size_t* cap, size_t need) {
    if (!line || !cap) return 0;
    if (need <= *cap) return 1;
    size_t new_cap = *cap ? *cap : LINE_INIT_CAP;
    while (new_cap < need) new_cap *= 2;
    char* nl = (char*)krealloc(*line, new_cap);
    if (!nl) return 0;
    *line = nl;
    *cap = new_cap;
    return 1;
}

static int path_ensure_cap(char** path, size_t* cap, size_t need) {
    if (!path || !cap) return 0;
    if (need <= *cap) return 1;
    size_t new_cap = *cap ? *cap : PATH_INIT_CAP;
    while (new_cap < need) new_cap *= 2;
    char* np = (char*)krealloc(*path, new_cap);
    if (!np) return 0;
    *path = np;
    *cap = new_cap;
    return 1;
}

static int path_set(char** path, size_t* cap, const char* src) {
    if (!path || !cap || !src) return 0;
    size_t n = strlen(src) + 1;
    if (!path_ensure_cap(path, cap, n)) return 0;
    memcpy(*path, src, n);
    return 1;
}

static int path_append(char** path, size_t* cap, const char* src) {
    if (!path || !cap || !src) return 0;
    size_t a = (*path) ? strlen(*path) : 0;
    size_t b = strlen(src);
    if (!path_ensure_cap(path, cap, a + b + 1)) return 0;
    memcpy(*path + a, src, b + 1);
    return 1;
}

static void refresh_line(term_instance_t* term, const char* path, char* line, int cursor, int* input_start_row, int* input_rows) {
    int prompt_len = get_prompt_len(path);
    int line_len = strlen(line);
    int total_len = prompt_len + line_len;

    int cols = term ? term->cols : 0;
    if (cols <= 0) cols = TERM_W;

    int needed_rows = (total_len / cols) + 1;

    int start_row = *input_start_row;
    int prev_rows = *input_rows;
    if (prev_rows < 1) prev_rows = 1;
    if (needed_rows < 1) needed_rows = 1;
    int clear_rows = (needed_rows > prev_rows) ? needed_rows : prev_rows;

    for (int r = 0; r < clear_rows; r++) {
        int row = start_row + r;
        if (row < 0) break;
        term_clear_row(term, row);
    }

    term->row = start_row;
    term->col = 0;
    print_prompt_text(term, path);
    term_print(term, line);

    *input_rows = needed_rows;

    int visual_cursor = prompt_len + cursor;
    int cursor_row = visual_cursor / cols;
    int cursor_col = visual_cursor % cols;
    term->row = start_row + cursor_row;
    term->col = cursor_col;
    
    int visible_rows = term ? term->view_rows : 0;
    if (visible_rows <= 0) visible_rows = TERM_H;
    if (term->row < term->view_row || term->row >= term->view_row + visible_rows) {
         if (term->row >= visible_rows) 
             term->view_row = term->row - visible_rows + 1;
         else 
             term->view_row = 0;
    }
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

static void shell_cd(term_instance_t* term, const char* new_path, uint32_t* cwd_inode, char** path_str, size_t* path_cap) {
    int inode = yulafs_lookup(new_path);
    if (inode == -1) { term_print(term, "cd: no such directory\n"); return; }
    
    yfs_inode_t info; 
    yulafs_stat(inode, &info);
    
    if (info.type != YFS_TYPE_DIR) { term_print(term, "cd: not a directory\n"); return; }
    
    *cwd_inode = (uint32_t)inode;
    if (!path_str || !path_cap || !*path_str) return;

    if (new_path[0] == '/') {
        path_set(path_str, path_cap, new_path);
        return;
    }

    if (strcmp(new_path, "..") == 0) {
        int len = (int)strlen(*path_str);
        if (len <= 1) return;
        for (int i = len - 1; i >= 0; i--) {
            if ((*path_str)[i] == '/') {
                (*path_str)[i == 0 ? 1 : i] = '\0';
                break;
            }
        }
        return;
    }

    if (strlen(*path_str) > 1) path_append(path_str, path_cap, "/");
    path_append(path_str, path_cap, new_path);
}

static int parse_args(char* line, char*** out_args) {
    if (out_args) *out_args = 0;
    if (!line || !out_args) return 0;

    int cap = ARGV_INIT_CAP;
    if (cap < 2) cap = 2;
    char** args = (char**)kmalloc(sizeof(char*) * cap);
    if (!args) return 0;

    int count = 0;
    char* ptr = line;
    int in_quote = 0;

    while (*ptr) {
        while (*ptr == ' ') *ptr++ = 0;
        if (!*ptr) break;

        if (count + 1 >= cap) {
            int new_cap = cap * 2;
            char** na = (char**)krealloc(args, sizeof(char*) * new_cap);
            if (!na) { kfree(args); return 0; }
            args = na;
            cap = new_cap;
        }

        if (*ptr == '"') { in_quote = 1; ptr++; args[count++] = ptr; }
        else args[count++] = ptr;

        while (*ptr) {
            if (in_quote) {
                if (*ptr == '"') { *ptr++ = 0; in_quote = 0; break; }
            } else {
                if (*ptr == ' ') break;
            }
            ptr++;
        }
    }

    args[count] = 0;
    *out_args = args;
    return count;
}

static void hist_init(shell_history_t* h) {
    if (!h) return;
    memset(h, 0, sizeof(*h));
    h->cap = HIST_INIT_CAP;
    h->lines = (char**)kzalloc(sizeof(char*) * h->cap);
    h->temp_cap = LINE_INIT_CAP;
    h->temp_line = (char*)kzalloc(h->temp_cap);
    h->view_idx = -1;
}

static void hist_destroy(shell_history_t* h) {
    if (!h) return;
    if (h->lines) {
        for (int i = 0; i < h->count; i++) {
            if (h->lines[i]) kfree(h->lines[i]);
        }
        kfree(h->lines);
    }
    if (h->temp_line) kfree(h->temp_line);
    memset(h, 0, sizeof(*h));
    h->view_idx = -1;
}

static void hist_save_temp_line(shell_history_t* h, const char* line) {
    if (!h || !line) return;
    size_t n = strlen(line) + 1;
    if (n > h->temp_cap) {
        size_t new_cap = h->temp_cap ? h->temp_cap : LINE_INIT_CAP;
        while (new_cap < n) new_cap *= 2;
        char* t = (char*)krealloc(h->temp_line, new_cap);
        if (!t) return;
        h->temp_line = t;
        h->temp_cap = new_cap;
    }
    memcpy(h->temp_line, line, n);
}

static int hist_ensure_cap(shell_history_t* h, int need) {
    if (!h) return 0;
    if (need <= h->cap) return 1;
    int new_cap = h->cap ? h->cap : HIST_INIT_CAP;
    while (new_cap < need) new_cap *= 2;
    char** new_lines = (char**)krealloc(h->lines, sizeof(char*) * new_cap);
    if (!new_lines) return 0;
    memset(new_lines + h->cap, 0, sizeof(char*) * (new_cap - h->cap));
    h->lines = new_lines;
    h->cap = new_cap;
    return 1;
}

static void hist_add(shell_history_t* h, const char* cmd) {
    if (!h || !cmd) return;
    if (strlen(cmd) == 0) return;
    if (h->count > 0 && h->lines[h->count - 1] && strcmp(h->lines[h->count - 1], cmd) == 0) return;
    if (!hist_ensure_cap(h, h->count + 1)) return;
    size_t n = strlen(cmd) + 1;
    char* s = (char*)kmalloc(n);
    if (!s) return;
    memcpy(s, cmd, n);
    h->lines[h->count++] = s;
    h->view_idx = -1;
}

static const char* hist_get_prev(shell_history_t* h) {
    if (!h || h->count == 0) return 0;
    if (h->view_idx == -1) h->view_idx = h->count - 1;
    else {
        if (h->view_idx == 0) return 0;
        h->view_idx--;
    }
    return h->lines[h->view_idx];
}

static const char* hist_get_next(shell_history_t* h) {
    if (!h || h->count == 0 || h->view_idx == -1) return 0;
    if (h->view_idx == h->count - 1) { h->view_idx = -1; return ""; }
    h->view_idx++;
    return h->lines[h->view_idx];
}

static task_t* spawn_command(const char* cmd, int argc, char** argv) {
    task_t* child = proc_spawn_elf(cmd, argc, argv);
    if (child) return child;

    size_t cmd_len = strlen(cmd);

    char* tmp = (char*)kmalloc(cmd_len + 5);
    if (tmp) {
        memcpy(tmp, cmd, cmd_len);
        memcpy(tmp + cmd_len, ".exe", 5);
        child = proc_spawn_elf(tmp, argc, argv);
        kfree(tmp);
        if (child) return child;
    }

    if (cmd[0] != '/') {
        const char* bin = "/bin/";
        size_t bin_len = 5;

        tmp = (char*)kmalloc(bin_len + cmd_len + 1);
        if (tmp) {
            memcpy(tmp, bin, bin_len);
            memcpy(tmp + bin_len, cmd, cmd_len + 1);
            child = proc_spawn_elf(tmp, argc, argv);
            if (child) { kfree(tmp); return child; }

            char* tmp_exe = (char*)kmalloc(bin_len + cmd_len + 5);
            if (tmp_exe) {
                memcpy(tmp_exe, bin, bin_len);
                memcpy(tmp_exe + bin_len, cmd, cmd_len);
                memcpy(tmp_exe + bin_len + cmd_len, ".exe", 5);
                child = proc_spawn_elf(tmp_exe, argc, argv);
                kfree(tmp_exe);
            }
            kfree(tmp);
        }
    }
    return child;
}

static int shell_run_pipeline(term_instance_t* term, int shell_pid, char** args, int arg_count) {
    if (!term || !args || arg_count <= 0) return 0;

    int cmd_count = 1;
    for (int i = 0; i < arg_count; i++) {
        if (strcmp(args[i], "|") == 0) cmd_count++;
    }
    if (cmd_count <= 1) return 0;

    for (int i = 0; i < arg_count; i++) {
        if (strcmp(args[i], "|") == 0) {
            if (i == 0 || i == arg_count - 1) {
                spinlock_acquire(&term->lock);
                term_print(term, "Invalid pipeline\n");
                spinlock_release(&term->lock);
                return 1;
            }
            if (i > 0 && strcmp(args[i - 1], "|") == 0) {
                spinlock_acquire(&term->lock);
                term_print(term, "Invalid pipeline\n");
                spinlock_release(&term->lock);
                return 1;
            }
        }
    }

    int* cmd_start = (int*)kmalloc(sizeof(int) * cmd_count);
    int* cmd_argc = (int*)kmalloc(sizeof(int) * cmd_count);
    task_t** tasks = (task_t**)kzalloc(sizeof(task_t*) * cmd_count);
    if (!cmd_start || !cmd_argc || !tasks) {
        if (cmd_start) kfree(cmd_start);
        if (cmd_argc) kfree(cmd_argc);
        if (tasks) kfree(tasks);
        spinlock_acquire(&term->lock);
        term_print(term, "Out of memory\n");
        spinlock_release(&term->lock);
        return 1;
    }

    int cmd_idx = 0;
    int start = 0;
    cmd_start[0] = 0;
    for (int i = 0; i < arg_count; i++) {
        if (strcmp(args[i], "|") == 0) {
            args[i] = 0;
            cmd_argc[cmd_idx] = i - start;
            cmd_idx++;
            start = i + 1;
            cmd_start[cmd_idx] = start;
        }
    }
    cmd_argc[cmd_idx] = arg_count - start;

    for (int i = 0; i < cmd_count; i++) {
        if (cmd_argc[i] <= 0 || !args[cmd_start[i]]) {
            spinlock_acquire(&term->lock);
            term_print(term, "Invalid pipeline\n");
            spinlock_release(&term->lock);
            kfree(cmd_start);
            kfree(cmd_argc);
            kfree(tasks);
            return 1;
        }
    }

    int saved_stdout = shell_dup(1);
    int saved_stdin = shell_dup(0);
    if (saved_stdout < 0 || saved_stdin < 0) {
        spinlock_acquire(&term->lock);
        term_print(term, "Pipe setup failed\n");
        spinlock_release(&term->lock);
        if (saved_stdout >= 0) vfs_close(saved_stdout);
        if (saved_stdin >= 0) vfs_close(saved_stdin);
        kfree(cmd_start);
        kfree(cmd_argc);
        kfree(tasks);
        return 1;
    }

    int prev_read = -1;

    for (int i = 0; i < cmd_count; i++) {
        int next_read = -1;
        int write_end = -1;

        if (i < cmd_count - 1) {
            int pfds[2];
            if (shell_create_pipe(pfds) != 0) {
                spinlock_acquire(&term->lock);
                term_print(term, "Pipe creation failed\n");
                spinlock_release(&term->lock);
                break;
            }
            next_read = pfds[0];
            write_end = pfds[1];
        }

        int in_fd = (prev_read >= 0) ? prev_read : saved_stdin;
        int out_fd = (i == cmd_count - 1) ? saved_stdout : write_end;

        shell_dup2(in_fd, 0);
        shell_dup2(out_fd, 1);

        task_t* t = spawn_command(args[cmd_start[i]], cmd_argc[i], &args[cmd_start[i]]);
        tasks[i] = t;
        if (!t) {
            spinlock_acquire(&term->lock);
            term_print(term, "Command not found: ");
            term_print(term, args[cmd_start[i]]);
            term_print(term, "\n");
            spinlock_release(&term->lock);
        }

        shell_dup2(saved_stdin, 0);
        shell_dup2(saved_stdout, 1);

        if (prev_read >= 0) {
            vfs_close(prev_read);
            prev_read = -1;
        }

        if (i < cmd_count - 1) {
            vfs_close(write_end);
            prev_read = next_read;
        }

        if (!t) break;
    }

    if (prev_read >= 0) vfs_close(prev_read);

    task_t* first_task = 0;
    for (int i = 0; i < cmd_count; i++) {
        if (tasks[i]) {
            first_task = tasks[i];
            break;
        }
    }

    if (first_task) {
        input_focus_set_pid(first_task->pid);
    }

    for (int i = 0; i < cmd_count; i++) {
        if (tasks[i]) proc_wait(tasks[i]->pid);
    }

    if (first_task) {
        input_focus_set_pid((uint32_t)shell_pid);
    }

    vfs_close(saved_stdout);
    vfs_close(saved_stdin);
    kfree(cmd_start);
    kfree(cmd_argc);
    kfree(tasks);
    return 1;
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

static void shell_sel_normalize(term_instance_t* term, int sr, int sc, int er, int ec, int* out_sr, int* out_sc, int* out_er, int* out_ec) {
     if (sr > er || (sr == er && sc > ec)) {
         int tr = sr; sr = er; er = tr;
         int tc = sc; sc = ec; ec = tc;
     }
     int cols = term ? term->cols : 0;
     if (cols <= 0) cols = TERM_W;
     int max_row = term ? term->max_row : 0;
     if (max_row < 0) max_row = 0;
     if (sr < 0) sr = 0;
     if (er < 0) er = 0;
     if (sr > max_row) sr = max_row;
     if (er > max_row) er = max_row;
     if (sc < 0) sc = 0;
     if (sc >= cols) sc = cols - 1;
     if (ec < 0) ec = 0;
     if (ec >= cols) ec = cols - 1;
     if (out_sr) *out_sr = sr;
     if (out_sc) *out_sc = sc;
     if (out_er) *out_er = er;
     if (out_ec) *out_ec = ec;
}

static __attribute__((unused)) int shell_sel_contains(int sr, int sc, int er, int ec, int row, int col) {
    if (row < sr || row > er) return 0;
    if (sr == er) return (col >= sc && col <= ec);
    if (row == sr) return col >= sc;
    if (row == er) return col <= ec;
    return 1;
}

static void shell_kbd_sel_step(shell_context_t* ctx, term_instance_t* term, int keycode) {
    if (!ctx || !term) return;

    spinlock_acquire(&term->lock);

    int cols = term->cols;
    if (cols <= 0) cols = TERM_W;
    int view_rows = term->view_rows;
    if (view_rows <= 0) view_rows = TERM_H;
    if (view_rows < 1) view_rows = 1;

    int max_row = term->max_row;
    if (max_row < 0) max_row = 0;

    int r;
    int c;
    if (ctx->sel_active) {
        r = ctx->sel_end_row;
        c = ctx->sel_end_col;
    } else {
        int at_bottom = (term->view_row + view_rows) >= term->row;
        if (at_bottom) {
            r = term->row;
            c = term->col;
        } else {
            r = term->view_row + view_rows - 1;
            c = 0;
        }

        if (r < 0) r = 0;
        if (r > max_row) r = max_row;
        if (c < 0) c = 0;
        if (c >= cols) c = cols - 1;

        ctx->sel_active = 1;
        ctx->sel_selecting = 0;
        ctx->sel_start_row = r;
        ctx->sel_start_col = c;
        ctx->sel_end_row = r;
        ctx->sel_end_col = c;
    }

    if (r < 0) r = 0;
    if (r > max_row) r = max_row;
    if (c < 0) c = 0;
    if (c >= cols) c = cols - 1;

    if (keycode == 0x82) {
        if (c > 0) c--;
        else if (r > 0) { r--; c = cols - 1; }
    } else if (keycode == 0x83) {
        if (c < cols - 1) c++;
        else if (r < max_row) { r++; c = 0; }
    } else if (keycode == 0x80) {
        if (r > 0) r--;
    } else if (keycode == 0x81) {
        if (r < max_row) r++;
    } else if (keycode == 0x86) {
        while (!(r == 0 && c == 0)) {
            int pr = r;
            int pc = c;
            if (pc > 0) pc--; else { pr--; pc = cols - 1; }
            char ch;
            term_get_cell(term, pr, pc, &ch, 0, 0);
            r = pr; c = pc;
            if (ch != ' ') break;
        }
        while (!(r == 0 && c == 0)) {
            int pr = r;
            int pc = c;
            if (pc > 0) pc--; else { pr--; pc = cols - 1; }
            char ch;
            term_get_cell(term, pr, pc, &ch, 0, 0);
            if (ch == ' ') break;
            r = pr; c = pc;
        }
    } else if (keycode == 0x87) {
        while (!(r == max_row && c == cols - 1)) {
            int pr = r;
            int pc = c;
            if (pc < cols - 1) pc++; else { if (pr >= max_row) break; pr++; pc = 0; }
            char ch;
            term_get_cell(term, pr, pc, &ch, 0, 0);
            r = pr; c = pc;
            if (ch != ' ') break;
        }
        while (!(r == max_row && c == cols - 1)) {
            int pr = r;
            int pc = c;
            if (pc < cols - 1) pc++; else { if (pr >= max_row) break; pr++; pc = 0; }
            char ch;
            term_get_cell(term, pr, pc, &ch, 0, 0);
            if (ch == ' ') break;
            r = pr; c = pc;
        }
    }

    if (r < 0) r = 0;
    if (r > max_row) r = max_row;
    if (c < 0) c = 0;
    if (c >= cols) c = cols - 1;

    ctx->sel_end_row = r;
    ctx->sel_end_col = c;

    if (ctx->sel_end_row < term->view_row) term->view_row = ctx->sel_end_row;
    if (ctx->sel_end_row >= term->view_row + view_rows) term->view_row = ctx->sel_end_row - view_rows + 1;
    if (term->view_row < 0) term->view_row = 0;
    if (term->view_row > max_row) term->view_row = max_row;

    spinlock_release(&term->lock);
}

static int shell_copy_selection(shell_context_t* ctx) {
    if (!ctx || !ctx->term) return 0;

     int active = 0;
     int sr = 0, sc = 0, er = 0, ec = 0;
     spinlock_acquire(&ctx->lock);
     active = ctx->sel_active;
     sr = ctx->sel_start_row;
     sc = ctx->sel_start_col;
     er = ctx->sel_end_row;
     ec = ctx->sel_end_col;
     spinlock_release(&ctx->lock);

     if (!active) return 0;

     shell_sel_normalize(ctx->term, sr, sc, er, ec, &sr, &sc, &er, &ec);

     const int cap = 4096;
     char* out = (char*)kmalloc(cap);
     if (!out) return 0;

     int pos = 0;
     spinlock_acquire(&ctx->term->lock);

     int cols = ctx->term->cols;
     if (cols <= 0) cols = TERM_W;

     for (int r = sr; r <= er && pos < cap - 1; r++) {
         int c0 = (r == sr) ? sc : 0;
         int c1 = (r == er) ? ec : (cols - 1);
         for (int c = c0; c <= c1 && pos < cap - 1; c++) {
             char ch;
             term_get_cell(ctx->term, r, c, &ch, 0, 0);
             out[pos++] = ch;
         }
         if (r != er && pos < cap - 1) out[pos++] = '\n';
     }

     spinlock_release(&ctx->term->lock);

     clipboard_set(out, pos);
     kfree(out);
     return 1;
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

    spinlock_init(&ctx->lock);

    ctx->term = my_term; ctx->hist = my_hist;
    hist_init(my_hist);

    int cols = (int)(fb_width / 8);
    int view_rows = (int)(fb_height / 16);
    if (cols < 1) cols = 1;
    if (view_rows < 1) view_rows = 1;
    my_term->cols = cols;
    my_term->view_rows = view_rows;

    term_init(my_term);

    task_t* self = proc_current();
    self->terminal = my_term;
    self->term_mode = 1;
    my_term->curr_fg = C_TEXT; my_term->curr_bg = C_BG;

    input_focus_set_pid((uint32_t)self->pid);
    tty_set_terminal(my_term);

    spinlock_acquire(&my_term->lock);
    term_putc(my_term, 0x0C);
    spinlock_release(&my_term->lock);

    size_t line_cap = LINE_INIT_CAP;
    char* line = (char*)kzalloc(line_cap);
    int line_len = 0; int cursor_pos = 0;
    if (!line) {
        term_destroy(my_term);
        hist_destroy(my_hist);
        kfree(my_term);
        kfree(my_hist);
        kfree(ctx);
        return;
    }
    ctx->line = line;

    size_t cwd_path_cap = PATH_INIT_CAP;
    ctx->cwd_path = (char*)kzalloc(cwd_path_cap);
    if (!ctx->cwd_path) {
        term_destroy(my_term);
        hist_destroy(my_hist);
        kfree(line);
        kfree(my_term);
        kfree(my_hist);
        kfree(ctx);
        return;
    }

    path_set(&ctx->cwd_path, &cwd_path_cap, "/home");
    uint32_t cwd_inode = yulafs_lookup("/home");
    if ((int)cwd_inode == -1) {
        cwd_inode = 1;
        path_set(&ctx->cwd_path, &cwd_path_cap, "/");
    }

    int kbd_fd = vfs_open("/dev/kbd", 0);
    vfs_open("/dev/console", 1); 
    vfs_open("/dev/console", 1); 

    spinlock_acquire(&my_term->lock);
    print_prompt_text(my_term, ctx->cwd_path);
    spinlock_release(&my_term->lock);
    ctx->input_start_row = my_term->row;
    ctx->input_rows = 1;
    ctx->cursor_pos = 0;
    if (yulafs_lookup("/bin") == -1) yulafs_mkdir("/bin");
    if (yulafs_lookup("/home") == -1) yulafs_mkdir("/home");

    while (1) {
        proc_current()->cwd_inode = cwd_inode;
        char c = 0;
        int bytes_read = vfs_read(kbd_fd, &c, 1);

        if (bytes_read > 0) {

            if ((uint8_t)c == (uint8_t)SHELL_KEY_COPY) {
                shell_copy_selection(ctx);

                spinlock_acquire(&ctx->lock);
                ctx->sel_active = 0;
                ctx->sel_selecting = 0;
                ctx->sel_start_row = 0;
                ctx->sel_start_col = 0;
                ctx->sel_end_row = 0;
                ctx->sel_end_col = 0;
                spinlock_release(&ctx->lock);
                continue;
            }

            spinlock_acquire(&ctx->lock);

            if (c == '\n') {

                spinlock_acquire(&my_term->lock);
                int visual_end = get_prompt_len(ctx->cwd_path) + line_len;
                int cols = my_term->cols;
                if (cols <= 0) cols = TERM_W;
                my_term->row = ctx->input_start_row + (visual_end / cols);
                my_term->col = visual_end % cols;
                term_putc(my_term, '\n');
                spinlock_release(&my_term->lock);

                line[line_len] = '\0';
                hist_add(my_hist, line);

                char** args = 0;
                int arg_count = parse_args(line, &args);

                spinlock_release(&ctx->lock);

                int measure_time = 0;
                uint32_t start_ticks = 0;
                int should_exit = 0;

                if (arg_count > 0 && strcmp(args[0], "time") == 0) {
                    if (arg_count < 2) {
                        spinlock_acquire(&my_term->lock);
                        term_print(my_term, "Usage: time <command>\n");
                        spinlock_release(&my_term->lock);
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
                    if (!shell_run_pipeline(my_term, (int)self->pid, args, arg_count)) {
                        if (strcmp(args[0], "help") == 0) {
                            spinlock_acquire(&my_term->lock);
                            term_print(my_term, "Commands: ls, cd, pwd, mkdir, run, clear, exit, ps, kill\n");
                            spinlock_release(&my_term->lock);
                        } else if (strcmp(args[0], "ls") == 0) {
                            spinlock_acquire(&my_term->lock);
                            shell_ls(my_term, (arg_count > 1) ? args[1] : 0, cwd_inode);
                            spinlock_release(&my_term->lock);
                        } else if (strcmp(args[0], "cd") == 0) {
                            spinlock_acquire(&my_term->lock);
                            shell_cd(my_term, (arg_count > 1) ? args[1] : "/", &cwd_inode, &ctx->cwd_path, &cwd_path_cap);
                            spinlock_release(&my_term->lock);
                        } else if (strcmp(args[0], "pwd") == 0) {
                            spinlock_acquire(&my_term->lock);
                            term_print(my_term, ctx->cwd_path);
                            term_print(my_term, "\n");
                            spinlock_release(&my_term->lock);
                        } else if (strcmp(args[0], "clear") == 0) {
                            spinlock_acquire(&my_term->lock);
                            term_putc(my_term, 0x0C);
                            spinlock_release(&my_term->lock);
                        } else if (strcmp(args[0], "mkdir") == 0 && arg_count > 1) {
                            yulafs_mkdir(args[1]);
                        } else if (strcmp(args[0], "exit") == 0) {
                            should_exit = 1;
                            goto loop_end;
                        } else if (strcmp(args[0], "ps") == 0) {
                            spinlock_acquire(&my_term->lock);
                            shell_ps(my_term);
                            spinlock_release(&my_term->lock);
                        } else if (strcmp(args[0], "kill") == 0 && arg_count > 1) {
                            int ret; int pid = atoi(args[1]);
                            __asm__ volatile("int $0x80" : "=a"(ret) : "a"(9), "b"(pid));
                            spinlock_acquire(&my_term->lock);
                            term_print(my_term, (ret == 0) ? "Killed\n" : "Fail\n");
                            spinlock_release(&my_term->lock);
                        } else if (strcmp(args[0], "rm") == 0 && arg_count > 1) {
                            int ret;
                            ret = yulafs_unlink(args[1]);
                            spinlock_acquire(&my_term->lock);
                            if (ret == 0) term_print(my_term, "Deleted\n");
                            else term_print(my_term, "Fail\n");
                            spinlock_release(&my_term->lock);
                        } else if (strcmp(args[0], "run") == 0) {
                            if (arg_count < 2 || !args[1]) {
                                spinlock_acquire(&my_term->lock);
                                term_print(my_term, "Usage: run <command> [args...]\n");
                                spinlock_release(&my_term->lock);
                            } else {
                                task_t* child = spawn_command(args[1], arg_count - 1, &args[1]);
                                if (child) {
                                    spinlock_acquire(&my_term->lock);
                                    term_print(my_term, "Started PID ");
                                    term_print(my_term, itoa(child->pid));
                                    term_print(my_term, "\n");
                                    spinlock_release(&my_term->lock);
                                } else {
                                    spinlock_acquire(&my_term->lock);
                                    term_print(my_term, "Command not found: ");
                                    term_print(my_term, args[1]);
                                    term_print(my_term, "\n");
                                    spinlock_release(&my_term->lock);
                                }
                            }
                        } else {
                            task_t* child = spawn_command(args[0], arg_count, args);
                            if (child) {
                                input_focus_set_pid(child->pid);
                                proc_wait(child->pid);
                                input_focus_set_pid((uint32_t)self->pid);
                            } else {
                                spinlock_acquire(&my_term->lock);
                                term_print(my_term, "Command not found: ");
                                term_print(my_term, args[0]);
                                term_print(my_term, "\n");
                                spinlock_release(&my_term->lock);
                            }
                        }
                    }
                }

                if (measure_time) {
                    spinlock_acquire(&my_term->lock);
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
                    spinlock_release(&my_term->lock);
                }

                loop_end: 

                if (args) kfree(args);
                if (should_exit) break;

                spinlock_acquire(&ctx->lock);

                spinlock_acquire(&my_term->lock);
                my_term->curr_fg = C_TEXT;
                my_term->curr_bg = C_BG;

                if (my_term->col > 0) term_putc(my_term, '\n');
                
                line_len = 0; 
                cursor_pos = 0; 
                line[0] = 0;
                
                print_prompt_text(my_term, ctx->cwd_path);
                ctx->input_start_row = my_term->row;
                ctx->input_rows = 1;
                ctx->cursor_pos = 0;

                spinlock_release(&my_term->lock);
            } 
            else if (c == 0x13) { 
                const char* h_str = hist_get_prev(my_hist);
                if (h_str) {
                    if (my_hist->view_idx == my_hist->count - 1) hist_save_temp_line(my_hist, line);
                    size_t n = strlen(h_str);
                    if (line_ensure_cap(&line, &line_cap, n + 1)) {
                        ctx->line = line;
                        memcpy(line, h_str, n + 1);
                        line_len = (int)n;
                        cursor_pos = line_len;
                        ctx->cursor_pos = cursor_pos;
                        spinlock_acquire(&my_term->lock);
                        refresh_line(my_term, ctx->cwd_path, line, cursor_pos, &ctx->input_start_row, &ctx->input_rows);
                        spinlock_release(&my_term->lock);
                    }
                }
            }
            else if (c == 0x14) { 
                const char* h_str = hist_get_next(my_hist);
                if (h_str) {
                    const char* src = h_str;
                    if (h_str[0] == 0 && my_hist->view_idx == -1) src = my_hist->temp_line;
                    size_t n = strlen(src);
                    if (line_ensure_cap(&line, &line_cap, n + 1)) {
                        ctx->line = line;
                        memcpy(line, src, n + 1);
                        line_len = (int)n;
                        cursor_pos = line_len;
                        ctx->cursor_pos = cursor_pos;
                        spinlock_acquire(&my_term->lock);
                        refresh_line(my_term, ctx->cwd_path, line, cursor_pos, &ctx->input_start_row, &ctx->input_rows);
                        spinlock_release(&my_term->lock);
                    }
                }
            }
            else if ((uint8_t)c == 0x82 || (uint8_t)c == 0x83 || (uint8_t)c == 0x86 || (uint8_t)c == 0x87) {
                shell_kbd_sel_step(ctx, my_term, (uint8_t)c);
            }
            else if ((uint8_t)c == 0x1B) {
                if (ctx->sel_active) {
                    ctx->sel_active = 0;
                    ctx->sel_selecting = 0;
                    ctx->sel_start_row = 0;
                    ctx->sel_start_col = 0;
                    ctx->sel_end_row = 0;
                    ctx->sel_end_col = 0;
                } else {
                    if (line_len > 0 || cursor_pos > 0) {
                        line[0] = 0;
                        line_len = 0;
                        cursor_pos = 0;
                        ctx->cursor_pos = cursor_pos;
                        spinlock_acquire(&my_term->lock);
                        refresh_line(my_term, ctx->cwd_path, line, cursor_pos, &ctx->input_start_row, &ctx->input_rows);
                        spinlock_release(&my_term->lock);
                    }
                }
            }
            else if ((uint8_t)c == 0x88) {
                if (cursor_pos > 0) {
                    for (int i = cursor_pos; i <= line_len; i++) {
                        line[i - cursor_pos] = line[i];
                    }
                    line_len -= cursor_pos;
                    cursor_pos = 0;
                    ctx->cursor_pos = cursor_pos;
                    spinlock_acquire(&my_term->lock);
                    refresh_line(my_term, ctx->cwd_path, line, cursor_pos, &ctx->input_start_row, &ctx->input_rows);
                    spinlock_release(&my_term->lock);
                }
            }
            else if ((uint8_t)c == 0x89) {
                if (cursor_pos < line_len) {
                    line[cursor_pos] = 0;
                    line_len = cursor_pos;
                    ctx->cursor_pos = cursor_pos;
                    spinlock_acquire(&my_term->lock);
                    refresh_line(my_term, ctx->cwd_path, line, cursor_pos, &ctx->input_start_row, &ctx->input_rows);
                    spinlock_release(&my_term->lock);
                }
            }
            else if (c == 0x11) {
                if (cursor_pos > 0) {
                    cursor_pos--; ctx->cursor_pos = cursor_pos;
                    spinlock_acquire(&my_term->lock);
                    refresh_line(my_term, ctx->cwd_path, line, cursor_pos, &ctx->input_start_row, &ctx->input_rows);
                    spinlock_release(&my_term->lock);
                }
            }
            else if (c == 0x12) {
                if (cursor_pos < line_len) {
                    cursor_pos++; ctx->cursor_pos = cursor_pos;
                    spinlock_acquire(&my_term->lock);
                    refresh_line(my_term, ctx->cwd_path, line, cursor_pos, &ctx->input_start_row, &ctx->input_rows);
                    spinlock_release(&my_term->lock);
                }
            }
            else if ((uint8_t)c == 0x80) {
                if (ctx->sel_active) {
                    shell_kbd_sel_step(ctx, my_term, 0x80);
                } else if (my_term->view_row > 0) {
                    spinlock_acquire(&my_term->lock);
                    my_term->view_row--;
                    spinlock_release(&my_term->lock);
                }
            }
            else if ((uint8_t)c == 0x81) { 
                if (ctx->sel_active) {
                    shell_kbd_sel_step(ctx, my_term, 0x81);
                } else {
                    spinlock_acquire(&my_term->lock);
                    int visible_rows = my_term->view_rows;
                    if (visible_rows < 1) visible_rows = 1;
                    if (my_term->view_row + visible_rows <= my_term->max_row) { my_term->view_row++; }
                    spinlock_release(&my_term->lock);
                }
            }
            else if (c == '\b') {
                if (cursor_pos > 0) {
                    for (int i = cursor_pos; i < line_len; i++) line[i-1] = line[i];
                    line_len--; cursor_pos--; line[line_len] = 0;
                    ctx->cursor_pos = cursor_pos;
                    spinlock_acquire(&my_term->lock);
                    refresh_line(my_term, ctx->cwd_path, line, cursor_pos, &ctx->input_start_row, &ctx->input_rows);
                    spinlock_release(&my_term->lock);
                }
            } 
            else if ((uint8_t)c >= 32) {
                if (line_ensure_cap(&line, &line_cap, (size_t)line_len + 2)) {
                    ctx->line = line;
                    for (int i = line_len; i > cursor_pos; i--) line[i] = line[i-1];
                    line[cursor_pos] = c;
                    line_len++; cursor_pos++; line[line_len] = 0;
                    ctx->cursor_pos = cursor_pos;
                    spinlock_acquire(&my_term->lock);
                    refresh_line(my_term, ctx->cwd_path, line, cursor_pos, &ctx->input_start_row, &ctx->input_rows);
                    spinlock_release(&my_term->lock);
                }
            }
            spinlock_release(&ctx->lock);
        }
    }

    tty_set_terminal(0);
    hist_destroy(my_hist);
    kfree(my_hist);
    kfree(line);
    kfree(ctx->cwd_path);
    term_destroy(my_term);
    kfree(my_term);
    kfree(ctx);
    sys_exit();
 }