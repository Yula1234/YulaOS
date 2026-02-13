// SPDX-License-Identifier: GPL-2.0

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

#define SPK_MAGIC "SPIN"
#define SPK_VERSION 1

#define MAX_NAME 64
#define MAX_VER 16
#define MAX_DESC 128
#define MAX_PATH 200
#define MAX_FILES 256
#define MAX_DEPS 32
#define MAX_LINE 1024

typedef struct {
    char magic[4];
    uint32_t version;
    char name[MAX_NAME];
    char ver[MAX_VER];
    char desc[MAX_DESC];
    uint32_t file_count;
    uint32_t deps_count;
    char reserved[288];
} __attribute__((packed)) spk_header_t;

typedef struct {
    char name[64];
    char minver[16];
    char reserved[16];
} __attribute__((packed)) spk_dep_t;

typedef struct {
    char path[MAX_PATH];
    uint32_t size;
    uint32_t mode;
    uint32_t offset;
    char reserved[44];
} __attribute__((packed)) spk_file_t;

typedef struct {
    char src[MAX_PATH];
    char dest[MAX_PATH];
    uint32_t mode;
} file_entry_t;

typedef struct {
    char name[64];
    char minver[16];
} dep_entry_t;

typedef struct {
    char name[MAX_NAME];
    char version[MAX_VER];
    char description[MAX_DESC];
    file_entry_t files[MAX_FILES];
    uint32_t file_count;
    dep_entry_t deps[MAX_DEPS];
    uint32_t dep_count;
} manifest_t;

static void trim(char* s) {
    char* p = s;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
        p++;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }

    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' || s[len-1] == '\r' || s[len-1] == '\n')) {
        s[--len] = '\0';
    }
}

static int parse_manifest(const char* path, manifest_t* m) {
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "error: cannot open %s\n", path);
        return -1;
    }

    memset(m, 0, sizeof(*m));

    char line[MAX_LINE];
    int line_num = 0;

    while (fgets(line, sizeof(line), f)) {
        line_num++;
        trim(line);

        if (line[0] == '#' || line[0] == '\0') {
            continue;
        }

        char* space = strchr(line, ' ');
        if (!space) {
            fprintf(stderr, "warning: line %d: invalid format\n", line_num);
            continue;
        }

        *space = '\0';
        char* key = line;
        char* value = space + 1;

        while (*value == ' ' || *value == '\t') {
            value++;
        }

        if (strcmp(key, "name") == 0) {
            strncpy(m->name, value, MAX_NAME - 1);
        } else if (strcmp(key, "version") == 0) {
            strncpy(m->version, value, MAX_VER - 1);
        } else if (strcmp(key, "description") == 0) {
            strncpy(m->description, value, MAX_DESC - 1);
        } else if (strcmp(key, "depends") == 0) {
            if (m->dep_count >= MAX_DEPS) {
                fprintf(stderr, "error: too many dependencies\n");
                fclose(f);
                return -1;
            }

            char* ver_sep = strchr(value, ' ');
            if (ver_sep) {
                *ver_sep = '\0';
                strncpy(m->deps[m->dep_count].name, value, 63);
                strncpy(m->deps[m->dep_count].minver, ver_sep + 1, 15);
            } else {
                strncpy(m->deps[m->dep_count].name, value, 63);
                m->deps[m->dep_count].minver[0] = '\0';
            }
            m->dep_count++;
        } else if (strcmp(key, "file") == 0) {
            if (m->file_count >= MAX_FILES) {
                fprintf(stderr, "error: too many files\n");
                fclose(f);
                return -1;
            }

            char* src = value;
            char* dest = strchr(src, ' ');
            if (!dest) {
                fprintf(stderr, "error: line %d: file needs destination\n", line_num);
                fclose(f);
                return -1;
            }
            *dest++ = '\0';
            while (*dest == ' ') {
                dest++;
            }

            char* mode_str = strchr(dest, ' ');
            uint32_t mode = 0755;
            if (mode_str) {
                *mode_str++ = '\0';
                while (*mode_str == ' ') {
                    mode_str++;
                }
                mode = strtoul(mode_str, NULL, 8);
            }

            strncpy(m->files[m->file_count].src, src, MAX_PATH - 1);
            strncpy(m->files[m->file_count].dest, dest, MAX_PATH - 1);
            m->files[m->file_count].mode = mode;
            m->file_count++;
        }
    }

    fclose(f);

    if (m->name[0] == '\0') {
        fprintf(stderr, "error: missing 'name' field\n");
        return -1;
    }
    if (m->version[0] == '\0') {
        fprintf(stderr, "error: missing 'version' field\n");
        return -1;
    }

    return 0;
}

