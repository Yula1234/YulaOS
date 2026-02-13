#include <yula.h>

#define SPIN_DEFAULT_REPO_BASE "https://raw.githubusercontent.com/Yula1234/YulaOSRepo/master"

#define SPIN_DIR_ETC "/etc/spin"
#define SPIN_FILE_REPO_CONF "/etc/spin/repo.conf"

#define SPIN_DIR_CACHE "/var/cache/spin"
#define SPIN_FILE_INDEX_CACHE "/var/cache/spin/index.txt"

#define SPIN_DIR_STATE "/var/lib/spin"
#define SPIN_FILE_INSTALLED "/var/lib/spin/installed.txt"

static void spin_copy_str(char* dst, uint32_t cap, const char* src) {
    if (!dst || cap == 0) {
        return;
    }

    if (!src) {
        dst[0] = '\0';
        return;
    }

    uint32_t i = 0;
    for (; i + 1u < cap && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static int spin_parse_u32_str(const char* s, uint32_t* out) {
    if (!s || !*s || !out) {
        return 0;
    }

    uint32_t v = 0;
    for (const char* p = s; *p; p++) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
        uint32_t d = (uint32_t)(*p - '0');
        if (v > (0xFFFFFFFFu - d) / 10u) {
            return 0;
        }
        v = v * 10u + d;
    }

    *out = v;
    return 1;
}

static int spin_file_size(const char* path, uint32_t* out_size) {
    if (out_size) *out_size = 0;
    if (!path || !out_size) {
        return -1;
    }

    stat_t st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    if (st.type != 1u) {
        return -1;
    }

    *out_size = st.size;
    return 0;
}

static int spin_is_space(char c);
static void spin_trim(char** io_s);
static int spin_read_all_text_file(const char* path, char** out_data, uint32_t* out_size);

static int spin_installed_db_contains(const char* name) {
    if (!name || !*name) {
        return 0;
    }

    char* data = 0;
    uint32_t size = 0;
    if (spin_read_all_text_file(SPIN_FILE_INSTALLED, &data, &size) != 0) {
        return 0;
    }

    int found = 0;
    const char* p = data;
    while (*p) {
        const char* line = p;
        while (*p && *p != '\n') {
            p++;
        }

        uint32_t len = (uint32_t)(p - line);
        if (*p == '\n') {
            p++;
        }

        if (len == 0) {
            continue;
        }

        char buf[512];
        if (len >= sizeof(buf)) {
            continue;
        }

        memcpy(buf, line, len);
        buf[len] = '\0';

        char* s = buf;
        spin_trim(&s);

        if (strncmp(s, "pkg", 3) == 0 && spin_is_space(s[3])) {
            char* tok = s;
            char* toks[2] = {0, 0};
            int t = 0;

            while (*tok && t < 2) {
                while (*tok && spin_is_space(*tok)) {
                    *tok++ = '\0';
                }
                if (!*tok) {
                    break;
                }
                toks[t++] = tok;
                while (*tok && !spin_is_space(*tok)) {
                    tok++;
                }
                if (*tok) {
                    *tok++ = '\0';
                }
            }

            if (t >= 2 && toks[1] && strcmp(toks[1], name) == 0) {
                found = 1;
                break;
            }
        }
    }

    free(data);
    return found;
}

static int spin_skip_fd(int fd, uint32_t bytes) {
    uint8_t buf[128];
    uint32_t left = bytes;

    while (left != 0) {
        uint32_t n = left;
        if (n > (uint32_t)sizeof(buf)) {
            n = (uint32_t)sizeof(buf);
        }

        int rn = read(fd, buf, n);
        if (rn <= 0) {
            return -1;
        }
        left -= (uint32_t)rn;
    }

    return 0;
}

static int spin_is_elf_file(const char* path) {
    if (!path || !*path) {
        return 0;
    }

    uint32_t fsize = 0;
    if (spin_file_size(path, &fsize) != 0 || fsize < 64u) {
        return 0;
    }

    int fd = open(path, 0);
    if (fd < 0) {
        return 0;
    }

    typedef struct {
        uint8_t e_ident[16];
        uint16_t e_type;
        uint16_t e_machine;
        uint32_t e_version;
        uint32_t e_entry;
        uint32_t e_phoff;
        uint32_t e_shoff;
        uint32_t e_flags;
        uint16_t e_ehsize;
        uint16_t e_phentsize;
        uint16_t e_phnum;
        uint16_t e_shentsize;
        uint16_t e_shnum;
        uint16_t e_shstrndx;
    } spin_elf32_ehdr_t;

    typedef struct {
        uint32_t p_type;
        uint32_t p_offset;
        uint32_t p_vaddr;
        uint32_t p_paddr;
        uint32_t p_filesz;
        uint32_t p_memsz;
        uint32_t p_flags;
        uint32_t p_align;
    } spin_elf32_phdr_t;

    spin_elf32_ehdr_t eh;
    int rn = read(fd, &eh, (uint32_t)sizeof(eh));
    if (rn != (int)sizeof(eh)) {
        close(fd);
        return 0;
    }

    if (eh.e_ident[0] != 0x7Fu || eh.e_ident[1] != 'E' || eh.e_ident[2] != 'L' || eh.e_ident[3] != 'F') {
        close(fd);
        return 0;
    }
    if (eh.e_ident[4] != 1u) {
        close(fd);
        return 0;
    }
    if (eh.e_ident[5] != 1u) {
        close(fd);
        return 0;
    }
    if (eh.e_ident[6] != 1u) {
        close(fd);
        return 0;
    }
    if (eh.e_type != 2u) {
        close(fd);
        return 0;
    }
    if (eh.e_machine != 3u) {
        close(fd);
        return 0;
    }
    if (eh.e_phentsize != (uint16_t)sizeof(spin_elf32_phdr_t)) {
        close(fd);
        return 0;
    }
    if (eh.e_phnum == 0u) {
        close(fd);
        return 0;
    }

    uint32_t ph_end = eh.e_phoff + (uint32_t)eh.e_phnum * (uint32_t)eh.e_phentsize;
    if (eh.e_phoff < (uint32_t)sizeof(eh) || ph_end < eh.e_phoff || ph_end > fsize) {
        close(fd);
        return 0;
    }

    if (eh.e_phoff > (uint32_t)sizeof(eh)) {
        if (spin_skip_fd(fd, eh.e_phoff - (uint32_t)sizeof(eh)) != 0) {
            close(fd);
            return 0;
        }
    }

    int has_load = 0;
    for (uint16_t i = 0; i < eh.e_phnum; i++) {
        spin_elf32_phdr_t ph;
        rn = read(fd, &ph, (uint32_t)sizeof(ph));
        if (rn != (int)sizeof(ph)) {
            close(fd);
            return 0;
        }

        if (ph.p_type == 1u) {
            has_load = 1;
        }

        uint32_t seg_end = ph.p_offset + ph.p_filesz;
        if (seg_end < ph.p_offset || seg_end > fsize) {
            close(fd);
            return 0;
        }
        if (ph.p_memsz < ph.p_filesz) {
            close(fd);
            return 0;
        }
    }

    close(fd);
    return has_load;
}

static int spin_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void spin_trim(char** io_s) {
    if (!io_s || !*io_s) {
        return;
    }

    char* s = *io_s;
    while (*s && spin_is_space(*s)) {
        s++;
    }

    char* end = s + strlen(s);
    while (end > s && spin_is_space(end[-1])) {
        end--;
    }
    *end = '\0';

    *io_s = s;
}

static int spin_mkdir_if_missing(const char* path) {
    if (!path || !*path) {
        return -1;
    }

    stat_t st;
    if (stat(path, &st) == 0) {
        if (st.type == 2u) {
            return 0;
        }
        return -1;
    }

    if (mkdir(path) == 0) {
        return 0;
    }

    if (stat(path, &st) == 0 && st.type == 2u) {
        return 0;
    }

    return -1;
}

static int spin_mkdir_p(const char* path) {
    if (!path || path[0] != '/') {
        return -1;
    }

    char buf[256];
    if (strlen(path) >= sizeof(buf)) {
        return -1;
    }

    strcpy(buf, path);

    for (uint32_t i = 1; buf[i] != '\0'; i++) {
        if (buf[i] != '/') {
            continue;
        }

        buf[i] = '\0';
        if (spin_mkdir_if_missing(buf) != 0) {
            return -1;
        }
        buf[i] = '/';
    }

    return spin_mkdir_if_missing(buf);
}

static int spin_read_all_text_file(const char* path, char** out_data, uint32_t* out_size) {
    if (out_data) *out_data = 0;
    if (out_size) *out_size = 0;

    if (!path || !out_data || !out_size) {
        return -1;
    }

    int fd = open(path, 0);
    if (fd < 0) {
        return -1;
    }

    uint32_t cap = 1024u;
    char* data = (char*)malloc(cap);
    if (!data) {
        close(fd);
        return -1;
    }

    uint32_t size = 0;
    for (;;) {
        if (size + 512u + 1u > cap) {
            uint32_t ncap = cap * 2u;
            if (ncap < cap) {
                free(data);
                close(fd);
                return -1;
            }
            char* nd = (char*)realloc(data, ncap);
            if (!nd) {
                free(data);
                close(fd);
                return -1;
            }
            data = nd;
            cap = ncap;
        }

        int rn = read(fd, data + size, 512u);
        if (rn < 0) {
            free(data);
            close(fd);
            return -1;
        }
        if (rn == 0) {
            break;
        }

        size += (uint32_t)rn;
    }

    close(fd);

    data[size] = '\0';

    *out_data = data;
    *out_size = size;
    return 0;
}

static int spin_write_all_file(const char* path, const void* data, uint32_t size) {
    if (!path || (!data && size != 0)) {
        return -1;
    }

    int fd = open(path, 1);
    if (fd < 0) {
        return -1;
    }

    const uint8_t* p = (const uint8_t*)data;
    uint32_t off = 0;
    while (off < size) {
        int wn = write(fd, p + off, size - off);
        if (wn <= 0) {
            close(fd);
            return -1;
        }
        off += (uint32_t)wn;
    }

    close(fd);
    return 0;
}

static int spin_replace_file_from_tmp(const char* tmp_path, const char* dst_path) {
    if (!tmp_path || !*tmp_path || !dst_path || !*dst_path) {
        return -1;
    }

    (void)unlink(dst_path);
    if (rename(tmp_path, dst_path) == 0) {
        return 0;
    }

    stat_t st;
    if (stat(tmp_path, &st) == 0) {
        (void)unlink(tmp_path);
    }
    return -1;
}

static int spin_read_repo_base(char out[384]) {
    if (!out) {
        return -1;
    }

    out[0] = '\0';

    char* conf = 0;
    uint32_t conf_size = 0;
    if (spin_read_all_text_file(SPIN_FILE_REPO_CONF, &conf, &conf_size) == 0) {
        char* s = conf;
        spin_trim(&s);
        if (*s) {
            spin_copy_str(out, 384u, s);
            free(conf);
            return 0;
        }
        free(conf);
    }

    spin_copy_str(out, 384u, SPIN_DEFAULT_REPO_BASE);
    return 0;
}

static int spin_set_repo_base(const char* base) {
    if (!base || !*base) {
        return -1;
    }

    if (spin_mkdir_p(SPIN_DIR_ETC) != 0) {
        return -2;
    }

    char tmp[256];
    (void)snprintf(tmp, sizeof(tmp), "%s.tmp", SPIN_FILE_REPO_CONF);

    if (spin_write_all_file(tmp, base, (uint32_t)strlen(base)) != 0) {
        unlink(tmp);
        return -3;
    }

    if (spin_replace_file_from_tmp(tmp, SPIN_FILE_REPO_CONF) != 0) {
        return -4;
    }

    return 0;
}

static int spin_run_wget(const char* url, const char* out_path, int quiet) {
    if (!url || !*url || !out_path || !*out_path) {
        return -1;
    }

    char* argv[8];
    int argc = 0;

    argv[argc++] = "wget";
    argv[argc++] = (char*)url;
    argv[argc++] = "-O";
    argv[argc++] = (char*)out_path;
    if (quiet) {
        argv[argc++] = "-q";
    }
    argv[argc] = 0;

    int pid = spawn_process_resolved("wget", argc, argv);
    if (pid < 0) {
        return -1;
    }

    int status = 0;
    if (waitpid(pid, &status) < 0) {
        return -1;
    }

    uint32_t size = 0;
    if (spin_file_size(out_path, &size) != 0) {
        return -1;
    }

    if (size == 0) {
        return -1;
    }

    (void)status;
    return 0;
}

static int spin_download_index(void) {
    if (spin_mkdir_p("/tmp") != 0) {
        return -2;
    }

    if (spin_mkdir_p(SPIN_DIR_CACHE) != 0) {
        return -3;
    }

    char base[384];
    if (spin_read_repo_base(base) != 0) {
        return -1;
    }

    char url[512];
    (void)snprintf(url, sizeof(url), "%s/index.txt", base);

    char tmp[256];
    (void)snprintf(tmp, sizeof(tmp), "%s.tmp", SPIN_FILE_INDEX_CACHE);

    if (spin_run_wget(url, tmp, 0) != 0) {
        unlink(tmp);
        return -4;
    }

    if (spin_replace_file_from_tmp(tmp, SPIN_FILE_INDEX_CACHE) != 0) {
        return -5;
    }

    return 0;
}

typedef struct {
    char name[64];
    char ver[32];
    char path[256];
    uint32_t expected_size;
} spin_pkg_entry_t;

static int spin_parse_index_find(const char* index_text, const char* name, spin_pkg_entry_t* out) {
    if (!index_text || !name || !*name || !out) {
        return -1;
    }

    const char* p = index_text;

    while (*p) {
        const char* line = p;
        while (*p && *p != '\n') {
            p++;
        }
        uint32_t len = (uint32_t)(p - line);
        if (*p == '\n') {
            p++;
        }

        if (len == 0) {
            continue;
        }

        char buf[384];
        if (len >= sizeof(buf)) {
            continue;
        }

        memcpy(buf, line, len);
        buf[len] = '\0';

        char* s = buf;
        spin_trim(&s);
        if (!*s) {
            continue;
        }

        if (strcmp(s, "SPIN_REPO1") == 0) {
            continue;
        }

        if (strncmp(s, "pkg", 3) != 0 || !spin_is_space(s[3])) {
            continue;
        }

        char* tok = s;
        int t = 0;
        char* toks[5] = {0, 0, 0, 0, 0};

        while (*tok && t < 5) {
            while (*tok && spin_is_space(*tok)) {
                *tok++ = '\0';
            }
            if (!*tok) {
                break;
            }
            toks[t++] = tok;
            while (*tok && !spin_is_space(*tok)) {
                tok++;
            }
        }

        if (t < 4) {
            continue;
        }

        const char* pkg_name = toks[1];
        const char* pkg_ver = toks[2];
        const char* pkg_path = toks[3];

        uint32_t expected_size = 0;
        if (t >= 5 && toks[4] && *toks[4]) {
            if (!spin_parse_u32_str(toks[4], &expected_size)) {
                continue;
            }
        }

        if (strcmp(pkg_name, name) != 0) {
            continue;
        }

        spin_copy_str(out->name, (uint32_t)sizeof(out->name), pkg_name);
        spin_copy_str(out->ver, (uint32_t)sizeof(out->ver), pkg_ver);
        spin_copy_str(out->path, (uint32_t)sizeof(out->path), pkg_path);
        out->expected_size = expected_size;
        return 0;
    }

    return -1;
}

static int spin_ensure_state_dirs(void) {
    if (spin_mkdir_if_missing("/var") != 0) {
        return -2;
    }
    if (spin_mkdir_if_missing("/var/lib") != 0) {
        return -3;
    }
    if (spin_mkdir_if_missing(SPIN_DIR_STATE) != 0) {
        return -4;
    }
    return 0;
}

static int spin_write_installed_db_replace(const char* name, const char* ver, const char* bin_path) {
    if (!name || !*name || !ver || !*ver || !bin_path || !*bin_path) {
        return -1;
    }

    {
        int rc = spin_ensure_state_dirs();
        if (rc != 0) {
            return rc;
        }
    }

    char* data = 0;
    uint32_t size = 0;
    (void)spin_read_all_text_file(SPIN_FILE_INSTALLED, &data, &size);

    uint32_t out_cap = size + 512u;
    char* out = (char*)malloc(out_cap);
    if (!out) {
        free(data);
        return -20;
    }

    uint32_t out_len = 0;
    if (data) {
        const char* p = data;
        while (*p) {
            const char* line = p;
            while (*p && *p != '\n') {
                p++;
            }
            uint32_t len = (uint32_t)(p - line);
            if (*p == '\n') {
                p++;
            }

            if (len == 0) {
                continue;
            }

            char buf[512];
            if (len >= sizeof(buf)) {
                continue;
            }

            memcpy(buf, line, len);
            buf[len] = '\0';

            char* s = buf;
            spin_trim(&s);

            int keep = 1;
            if (strncmp(s, "pkg", 3) == 0 && spin_is_space(s[3])) {
                char* tok = s;
                char* toks[2] = {0, 0};
                int t = 0;

                while (*tok && t < 2) {
                    while (*tok && spin_is_space(*tok)) {
                        *tok++ = '\0';
                    }
                    if (!*tok) {
                        break;
                    }
                    toks[t++] = tok;
                    while (*tok && !spin_is_space(*tok)) {
                        tok++;
                    }
                    if (*tok) {
                        *tok++ = '\0';
                    }
                }

                if (t >= 2 && toks[1] && strcmp(toks[1], name) == 0) {
                    keep = 0;
                }
            }

            if (!keep) {
                continue;
            }

            if (out_len + len + 1u >= out_cap) {
                uint32_t nc = out_cap * 2u;
                if (nc < out_cap) {
                    free(out);
                    free(data);
                    return -21;
                }
                char* no = (char*)realloc(out, nc);
                if (!no) {
                    free(out);
                    free(data);
                    return -21;
                }
                out = no;
                out_cap = nc;
            }

            memcpy(out + out_len, line, len);
            out_len += len;
            out[out_len++] = '\n';
        }
    }

    free(data);

    char line[512];
    int n = snprintf(line, sizeof(line), "pkg %s %s %s\n", name, ver, bin_path);
    if (n < 0 || (uint32_t)n >= sizeof(line)) {
        free(out);
        return -11;
    }

    if (out_len + (uint32_t)n >= out_cap) {
        uint32_t nc = out_len + (uint32_t)n + 1u;
        char* no = (char*)realloc(out, nc);
        if (!no) {
            free(out);
            return -21;
        }
        out = no;
        out_cap = nc;
    }

    memcpy(out + out_len, line, (uint32_t)n);
    out_len += (uint32_t)n;

    int wrc = spin_write_all_file(SPIN_FILE_INSTALLED, out, out_len);
    free(out);

    if (wrc != 0) {
        return -12;
    }

    return 0;
}

static int spin_remove_installed_entry(const char* name) {
    if (!name || !*name) {
        return -1;
    }

    {
        int rc = spin_ensure_state_dirs();
        if (rc != 0) {
            return rc;
        }
    }

    char* data = 0;
    uint32_t size = 0;

    if (spin_read_all_text_file(SPIN_FILE_INSTALLED, &data, &size) != 0) {
        return 0;
    }

    char* out = (char*)malloc(size + 1u);
    if (!out) {
        free(data);
        return -1;
    }

    uint32_t out_len = 0;
    const char* p = data;

    while (*p) {
        const char* line = p;
        while (*p && *p != '\n') {
            p++;
        }
        uint32_t len = (uint32_t)(p - line);
        if (*p == '\n') {
            p++;
        }

        if (len == 0) {
            continue;
        }

        char buf[512];
        if (len >= sizeof(buf)) {
            continue;
        }

        memcpy(buf, line, len);
        buf[len] = '\0';

        char* s = buf;
        spin_trim(&s);

        int keep = 1;
        if (strncmp(s, "pkg", 3) == 0 && spin_is_space(s[3])) {
            char* tok = s;
            char* toks[2] = {0, 0};
            int t = 0;

            while (*tok && t < 2) {
                while (*tok && spin_is_space(*tok)) {
                    *tok++ = '\0';
                }
                if (!*tok) {
                    break;
                }
                toks[t++] = tok;
                while (*tok && !spin_is_space(*tok)) {
                    tok++;
                }
                if (*tok) {
                    *tok++ = '\0';
                }
            }

            if (t >= 2 && toks[1] && strcmp(toks[1], name) == 0) {
                keep = 0;
            }
        }

        if (!keep) {
            continue;
        }

        if (out_len + len + 1u >= size + 1u) {
            free(out);
            free(data);
            return -1;
        }

        memcpy(out + out_len, line, len);
        out_len += len;
        out[out_len++] = '\n';
    }

    free(data);

    if (spin_write_all_file(SPIN_FILE_INSTALLED, out, out_len) != 0) {
        free(out);
        return -10;
    }

    free(out);

    return 0;
}

static void spin_print_usage(void) {
    printf("spin: package manager\n");
    printf("Usage:\n");
    printf("  spin version\n");
    printf("  spin repo\n");
    printf("  spin repo <base_url>\n");
    printf("  spin update\n");
    printf("  spin search <substr>\n");
    printf("  spin install <name>\n");
    printf("  spin remove <name>\n");
    printf("  spin list\n");
}

static int spin_cmd_version(void) {
    printf("spin build %s %s\n", __DATE__, __TIME__);
    return 0;
}

static int spin_cmd_repo(int argc, char** argv) {
    if (argc == 2) {
        char base[384];
        if (spin_read_repo_base(base) != 0) {
            printf("spin: failed to read repo\n");
            return 1;
        }
        printf("%s\n", base);
        return 0;
    }

    if (argc == 3) {
        int rc = spin_set_repo_base(argv[2]);
        if (rc != 0) {
            if (rc == -2) {
                printf("spin: failed to create %s\n", SPIN_DIR_ETC);
                return 1;
            }
            if (rc == -3) {
                printf("spin: failed to write %s\n", SPIN_FILE_REPO_CONF);
                return 1;
            }
            if (rc == -4) {
                printf("spin: failed to rename repo.conf\n");
                return 1;
            }

            printf("spin: failed to set repo\n");
            return 1;
        }
        return 0;
    }

    spin_print_usage();
    return 1;
}

static int spin_cmd_update(void) {
    int rc = spin_download_index();
    if (rc != 0) {
        char base[384];
        (void)spin_read_repo_base(base);

        char url[512];
        (void)snprintf(url, sizeof(url), "%s/index.txt", base);

        if (rc == -2) {
            printf("spin: update failed: cannot create /tmp\n");
            return 1;
        }
        if (rc == -3) {
            printf("spin: update failed: cannot create %s\n", SPIN_DIR_CACHE);
            return 1;
        }
        if (rc == -4) {
            printf("spin: update failed: wget failed for %s\n", url);
            return 1;
        }
        if (rc == -5) {
            printf("spin: update failed: cannot write %s\n", SPIN_FILE_INDEX_CACHE);
            return 1;
        }

        printf("spin: update failed\n");
        return 1;
    }
    return 0;
}

static int spin_load_index(char** out_text) {
    if (out_text) *out_text = 0;

    if (spin_download_index() != 0) {
        return -1;
    }

    char* data = 0;
    uint32_t size = 0;
    if (spin_read_all_text_file(SPIN_FILE_INDEX_CACHE, &data, &size) != 0) {
        return -1;
    }

    if (size < 8u) {
        free(data);
        return -1;
    }

    *out_text = data;
    return 0;
}

static int spin_cmd_search(int argc, char** argv) {
    if (argc != 3) {
        spin_print_usage();
        return 1;
    }

    const char* needle = argv[2];
    if (!needle || !*needle) {
        return 1;
    }

    char* index_text = 0;
    if (spin_load_index(&index_text) != 0) {
        printf("spin: failed to load index\n");
        return 1;
    }

    const char* p = index_text;
    while (*p) {
        const char* line = p;
        while (*p && *p != '\n') {
            p++;
        }
        uint32_t len = (uint32_t)(p - line);
        if (*p == '\n') {
            p++;
        }

        if (len == 0) {
            continue;
        }

        char buf[384];
        if (len >= sizeof(buf)) {
            continue;
        }

        memcpy(buf, line, len);
        buf[len] = '\0';

        if (strstr(buf, needle)) {
            printf("%s\n", buf);
        }
    }

    free(index_text);
    return 0;
}

static int spin_cmd_install(int argc, char** argv) {
    if (argc != 3) {
        spin_print_usage();
        return 1;
    }

    const char* name = argv[2];

    char* index_text = 0;
    if (spin_load_index(&index_text) != 0) {
        printf("spin: failed to load index\n");
        return 1;
    }

    spin_pkg_entry_t e;
    memset(&e, 0, sizeof(e));
    if (spin_parse_index_find(index_text, name, &e) != 0) {
        free(index_text);
        printf("spin: package not found\n");
        return 1;
    }

    free(index_text);

    char base[384];
    if (spin_read_repo_base(base) != 0) {
        printf("spin: failed to read repo\n");
        return 1;
    }

    char url[640];
    (void)snprintf(url, sizeof(url), "%s/%s", base, e.path);

    char tmp[256];
    (void)snprintf(tmp, sizeof(tmp), "/tmp/spin.%s.new", e.name);

    if (spin_mkdir_p("/tmp") != 0) {
        printf("spin: install failed\n");
        return 1;
    }

    int ok = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        (void)unlink(tmp);

        if (spin_run_wget(url, tmp, 0) != 0) {
            continue;
        }

        if (e.expected_size != 0) {
            uint32_t got = 0;
            if (spin_file_size(tmp, &got) != 0 || got != e.expected_size) {
                continue;
            }
        }

        if (!spin_is_elf_file(tmp)) {
            continue;
        }

        ok = 1;
        break;
    }

    if (!ok) {
        unlink(tmp);
        printf("spin: download failed\n");
        return 1;
    }

    char dst[128];
    (void)snprintf(dst, sizeof(dst), "/bin/%s.exe", e.name);

    if (rename(tmp, dst) != 0) {
        unlink(tmp);
        printf("spin: install failed\n");
        return 1;
    }

    {
        int rc = spin_write_installed_db_replace(e.name, e.ver, dst);
        if (rc != 0) {
            if (rc == -2) {
                printf("spin: warning: cannot create /var\n");
            } else if (rc == -3) {
                printf("spin: warning: cannot create /var/lib\n");
            } else if (rc == -4) {
                printf("spin: warning: cannot create %s\n", SPIN_DIR_STATE);
            } else if (rc == -10) {
                printf("spin: warning: cannot open %s\n", SPIN_FILE_INSTALLED);
            } else if (rc == -12) {
                printf("spin: warning: write failed for %s\n", SPIN_FILE_INSTALLED);
            } else {
                printf("spin: warning: installed DB update failed\n");
            }
        }
    }

    return 0;
}

