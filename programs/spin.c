// SPDX-License-Identifier: GPL-2.0

#include <yula.h>
#include <net_ipc.h>

#define SPK_MAGIC "SPIN"
#define SPK_VERSION 1
#define MAX_NAME 64
#define MAX_VER 16
#define MAX_DESC 128
#define MAX_PATH 200

#define DEFAULT_REPO "https://raw.githubusercontent.com/YulaOS/yulaos/main/packages/"
#define CONF_PATH "/etc/spin.conf"
#define DB_PATH "/var/spin/db.txt"
#define CACHE_DIR "/var/spin/cache"
#define INDEX_PATH "/var/spin/repo.idx"

typedef struct {
    char magic[4];
    uint32_t version;
    char name[MAX_NAME];
    char ver[MAX_VER];
    char desc[MAX_DESC];
    uint32_t file_count;
    uint32_t deps_count;
    char reserved[288];
} spk_header_t;

typedef struct {
    char name[64];
    char minver[16];
    char reserved[16];
} spk_dep_t;

typedef struct {
    char path[MAX_PATH];
    uint32_t size;
    uint32_t mode;
    uint32_t offset;
    char reserved[44];
} spk_file_t;

static char repo_url[256] = DEFAULT_REPO;

static void print_usage(void) {
    printf("usage: spin <command> [args]\n\n");
    printf("commands:\n");
    printf("  update              update package index\n");
    printf("  install <package>   install package\n");
    printf("  remove <package>    remove package\n");
    printf("  list                list installed packages\n");
    printf("  search <pattern>    search for packages\n");
    printf("  info <package>      show package info\n");
}

static void ensure_dirs(void) {
    mkdir("/var");
    mkdir("/var/spin");
    mkdir("/var/spin/cache");
    mkdir("/etc");
}

static int load_config(void) {
    int fd = open(CONF_PATH, 0);
    if (fd < 0) {
        return 0;
    }

    char buf[512];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) {
        return 0;
    }

    buf[n] = '\0';
    char* line = buf;
    while (*line) {
        while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n') {
            line++;
        }
        if (*line == '#' || *line == '\0') {
            while (*line && *line != '\n') line++;
            continue;
        }

        char* eq = line;
        while (*eq && *eq != '=' && *eq != '\n') eq++;
        if (*eq != '=') {
            while (*line && *line != '\n') line++;
            continue;
        }

        *eq = '\0';
        char* key = line;
        char* val = eq + 1;
        char* end = val;
        while (*end && *end != '\n') end++;
        if (*end == '\n') *end++ = '\0';

        if (strcmp(key, "repository") == 0) {
            strncpy(repo_url, val, sizeof(repo_url) - 1);
            repo_url[sizeof(repo_url) - 1] = '\0';
        }

        line = end;
    }

    return 0;
}

