// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <yula.h>

#define BUF_SIZE    4096
#define PATH_MAX    256
#define MAX_LINE    2048

#define ANSI_RESET  "\x1b[0m"
#define ANSI_TEXT   "\x1b[0m"
#define ANSI_MATCH  "\x1b[91m"
#define ANSI_FILE   "\x1b[94m"
#define ANSI_LNUM   "\x1b[92m"
#define ANSI_SEP    "\x1b[90m"
#define ANSI_ERR    "\x1b[91m"

int opt_recursive = 0;
int opt_show_filename = 1;
int opt_line_number = 1;

void print_match_line(const char* filename, int line_num, const char* line, const char* pattern) {
    const char* ptr = line;
    const char* match = strstr(line, pattern);
    
    if (!match) return;

    if (opt_show_filename && filename) {
        puts(ANSI_FILE);
        puts(filename);
        puts(ANSI_SEP);
        puts(":");
    }

    if (opt_line_number) {
        puts(ANSI_LNUM);
        print_dec(line_num);
        puts(ANSI_SEP);
        puts(":");
    }
    
    if ((opt_show_filename && filename) || opt_line_number) puts(" ");

    int pat_len = strlen(pattern);
    while ((match = strstr(ptr, pattern))) {
        while (ptr < match) {
            char tmp[2] = {*ptr++, 0};
            puts(ANSI_TEXT);
            puts(tmp);
        }
        
        puts(ANSI_MATCH);
        puts(pattern);
        ptr += pat_len;
    }
    
    puts(ANSI_TEXT);
    puts(ptr);
    puts(ANSI_RESET);
    puts("\n");
}

void grep_from_fd(int fd, const char* filename, const char* pattern) {
    char chunk[BUF_SIZE];
    char line_buf[MAX_LINE];
    int line_pos = 0;
    int line_num = 1;
    int n;

    while ((n = read(fd, chunk, BUF_SIZE)) > 0) {
        for (int i = 0; i < n; i++) {
            char c = chunk[i];
            if (c == '\n') {
                line_buf[line_pos] = 0;
                if (strstr(line_buf, pattern)) {
                    print_match_line(filename, line_num, line_buf, pattern);
                }
                line_pos = 0;
                line_num++;
            } else {
                if (line_pos < MAX_LINE - 1) {
                    if (c != '\r') line_buf[line_pos++] = c;
                }
            }
        }
    }
    if (line_pos > 0) {
        line_buf[line_pos] = 0;
        if (strstr(line_buf, pattern)) print_match_line(filename, line_num, line_buf, pattern);
    }
}

void grep_file(const char* path, const char* pattern) {
    int fd = open(path, 0);
    if (fd < 0) {
        puts(ANSI_ERR);
        printf("grep: %s: No such file or directory\n", path);
        puts(ANSI_RESET);
        return;
    }
    grep_from_fd(fd, path, pattern);
    close(fd);
}

typedef struct {
    uint32_t inode;
    char name[60];
} yfs_dirent_t;

void process_path(const char* path, const char* pattern) {
    stat_t st;
    if (stat(path, &st) != 0) {
        puts(ANSI_ERR);
        printf("grep: %s: Cannot stat\n", path);
        puts(ANSI_RESET);
        return;
    }

    if (st.type == 1) {
        grep_file(path, pattern);
    } else if (st.type == 2) {
        if (!opt_recursive) {
            puts(ANSI_SEP);
            printf("grep: %s: Is a directory\n", path);
            puts(ANSI_RESET);
            return;
        }
        
        int fd = open(path, 0);
        if (fd < 0) return;

        yfs_dirent_t ent;
        while (read(fd, &ent, sizeof(yfs_dirent_t)) > 0) {
            if (ent.inode == 0) continue;
            if (strcmp(ent.name, ".") == 0 || strcmp(ent.name, "..") == 0) continue;

            char new_path[PATH_MAX];
            strcpy(new_path, path);
            int len = strlen(path);
            if (path[len - 1] != '/') strcat(new_path, "/");
            strcat(new_path, ent.name);

            process_path(new_path, pattern);
        }
        close(fd);
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: grep [-r] <pattern> [file...]\n");
        return 1;
    }

    int arg_idx = 1;
    if (strcmp(argv[1], "-r") == 0) {
        opt_recursive = 1;
        arg_idx++;
    }

    if (arg_idx >= argc) {
        printf("grep: pattern missing\n");
        return 1;
    }

    const char* pattern = argv[arg_idx++];

    if (arg_idx >= argc) {
        if (opt_recursive) {
            process_path(".", pattern);
        } else {
            grep_from_fd(0, 0, pattern);
        }
    } else {
        for (; arg_idx < argc; arg_idx++) {
            process_path(argv[arg_idx], pattern);
        }
    }

    puts(ANSI_RESET);
    return 0;
}