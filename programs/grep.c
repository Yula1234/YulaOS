#include <yula.h>

#define BUF_SIZE    4096
#define PATH_MAX    256
#define MAX_LINE    2048

#define C_BG        0x141414
#define C_TEXT      0xD4D4D4
#define C_MATCH     0xF44747
#define C_FILE      0x569CD6
#define C_LNUM      0x6A9955
#define C_SEP       0x606060
#define C_ERR       0xF44747

int opt_recursive = 0;
int opt_show_filename = 1;
int opt_line_number = 1;

const char* strstr(const char* haystack, const char* needle) {
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        if (*haystack == *needle) {
            const char* h = haystack;
            const char* n = needle;
            while (*h && *n && *h == *n) { h++; n++; }
            if (!*n) return haystack;
        }
    }
    return 0;
}

void print_match_line(const char* filename, int line_num, const char* line, const char* pattern) {
    const char* ptr = line;
    const char* match = strstr(line, pattern);
    
    if (!match) return;

    if (opt_show_filename && filename) {
        set_console_color(C_FILE, C_BG);
        print(filename);
        set_console_color(C_SEP, C_BG);
        print(":");
    }

    if (opt_line_number) {
        set_console_color(C_LNUM, C_BG);
        print_dec(line_num);
        set_console_color(C_SEP, C_BG);
        print(":");
    }
    
    if ((opt_show_filename && filename) || opt_line_number) print(" ");

    int pat_len = strlen(pattern);
    while ((match = strstr(ptr, pattern))) {
        while (ptr < match) {
            char tmp[2] = {*ptr++, 0};
            set_console_color(C_TEXT, C_BG);
            print(tmp);
        }
        
        set_console_color(C_MATCH, C_BG);
        print(pattern);
        ptr += pat_len;
    }
    
    set_console_color(C_TEXT, C_BG);
    print(ptr);
    print("\n");
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
        set_console_color(C_ERR, C_BG);
        printf("grep: %s: No such file or directory\n", path);
        set_console_color(C_TEXT, C_BG);
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
        set_console_color(C_ERR, C_BG);
        printf("grep: %s: Cannot stat\n", path);
        set_console_color(C_TEXT, C_BG);
        return;
    }

    if (st.type == 1) {
        grep_file(path, pattern);
    } else if (st.type == 2) {
        if (!opt_recursive) {
            set_console_color(C_SEP, C_BG);
            printf("grep: %s: Is a directory\n", path);
            set_console_color(C_TEXT, C_BG);
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

    set_console_color(C_TEXT, C_BG);
    return 0;
}