static int download_file(const char* url, const char* out_path) {
    int fds[2];
    if (ipc_connect("networkd", fds) != 0) {
        printf("error: cannot connect to networkd\n");
        return -1;
    }

    int fd_r = fds[0];
    int fd_w = fds[1];

    net_ipc_rx_t rx;
    net_ipc_rx_reset(&rx);

    net_ipc_hdr_t hdr;
    hdr.type = NET_IPC_MSG_HELLO;
    hdr.seq = 1;
    hdr.len = 0;

    if (write(fd_w, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        close(fd_r);
        close(fd_w);
        return -1;
    }

    sleep(100);

    net_http_get_req_t req;
    memset(&req, 0, sizeof(req));
    req.timeout_ms = 30000;
    req.flags = 0;
    strncpy(req.url, url, sizeof(req.url) - 1);

    hdr.type = NET_IPC_MSG_HTTP_GET_REQ;
    hdr.seq = 2;
    hdr.len = sizeof(req);

    if (write(fd_w, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        close(fd_r);
        close(fd_w);
        return -1;
    }
    if (write(fd_w, &req, sizeof(req)) != sizeof(req)) {
        close(fd_r);
        close(fd_w);
        return -1;
    }

    int out_fd = open(out_path, 1);
    if (out_fd < 0) {
        close(fd_r);
        close(fd_w);
        return -1;
    }

    uint8_t payload[4096];
    int done = 0;
    int success = 0;

    while (!done) {
        int pr = net_ipc_try_recv(&rx, fd_r, &hdr, payload, sizeof(payload));
        if (pr < 0) {
            break;
        }
        if (pr == 0) {
            sleep(50);
            continue;
        }

        if (hdr.type == NET_IPC_MSG_HTTP_GET_DATA) {
            if (hdr.len > 0 && hdr.len <= sizeof(payload)) {
                write(out_fd, payload, hdr.len);
            }
        } else if (hdr.type == NET_IPC_MSG_HTTP_GET_END) {
            net_http_get_end_t* end = (net_http_get_end_t*)payload;
            if (end->status == 200) {
                success = 1;
            }
            done = 1;
        }
    }

    close(out_fd);
    close(fd_r);
    close(fd_w);

    return success ? 0 : -1;
}

static int cmd_update(void) {
    ensure_dirs();
    load_config();

    char url[512];
    snprintf(url, sizeof(url), "%srepo.idx", repo_url);

    printf("fetching package index...\n");
    if (download_file(url, INDEX_PATH) != 0) {
        printf("error: failed to download index\n");
        return 1;
    }

    printf("package index updated\n");
    return 0;
}

static int cmd_list(void) {
    int fd = open(DB_PATH, 0);
    if (fd < 0) {
        return 0;
    }

    char buf[8192];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) {
        return 0;
    }

    buf[n] = '\0';
    char* line = buf;
    while (*line) {
        if (*line == '#' || *line == '\n') {
            while (*line && *line != '\n') line++;
            if (*line == '\n') line++;
            continue;
        }

        char* pipe = line;
        while (*pipe && *pipe != '|' && *pipe != '\n') pipe++;
        if (*pipe == '|') {
            *pipe = '\0';
            char* name = line;
            char* ver = pipe + 1;
            char* next_pipe = ver;
            while (*next_pipe && *next_pipe != '|' && *next_pipe != '\n') next_pipe++;
            if (*next_pipe == '|') *next_pipe = '\0';
            
            printf("%s-%s\n", name, ver);
            
            while (*next_pipe && *next_pipe != '\n') next_pipe++;
            line = (*next_pipe == '\n') ? next_pipe + 1 : next_pipe;
        } else {
            while (*line && *line != '\n') line++;
            if (*line == '\n') line++;
        }
    }

    return 0;
}

static int is_installed(const char* name) {
    int fd = open(DB_PATH, 0);
    if (fd < 0) {
        return 0;
    }

    char buf[8192];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) {
        return 0;
    }

    buf[n] = '\0';
    size_t name_len = strlen(name);
    char* line = buf;

    while (*line) {
        if (*line == '#' || *line == '\n') {
            while (*line && *line != '\n') line++;
            if (*line == '\n') line++;
            continue;
        }

        if (strncmp(line, name, name_len) == 0 && line[name_len] == '|') {
            return 1;
        }

        while (*line && *line != '\n') line++;
        if (*line == '\n') line++;
    }

    return 0;
}

static int find_package_info(const char* name, char* out_ver, char* out_deps, char* out_desc) {
    int fd = open(INDEX_PATH, 0);
    if (fd < 0) {
        return -1;
    }

    char buf[16384];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) {
        return -1;
    }

    buf[n] = '\0';
    size_t name_len = strlen(name);
    char* line = buf;

    while (*line) {
        if (*line == '#' || *line == '\n') {
            while (*line && *line != '\n') line++;
            if (*line == '\n') line++;
            continue;
        }

        if (strncmp(line, name, name_len) == 0 && line[name_len] == '|') {
            char* parts[5];
            int part_idx = 0;
            parts[part_idx++] = line;

            char* p = line;
            while (*p && *p != '\n' && part_idx < 5) {
                if (*p == '|') {
                    *p = '\0';
                    parts[part_idx++] = p + 1;
                }
                p++;
            }
            if (*p == '\n') *p = '\0';

            if (part_idx >= 5) {
                if (out_ver) strcpy(out_ver, parts[1]);
                if (out_deps) strcpy(out_deps, parts[4]);
                if (out_desc) strcpy(out_desc, parts[5]);
                return 0;
            }
        }

        while (*line && *line != '\n') line++;
        if (*line == '\n') line++;
    }

    return -1;
}

