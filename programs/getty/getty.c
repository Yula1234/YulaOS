#include <yula.h>

static int write_str_fd(int fd, const char* s) {
    if (!s) {
        return 0;
    }

    uint32_t len = (uint32_t)strlen(s);
    if (len == 0) {
        return 0;
    }

    return write(fd, s, len);
}

static int dup2_stdio_from(int fd) {
    if (dup2(fd, 0) < 0) {
        return -1;
    }
    if (dup2(fd, 1) < 0) {
        return -1;
    }
    if (dup2(fd, 2) < 0) {
        return -1;
    }

    return 0;
}

static int is_tty_fd(int fd) {
    yos_termios_t t;
    return ioctl(fd, YOS_TCGETS, &t) == 0;
}

typedef enum {
    GETTY_OK = 0,
    GETTY_TRANSIENT = 1,
    GETTY_FATAL = 2,
} getty_status_t;

static getty_status_t run_getty_once(const char* tty_path, const char* prog_name) {
    int fd = open(tty_path, 0);
    if (fd < 0) {
        write_str_fd(2, "getty: open failed\n");
        return GETTY_TRANSIENT;
    }

    if (!is_tty_fd(fd)) {
        (void)close(fd);
        write_str_fd(2, "getty: not a tty\n");
        return GETTY_FATAL;
    }

    if (setsid() < 0) {
        (void)close(fd);
        write_str_fd(2, "getty: setsid failed\n");
        return GETTY_TRANSIENT;
    }

    uint32_t pgid = (uint32_t)getpid();
    if (setpgid(pgid) < 0) {
        (void)close(fd);
        write_str_fd(2, "getty: setpgid failed\n");
        return GETTY_TRANSIENT;
    }

    if (ioctl(fd, YOS_TIOCSCTTY, 0) < 0) {
        (void)close(fd);
        write_str_fd(2, "getty: TIOCSCTTY failed\n");
        return GETTY_TRANSIENT;
    }

    if (ioctl(fd, YOS_TCSETPGRP, &pgid) < 0) {
        (void)close(fd);
        write_str_fd(2, "getty: TCSETPGRP failed\n");
        return GETTY_TRANSIENT;
    }

    if (dup2_stdio_from(fd) != 0) {
        (void)close(fd);
        write_str_fd(2, "getty: dup2 failed\n");
        return GETTY_TRANSIENT;
    }

    if (fd > 2) {
        (void)close(fd);
    }

    (void)write_str_fd(1, "YulaOS serial getty\n");

    char* argv[2];
    argv[0] = (char*)prog_name;
    argv[1] = 0;

    int pid = spawn_process_resolved(prog_name, 1, argv);
    if (pid < 0) {
        (void)write_str_fd(2, "getty: spawn failed\n");
        return GETTY_TRANSIENT;
    }

    int st = 0;
    (void)waitpid(pid, &st);

    return GETTY_OK;
}

static int spawn_loop(const char* tty_path, const char* prog_name, int once) {
    if (!tty_path || !*tty_path) {
        return 1;
    }

    if (!prog_name || !*prog_name) {
        prog_name = "ush";
    }

    for (;;) {
        getty_status_t rc = run_getty_once(tty_path, prog_name);
        if (once) {
            return rc;
        }

        if (rc == GETTY_FATAL) {
            return rc;
        }

        usleep(250000);
    }
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv) {
        write_str_fd(2, "usage: getty [--once] <tty-path> [program]\n");
        return 1;
    }

    int once = 0;
    int argi = 1;
    if (argv[argi] && strcmp(argv[argi], "--once") == 0) {
        once = 1;
        argi++;
    }

    if (!argv[argi]) {
        write_str_fd(2, "usage: getty [--once] <tty-path> [program]\n");
        return 1;
    }

    const char* tty_path = argv[argi++];
    const char* prog_name = argv[argi] ? argv[argi] : "ush";

    return spawn_loop(tty_path, prog_name, once);
}