static int spin_cmd_remove(int argc, char** argv) {
    if (argc != 3) {
        spin_print_usage();
        return 1;
    }

    const char* name = argv[2];

    char dst[128];
    (void)snprintf(dst, sizeof(dst), "/bin/%s.exe", name);

    if (unlink(dst) != 0) {
        stat_t st;
        if (stat(dst, &st) == 0) {
            printf("spin: remove failed\n");
            return 1;
        }
    }

    {
        int rc = spin_remove_installed_entry(name);
        if (rc != 0) {
            if (rc == -2) {
                printf("spin: warning: cannot create /var\n");
            } else if (rc == -3) {
                printf("spin: warning: cannot create /var/lib\n");
            } else if (rc == -4) {
                printf("spin: warning: cannot create %s\n", SPIN_DIR_STATE);
            } else if (rc == -10) {
                printf("spin: warning: write failed for %s\n", SPIN_FILE_INSTALLED);
            } else {
                printf("spin: warning: installed DB update failed\n");
            }
        } else {
            if (spin_installed_db_contains(name)) {
                printf("spin: warning: installed DB update failed\n");
            }
        }
    }
    return 0;
}

static int spin_cmd_list(void) {
    char* data = 0;
    uint32_t size = 0;
    if (spin_read_all_text_file(SPIN_FILE_INSTALLED, &data, &size) != 0) {
        return 0;
    }

    print(data);
    free(data);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        spin_print_usage();
        return 1;
    }

    if (strcmp(argv[1], "version") == 0) {
        return spin_cmd_version();
    }

    if (strcmp(argv[1], "repo") == 0) {
        return spin_cmd_repo(argc, argv);
    }

    if (strcmp(argv[1], "update") == 0) {
        return spin_cmd_update();
    }

    if (strcmp(argv[1], "search") == 0) {
        return spin_cmd_search(argc, argv);
    }

    if (strcmp(argv[1], "install") == 0) {
        return spin_cmd_install(argc, argv);
    }

    if (strcmp(argv[1], "remove") == 0) {
        return spin_cmd_remove(argc, argv);
    }

    if (strcmp(argv[1], "list") == 0) {
        return spin_cmd_list();
    }

    spin_print_usage();
    return 1;
}