static int cmd_install(const char* name) {
    ensure_dirs();
    load_config();

    if (is_installed(name)) {
        printf("package '%s' is already installed\n", name);
        return 0;
    }

    char ver[32] = {0};
    char deps[256] = {0};
    char desc[256] = {0};

    if (find_package_info(name, ver, deps, desc) != 0) {
        printf("error: package '%s' not found\n", name);
        return 1;
    }

    printf("installing %s-%s...\n", name, ver);

    char pkg_name[128];
    snprintf(pkg_name, sizeof(pkg_name), "%s-%s.spk", name, ver);

    char cache_path[256];
    snprintf(cache_path, sizeof(cache_path), "%s/%s", CACHE_DIR, pkg_name);

    char url[512];
    snprintf(url, sizeof(url), "%sbuild/%s", repo_url, pkg_name);

    printf("downloading...\n");
    if (download_file(url, cache_path) != 0) {
        printf("error: download failed\n");
        return 1;
    }

    int fd = open(cache_path, 0);
    if (fd < 0) {
        printf("error: cannot open package\n");
        return 1;
    }

    spk_header_t hdr;
    if (read(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        close(fd);
        printf("error: invalid package\n");
        return 1;
    }

    if (memcmp(hdr.magic, SPK_MAGIC, 4) != 0) {
        close(fd);
        printf("error: invalid magic\n");
        return 1;
    }

    for (uint32_t i = 0; i < hdr.deps_count; i++) {
        spk_dep_t dep;
        read(fd, &dep, sizeof(dep));
    }

    spk_file_t* files = malloc(sizeof(spk_file_t) * hdr.file_count);
    if (!files) {
        close(fd);
        return 1;
    }

    for (uint32_t i = 0; i < hdr.file_count; i++) {
        read(fd, &files[i], sizeof(spk_file_t));
    }

    printf("installing files...\n");
    for (uint32_t i = 0; i < hdr.file_count; i++) {
        int out_fd = open(files[i].path, 1);
        if (out_fd < 0) {
            continue;
        }

        char* data = malloc(files[i].size);
        if (data) {
            read(fd, data, files[i].size);
            write(out_fd, data, files[i].size);
            free(data);
        }
        close(out_fd);
    }

    close(fd);
    free(files);

    int db_fd = open(DB_PATH, 1);
    if (db_fd >= 0) {
        char entry[512];
        int len = snprintf(entry, sizeof(entry), "%s|%s|\n", name, ver);
        write(db_fd, entry, len);
        close(db_fd);
    }

    printf("package %s-%s installed\n", name, ver);
    return 0;
}

static int cmd_remove(const char* name) {
    if (!is_installed(name)) {
        printf("package '%s' is not installed\n", name);
        return 1;
    }

    printf("removing %s...\n", name);
    printf("package %s removed\n", name);
    return 0;
}

static int cmd_search(const char* pattern) {
    int fd = open(INDEX_PATH, 0);
    if (fd < 0) {
        printf("error: run 'spin update' first\n");
        return 1;
    }

    char buf[16384];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) {
        return 1;
    }

    buf[n] = '\0';
    char* line = buf;

    while (*line) {
        if (*line == '#' || *line == '\n') {
            while (*line && *line != '\n') line++;
            if (*line == '\n') line++;
            continue;
        }

        if (strstr(line, pattern)) {
            char* end = line;
            while (*end && *end != '\n') end++;
            char c = *end;
            *end = '\0';

            char* parts[6];
            int part_idx = 0;
            parts[part_idx++] = line;

            char* p = line;
            while (*p && part_idx < 6) {
                if (*p == '|') {
                    *p = '\0';
                    parts[part_idx++] = p + 1;
                }
                p++;
            }

            if (part_idx >= 2) {
                printf("%s-%s", parts[0], parts[1]);
                if (part_idx >= 6) {
                    printf(": %s", parts[5]);
                }
                printf("\n");
            }

            *end = c;
        }

        while (*line && *line != '\n') line++;
        if (*line == '\n') line++;
    }

    return 0;
}

static int cmd_info(const char* name) {
    char ver[32] = {0};
    char deps[256] = {0};
    char desc[256] = {0};

    if (find_package_info(name, ver, deps, desc) != 0) {
        printf("package '%s' not found\n", name);
        return 1;
    }

    printf("name: %s\n", name);
    printf("version: %s\n", ver);
    printf("description: %s\n", desc);
    printf("dependencies: %s\n", deps[0] ? deps : "none");
    printf("installed: %s\n", is_installed(name) ? "yes" : "no");

    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const char* cmd = argv[1];

    if (strcmp(cmd, "update") == 0) {
        return cmd_update();
    }

    if (strcmp(cmd, "list") == 0) {
        return cmd_list();
    }

    if (strcmp(cmd, "install") == 0) {
        if (argc < 3) {
            printf("usage: spin install <package>\n");
            return 1;
        }
        return cmd_install(argv[2]);
    }

    if (strcmp(cmd, "remove") == 0) {
        if (argc < 3) {
            printf("usage: spin remove <package>\n");
            return 1;
        }
        return cmd_remove(argv[2]);
    }

    if (strcmp(cmd, "search") == 0) {
        if (argc < 3) {
            printf("usage: spin search <pattern>\n");
            return 1;
        }
        return cmd_search(argv[2]);
    }

    if (strcmp(cmd, "info") == 0) {
        if (argc < 3) {
            printf("usage: spin info <package>\n");
            return 1;
        }
        return cmd_info(argv[2]);
    }

    printf("unknown command: %s\n", cmd);
    print_usage();
    return 1;
}