static long get_file_size(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    return st.st_size;
}

static int build_package(const manifest_t* m, const char* out_path) {
    FILE* out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "error: cannot create %s\n", out_path);
        return -1;
    }

    spk_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, SPK_MAGIC, 4);
    hdr.version = SPK_VERSION;
    strncpy(hdr.name, m->name, MAX_NAME - 1);
    strncpy(hdr.ver, m->version, MAX_VER - 1);
    strncpy(hdr.desc, m->description, MAX_DESC - 1);
    hdr.file_count = m->file_count;
    hdr.deps_count = m->dep_count;

    if (fwrite(&hdr, sizeof(hdr), 1, out) != 1) {
        fprintf(stderr, "error: write header failed\n");
        fclose(out);
        return -1;
    }

    for (uint32_t i = 0; i < m->dep_count; i++) {
        spk_dep_t dep;
        memset(&dep, 0, sizeof(dep));
        strncpy(dep.name, m->deps[i].name, 63);
        strncpy(dep.minver, m->deps[i].minver, 15);

        if (fwrite(&dep, sizeof(dep), 1, out) != 1) {
            fprintf(stderr, "error: write dep failed\n");
            fclose(out);
            return -1;
        }
    }

    uint32_t data_offset = 0;
    spk_file_t file_entries[MAX_FILES];

    for (uint32_t i = 0; i < m->file_count; i++) {
        long size = get_file_size(m->files[i].src);
        if (size < 0) {
            fprintf(stderr, "error: cannot stat %s\n", m->files[i].src);
            fclose(out);
            return -1;
        }

        memset(&file_entries[i], 0, sizeof(spk_file_t));
        strncpy(file_entries[i].path, m->files[i].dest, MAX_PATH - 1);
        file_entries[i].size = (uint32_t)size;
        file_entries[i].mode = m->files[i].mode;
        file_entries[i].offset = data_offset;

        data_offset += (uint32_t)size;
    }

    for (uint32_t i = 0; i < m->file_count; i++) {
        if (fwrite(&file_entries[i], sizeof(spk_file_t), 1, out) != 1) {
            fprintf(stderr, "error: write file entry failed\n");
            fclose(out);
            return -1;
        }
    }

    for (uint32_t i = 0; i < m->file_count; i++) {
        FILE* src = fopen(m->files[i].src, "rb");
        if (!src) {
            fprintf(stderr, "error: cannot open %s\n", m->files[i].src);
            fclose(out);
            return -1;
        }

        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
            if (fwrite(buf, 1, n, out) != n) {
                fprintf(stderr, "error: write data failed\n");
                fclose(src);
                fclose(out);
                return -1;
            }
        }

        fclose(src);
    }

    fclose(out);
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: spin-build <manifest.spinpkg> <output.spk>\n");
        return 1;
    }

    manifest_t m;
    if (parse_manifest(argv[1], &m) != 0) {
        return 1;
    }

    printf("building package: %s-%s\n", m.name, m.version);
    printf("description: %s\n", m.description);
    printf("files: %u\n", m.file_count);
    printf("dependencies: %u\n", m.dep_count);

    const char* out_path = argv[2];
    
    if (build_package(&m, out_path) != 0) {
        fprintf(stderr, "failed to build package\n");
        return 1;
    }

    long pkg_size = get_file_size(out_path);
    printf("\npackage created: %s (%ld bytes)\n", out_path, pkg_size);

    return 0;
}
