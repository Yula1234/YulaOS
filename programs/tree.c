// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <yula.h>

#define MAX_PATH 256

#define C_DIR     0x569CD6
#define C_FILE    0xD4D4D4
#define C_EXE     0xB5CEA8
#define C_ASM     0xCE9178
#define C_TREE    0x606060
#define C_BG      0x141414

typedef struct {
    uint32_t inode;
    char name[60];
} yfs_dirent_t;

typedef struct {
    char name[64];
    int is_dir;
    int size;
} Entry;

int total_dirs = 0;
int total_files = 0;

uint32_t get_color(const char* name, int is_dir) {
    if (is_dir) return C_DIR;
    
    int len = strlen(name);
    if (len > 4 && strcmp(name + len - 4, ".exe") == 0) return C_EXE;
    if (len > 4 && strcmp(name + len - 4, ".asm") == 0) return C_ASM;
    if (len > 2 && strcmp(name + len - 2, ".c") == 0) return C_ASM;
    
    return C_FILE;
}

void sort_entries(Entry* list, int count) {
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            int swap = 0;
            if (list[j+1].is_dir && !list[j].is_dir) swap = 1;
            else if (list[j].is_dir == list[j+1].is_dir) {
                if (strcmp(list[j].name, list[j+1].name) > 0) swap = 1;
            }
            
            if (swap) {
                Entry temp = list[j];
                list[j] = list[j+1];
                list[j+1] = temp;
            }
        }
    }
}

void print_tree(const char* path, const char* prefix) {
    int fd = open(path, 0);
    if (fd < 0) return;

    int capacity = 16;
    int count = 0;
    Entry* list = malloc(capacity * sizeof(Entry));
    if (!list) { close(fd); return; }

    int used_getdents = 0;

    yfs_dirent_info_t dents[32];
    while (1) {
        int n = getdents(fd, dents, sizeof(dents));
        if (n < 0) break;
        used_getdents = 1;
        if (n == 0) break;

        int dent_count = n / (int)sizeof(yfs_dirent_info_t);
        for (int di = 0; di < dent_count; di++) {
            if (dents[di].inode == 0) continue;
            if (strcmp(dents[di].name, ".") == 0 || strcmp(dents[di].name, "..") == 0) continue;

            if (count >= capacity) {
                capacity *= 2;
                list = realloc(list, capacity * sizeof(Entry));
                if (!list) { close(fd); return; }
            }

            strcpy(list[count].name, dents[di].name);
            list[count].is_dir = (dents[di].type == 2);
            list[count].size = (int)dents[di].size;
            count++;
        }
    }

    if (!used_getdents) {
        yfs_dirent_t dent;
        while (read(fd, &dent, sizeof(yfs_dirent_t)) > 0) {
            if (dent.inode == 0) continue;
            if (strcmp(dent.name, ".") == 0 || strcmp(dent.name, "..") == 0) continue;

            if (count >= capacity) {
                capacity *= 2;
                list = realloc(list, capacity * sizeof(Entry));
                if (!list) { close(fd); return; }
            }

            strcpy(list[count].name, dent.name);
            
            stat_t st;
            if (fstatat(fd, dent.name, &st) == 0) {
                list[count].is_dir = (st.type == 2);
                list[count].size = st.size;
            } else {
                char full_path[MAX_PATH];
                strcpy(full_path, path);
                if (full_path[strlen(full_path)-1] != '/') strcat(full_path, "/");
                strcat(full_path, dent.name);

                if (stat(full_path, &st) == 0) {
                    list[count].is_dir = (st.type == 2);
                    list[count].size = st.size;
                } else {
                    list[count].is_dir = 0;
                }
            }

            count++;
        }
    }
    
    close(fd);

    sort_entries(list, count);

    for (int i = 0; i < count; i++) {
        int is_last = (i == count - 1);

        char line_prefix[MAX_PATH + 8];
        line_prefix[0] = 0;
        strcat(line_prefix, prefix);
        strcat(line_prefix, is_last ? "`-- " : "|-- ");

        set_console_color(C_TREE, C_BG);
        print(line_prefix);

        uint32_t col = get_color(list[i].name, list[i].is_dir);
        set_console_color(col, C_BG);

        char name_line[MAX_PATH + 2];
        name_line[0] = 0;
        strcat(name_line, list[i].name);
        strcat(name_line, "\n");
        print(name_line);

        if (list[i].is_dir) {
            total_dirs++;
            
            char new_prefix[MAX_PATH];
            strcpy(new_prefix, prefix);
            strcat(new_prefix, is_last ? "    " : "|   ");
            
            char new_path[MAX_PATH];
            strcpy(new_path, path);
            if (new_path[strlen(new_path)-1] != '/') strcat(new_path, "/");
            strcat(new_path, list[i].name);

            print_tree(new_path, new_prefix);
        } else {
            total_files++;
        }
    }

    free(list);
}

int main(int argc, char** argv) {
    const char* start_path = ".";
    if (argc > 1) start_path = argv[1];

    set_console_color(C_DIR, C_BG);
    print(start_path);
    print("\n");

    print_tree(start_path, "");

    set_console_color(C_TREE, C_BG);
    print("\n");
    print_dec(total_dirs); print(" directories, ");
    print_dec(total_files); print(" files\n");
    
    set_console_color(C_FILE, C_BG);
    return 0;
}