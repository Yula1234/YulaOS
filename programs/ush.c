#include <yula.h>

static int write_all(int fd, const void* buf, uint32_t size) {
    const uint8_t* p = (const uint8_t*)buf;
    uint32_t done = 0;
    while (done < size) {
        int r = write(fd, p + done, size - done);
        if (r <= 0) return -1;
        done += (uint32_t)r;
    }
    return (int)done;
}

static int write_str(int fd, const char* s) {
    if (!s) return 0;
    return write_all(fd, s, (uint32_t)strlen(s));
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int split_args(char* s, char* argv[], int argv_cap) {
    int argc = 0;
    if (!s || !argv || argv_cap <= 0) return 0;

    while (*s) {
        while (*s && is_space(*s)) s++;
        if (!*s) break;
        if (argc + 1 >= argv_cap) break;

        argv[argc++] = s;
        while (*s && !is_space(*s)) s++;
        if (*s) {
            *s = '\0';
            s++;
        }
    }

    argv[argc] = 0;
    return argc;
}

static void ush_print_prompt(int fd_out) {
    char cwd[256];
    int n = getcwd(cwd, (uint32_t)sizeof(cwd));
    if (n > 0) {
        write_str(fd_out, cwd);
        write_str(fd_out, " > ");
        return;
    }
    write_str(fd_out, "> ");
}

static int spawn_by_name(const char* name, int argc, char** argv) {
    if (!name || !*name) return -1;

    if (name[0] == '/') {
        return spawn_process(name, argc, argv);
    }

    const size_t n = strlen(name);
    const int has_exe = (n >= 4 && strcmp(name + (n - 4u), ".exe") == 0);

    char path1[128];
    char path2[128];
    if (has_exe) {
        (void)snprintf(path1, sizeof(path1), "/bin/%s", name);
        (void)snprintf(path2, sizeof(path2), "/bin/usr/%s", name);
    } else {
        (void)snprintf(path1, sizeof(path1), "/bin/%s.exe", name);
        (void)snprintf(path2, sizeof(path2), "/bin/usr/%s.exe", name);
    }

    int pid = spawn_process(path1, argc, argv);
    if (pid < 0) pid = spawn_process(path2, argc, argv);
    return pid;
}

static int read_line(int fd_in, int fd_out, char* buf, int cap) {
    if (!buf || cap <= 1) return -1;
    int len = 0;

    for (;;) {
        char c = 0;
        int r = read(fd_in, &c, 1);
        if (r <= 0) return -1;

        if (c == '\r' || c == '\n') {
            write_str(fd_out, "\n");
            buf[len] = '\0';
            return len;
        }

        if (c == 0x08) {
            if (len > 0) {
                len--;
                write_str(fd_out, "\b \b");
            }
            continue;
        }

        if ((uint8_t)c < 32u || (uint8_t)c > 126u) {
            continue;
        }

        if (len + 1 >= cap) {
            continue;
        }

        buf[len++] = c;
        (void)write_all(fd_out, &c, 1);
    }
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    for (int fd = 3; fd < 64; fd++) {
        (void)close(fd);
    }

    char line[256];
    char* args[32];

    for (;;) {
        ush_print_prompt(1);
        int n = read_line(0, 1, line, (int)sizeof(line));
        if (n < 0) break;

        int ac = split_args(line, args, (int)(sizeof(args) / sizeof(args[0])));
        if (ac <= 0) continue;

        if (strcmp(args[0], "exit") == 0) {
            break;
        }

        if (strcmp(args[0], "cd") == 0) {
            const char* path = (ac > 1) ? args[1] : "/";
            if (chdir(path) != 0) {
                write_str(1, "cd: failed\n");
            }
            continue;
        }

        if (strcmp(args[0], "pwd") == 0) {
            char cwd[256];
            int rn = getcwd(cwd, (uint32_t)sizeof(cwd));
            if (rn > 0) {
                write_str(1, cwd);
                write_str(1, "\n");
            } else {
                write_str(1, "pwd: failed\n");
            }
            continue;
        }

        if (strcmp(args[0], "clear") == 0) {
            write_str(1, "\x1b[2J\x1b[H");
            continue;
        }

        int pid = spawn_by_name(args[0], ac, args);
        if (pid < 0) {
            write_str(1, "ush: spawn failed\n");
            continue;
        }

        int st = 0;
        (void)waitpid(pid, &st);
    }

    return 0;
}
