#include <yula.h>

static int write_all(int fd, const void* buf, uint32_t size) {
    const uint8_t* p = (const uint8_t*)buf;
    uint32_t done = 0;
    while (done < size) {
        uintptr_t a = (uintptr_t)(p + done);
        uintptr_t b = a + (uintptr_t)(size - done);
        if (b < a) {
            return -1;
        }
        if (a < 0x08000000u || b > 0xC0000000u) {
            return -1;
        }
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

static void write_int(int fd, int v) {
    char buf[16];
    uint32_t u = (v < 0) ? (uint32_t)(-v) : (uint32_t)v;
    int pos = 0;

    if (v < 0) {
        buf[pos++] = '-';
    }

    char tmp[10];
    int n = 0;
    if (u == 0) {
        tmp[n++] = '0';
    } else {
        while (u != 0 && n < (int)sizeof(tmp)) {
            tmp[n++] = (char)('0' + (u % 10u));
            u /= 10u;
        }
    }

    while (n > 0 && pos < (int)sizeof(buf)) {
        buf[pos++] = tmp[--n];
    }

    if (pos > 0) {
        (void)write_all(fd, buf, (uint32_t)pos);
    }
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

typedef enum {
    USH_TOK_WORD = 0,
    USH_TOK_PIPE,
    USH_TOK_REDIR_IN,
    USH_TOK_REDIR_OUT,
    USH_TOK_REDIR_OUT_APPEND,
    USH_TOK_BACKGROUND,
} ush_tok_kind_t;

typedef struct {
    ush_tok_kind_t kind;
    char* text;
} ush_tok_t;

static int spawn_by_name(const char* name, int argc, char** argv);

static char* ush_strdup_printf(const char* fmt, const char* a) {
    if (!fmt) return 0;
    char tmp[256];
    int r = snprintf(tmp, sizeof(tmp), fmt, a ? a : "");
    if (r < 0) return 0;
    tmp[sizeof(tmp) - 1] = '\0';
    return strdup(tmp);
}

static int ush_tok_push(ush_tok_t** io_toks, int* io_cnt, int* io_cap, ush_tok_t t) {
    if (!io_toks || !io_cnt || !io_cap) return 0;
    if (*io_cnt >= *io_cap) {
        int nc = (*io_cap > 0) ? (*io_cap * 2) : 32;
        ush_tok_t* nt = (ush_tok_t*)realloc(*io_toks, (size_t)nc * sizeof((*io_toks)[0]));
        if (!nt) return 0;
        *io_toks = nt;
        *io_cap = nc;
    }
    (*io_toks)[(*io_cnt)++] = t;
    return 1;
}

static void ush_tokens_free_all(ush_tok_t* toks, int cnt) {
    if (!toks) return;
    for (int i = 0; i < cnt; i++) {
        if (toks[i].kind == USH_TOK_WORD) {
            free(toks[i].text);
            toks[i].text = 0;
        }
    }
    free(toks);
}

static int ush_buf_ensure_cap(char** buf, size_t* cap, size_t need) {
    if (!buf || !cap) return 0;
    if (need <= *cap) return 1;
    size_t nc = (*cap > 0) ? *cap : 64u;
    while (nc < need) nc *= 2u;
    char* nb = (char*)realloc(*buf, nc);
    if (!nb) return 0;
    *buf = nb;
    *cap = nc;
    return 1;
}

static int ush_tokenize_alloc(const char* s, ush_tok_t** out_toks, int* out_cnt, char** out_err) {
    if (out_err) *out_err = 0;
    if (out_toks) *out_toks = 0;
    if (out_cnt) *out_cnt = 0;
    if (!s || !out_toks || !out_cnt) return -1;

    ush_tok_t* toks = 0;
    int cnt = 0;
    int cap = 0;

    const char* p = s;
    while (*p) {
        while (*p && is_space(*p)) p++;
        if (!*p) break;

        if (*p == '|') {
            ush_tok_t t = { .kind = USH_TOK_PIPE, .text = 0 };
            if (!ush_tok_push(&toks, &cnt, &cap, t)) goto oom;
            p++;
            continue;
        }
        if (*p == '<') {
            ush_tok_t t = { .kind = USH_TOK_REDIR_IN, .text = 0 };
            if (!ush_tok_push(&toks, &cnt, &cap, t)) goto oom;
            p++;
            continue;
        }
        if (*p == '>') {
            ush_tok_t t;
            if (p[1] == '>') {
                t.kind = USH_TOK_REDIR_OUT_APPEND;
                t.text = 0;
                if (!ush_tok_push(&toks, &cnt, &cap, t)) goto oom;
                p += 2;
            } else {
                t.kind = USH_TOK_REDIR_OUT;
                t.text = 0;
                if (!ush_tok_push(&toks, &cnt, &cap, t)) goto oom;
                p++;
            }
            continue;
        }
        if (*p == '&') {
            ush_tok_t t = { .kind = USH_TOK_BACKGROUND, .text = 0 };
            if (!ush_tok_push(&toks, &cnt, &cap, t)) goto oom;
            p++;
            continue;
        }

        char* wbuf = 0;
        size_t wcap = 0;
        size_t wlen = 0;
        int quote = 0;

        while (*p) {
            char c = *p;
            if (quote == 0) {
                if (is_space(c) || c == '|' || c == '<' || c == '>') break;
                if (c == '\\') {
                    if (p[1]) {
                        p++;
                        c = *p;
                        if (!ush_buf_ensure_cap(&wbuf, &wcap, wlen + 2)) { free(wbuf); goto oom; }
                        wbuf[wlen++] = c;
                        p++;
                        continue;
                    }
                    p++;
                    continue;
                }
                if (c == '\'' || c == '"') {
                    quote = c;
                    p++;
                    continue;
                }
                if (!ush_buf_ensure_cap(&wbuf, &wcap, wlen + 2)) { free(wbuf); goto oom; }
                wbuf[wlen++] = c;
                p++;
                continue;
            }

            if (c == quote) {
                quote = 0;
                p++;
                continue;
            }

            if (quote == '"' && c == '\\') {
                if (p[1]) {
                    p++;
                    c = *p;
                    if (!ush_buf_ensure_cap(&wbuf, &wcap, wlen + 2)) { free(wbuf); goto oom; }
                    wbuf[wlen++] = c;
                    p++;
                    continue;
                }
                p++;
                continue;
            }

            if (!ush_buf_ensure_cap(&wbuf, &wcap, wlen + 2)) { free(wbuf); goto oom; }
            wbuf[wlen++] = c;
            p++;
        }

        if (quote != 0) {
            free(wbuf);
            if (out_err) *out_err = ush_strdup_printf("ush: unterminated quote: %s\n", "");
            ush_tokens_free_all(toks, cnt);
            return -1;
        }

        if (!ush_buf_ensure_cap(&wbuf, &wcap, wlen + 1)) { free(wbuf); goto oom; }
        wbuf[wlen] = '\0';

        char* word = strdup(wbuf);
        free(wbuf);
        if (!word) goto oom;

        ush_tok_t t = { .kind = USH_TOK_WORD, .text = word };
        if (!ush_tok_push(&toks, &cnt, &cap, t)) {
            free(word);
            goto oom;
        }
    }

    *out_toks = toks;
    *out_cnt = cnt;
    return 0;

oom:
    if (out_err) *out_err = ush_strdup_printf("ush: out of memory: %s\n", "");
    ush_tokens_free_all(toks, cnt);
    return -1;
}

typedef struct {
    char** argv;
    int argc;
    int cap;

    const char* in_path;
    const char* out_path;
    int out_append;
} ush_cmd_t;

typedef struct {
    ush_cmd_t* cmds;
    int count;
    int cap;

    char** owned_words;
    int owned_count;
    int owned_cap;

    int background;
} ush_pipeline_t;

static void ush_cmd_init(ush_cmd_t* c) {
    if (!c) return;
    c->argv = 0;
    c->argc = 0;
    c->cap = 0;
    c->in_path = 0;
    c->out_path = 0;
    c->out_append = 0;
}

static void ush_cmd_destroy(ush_cmd_t* c) {
    if (!c) return;
    free(c->argv);
    ush_cmd_init(c);
}

static int ush_cmd_push_arg(ush_cmd_t* c, char* s) {
    if (!c || !s || !*s) return 0;
    if (c->argc + 2 > c->cap) {
        int nc = (c->cap > 0) ? (c->cap * 2) : 8;
        while (nc < c->argc + 2) nc *= 2;
        char** nv = (char**)realloc(c->argv, (size_t)nc * sizeof(c->argv[0]));
        if (!nv) return 0;
        c->argv = nv;
        c->cap = nc;
    }
    c->argv[c->argc++] = s;
    c->argv[c->argc] = 0;
    return 1;
}

static void ush_pipeline_init(ush_pipeline_t* p) {
    if (!p) return;
    p->cmds = 0;
    p->count = 0;
    p->cap = 0;

    p->owned_words = 0;
    p->owned_count = 0;
    p->owned_cap = 0;

    p->background = 0;
}

static void ush_pipeline_destroy(ush_pipeline_t* p) {
    if (!p) return;
    for (int i = 0; i < p->count; i++) {
        ush_cmd_destroy(&p->cmds[i]);
    }
    free(p->cmds);

    for (int i = 0; i < p->owned_count; i++) {
        free(p->owned_words[i]);
    }
    free(p->owned_words);

    p->cmds = 0;
    p->count = 0;
    p->cap = 0;

    p->owned_words = 0;
    p->owned_count = 0;
    p->owned_cap = 0;

    p->background = 0;
}

static int ush_pipeline_add_owned_word(ush_pipeline_t* p, char* s) {
    if (!p || !s) return 0;
    if (p->owned_count >= p->owned_cap) {
        int nc = (p->owned_cap > 0) ? (p->owned_cap * 2) : 32;
        char** nw = (char**)realloc(p->owned_words, (size_t)nc * sizeof(p->owned_words[0]));
        if (!nw) return 0;
        p->owned_words = nw;
        p->owned_cap = nc;
    }
    p->owned_words[p->owned_count++] = s;
    return 1;
}

static ush_cmd_t* ush_pipeline_push_cmd(ush_pipeline_t* p) {
    if (!p) return 0;
    if (p->count >= p->cap) {
        int nc = (p->cap > 0) ? (p->cap * 2) : 4;
        ush_cmd_t* nn = (ush_cmd_t*)realloc(p->cmds, (size_t)nc * sizeof(p->cmds[0]));
        if (!nn) return 0;
        for (int i = p->cap; i < nc; i++) {
            ush_cmd_init(&nn[i]);
        }
        p->cmds = nn;
        p->cap = nc;
    }
    ush_cmd_t* c = &p->cmds[p->count++];
    ush_cmd_init(c);
    return c;
}

static int ush_parse_tokens(const ush_tok_t* toks, int cnt, ush_pipeline_t* out_pl, char** out_err) {
    if (out_err) *out_err = 0;
    if (!out_pl) return -1;
    ush_pipeline_init(out_pl);

    if (!toks || cnt == 0) return 0;

    ush_cmd_t* cur = ush_pipeline_push_cmd(out_pl);
    if (!cur) goto oom;

    for (int i = 0; i < cnt; i++) {
        const ush_tok_t t = toks[i];
        if (t.kind == USH_TOK_WORD) {
            if (!ush_cmd_push_arg(cur, t.text)) goto oom;
            continue;
        }

        if (t.kind == USH_TOK_PIPE) {
            if (cur->argc == 0) {
                if (out_err) *out_err = ush_strdup_printf("ush: syntax error near '|': %s\n", "");
                goto fail;
            }
            cur = ush_pipeline_push_cmd(out_pl);
            if (!cur) goto oom;
            continue;
        }

        if (t.kind == USH_TOK_REDIR_IN || t.kind == USH_TOK_REDIR_OUT || t.kind == USH_TOK_REDIR_OUT_APPEND) {
            if (i + 1 >= cnt || toks[i + 1].kind != USH_TOK_WORD) {
                if (out_err) *out_err = ush_strdup_printf("ush: redirection without path: %s\n", "");
                goto fail;
            }
            const char* path = toks[i + 1].text;
            if (!path || !*path) {
                if (out_err) *out_err = ush_strdup_printf("ush: invalid redirection path: %s\n", "");
                goto fail;
            }

            if (t.kind == USH_TOK_REDIR_IN) {
                if (cur->in_path) {
                    if (out_err) *out_err = ush_strdup_printf("ush: duplicate input redirection: %s\n", "");
                    goto fail;
                }
                cur->in_path = path;
            } else {
                if (cur->out_path) {
                    if (out_err) *out_err = ush_strdup_printf("ush: duplicate output redirection: %s\n", "");
                    goto fail;
                }
                cur->out_path = path;
                cur->out_append = (t.kind == USH_TOK_REDIR_OUT_APPEND);
            }
            i++;
            continue;
        }

        if (t.kind == USH_TOK_BACKGROUND) {
            if (i != cnt - 1) {
                if (out_err) *out_err = ush_strdup_printf("ush: syntax error near '&': %s\n", "");
                goto fail;
            }
            if (cur->argc == 0) {
                if (out_err) *out_err = ush_strdup_printf("ush: syntax error near '&': %s\n", "");
                goto fail;
            }
            out_pl->background = 1;
            continue;
        }
    }

    if (out_pl->count > 0 && out_pl->cmds[out_pl->count - 1].argc == 0) {
        if (out_err) *out_err = ush_strdup_printf("ush: syntax error: trailing '|': %s\n", "");
        goto fail;
    }
    return 0;

oom:
    if (out_err) *out_err = ush_strdup_printf("ush: out of memory: %s\n", "");
fail:
    ush_pipeline_destroy(out_pl);
    return -1;
}

static int ush_parse_line(char* line, ush_pipeline_t* out_pl, char** out_err) {
    if (out_err) *out_err = 0;
    if (!line || !out_pl) return -1;

    ush_tok_t* toks = 0;
    int cnt = 0;
    char* err = 0;

    if (ush_tokenize_alloc(line, &toks, &cnt, &err) != 0) {
        if (out_err) *out_err = err;
        else free(err);
        return -1;
    }

    int rc = ush_parse_tokens(toks, cnt, out_pl, &err);
    if (rc != 0) {
        ush_tokens_free_all(toks, cnt);
        if (out_err) *out_err = err;
        else free(err);
        return -1;
    }

    for (int i = 0; i < cnt; i++) {
        if (toks[i].kind == USH_TOK_WORD && toks[i].text) {
            if (!ush_pipeline_add_owned_word(out_pl, toks[i].text)) {
                ush_tokens_free_all(toks, cnt);
                free(err);
                ush_pipeline_destroy(out_pl);
                if (out_err) *out_err = ush_strdup_printf("ush: out of memory: %s\n", "");
                return -1;
            }
            toks[i].text = 0;
        }
    }

    ush_tokens_free_all(toks, cnt);
    free(err);
    return 0;
}

static void ush_restore_stdio(int save0, int save1, int save2) {
    (void)dup2(save0, 0);
    (void)dup2(save1, 1);
    (void)dup2(save2, 2);
}

static int ush_save_stdio(int save0, int save1, int save2) {
    if (dup2(0, save0) < 0) {
        return -1;
    }
    if (dup2(1, save1) < 0) {
        return -1;
    }
    if (dup2(2, save2) < 0) {
        return -1;
    }

    return 0;
}

static void ush_close_fd(int* fd) {
    if (!fd) return;
    if (*fd >= 0) (void)close(*fd);
    *fd = -1;
}

static int ush_apply_single_redirs(const ush_cmd_t* c, int save0, int save1, int save2, int* io_in_fd, int* io_out_fd) {
    if (io_in_fd) *io_in_fd = -1;
    if (io_out_fd) *io_out_fd = -1;
    if (!c) return 0;

    if (c->in_path) {
        int fd = open(c->in_path, 0);
        if (fd < 0) return -1;
        if (dup2(fd, 0) < 0) { (void)close(fd); return -1; }
        if (io_in_fd) *io_in_fd = fd;
        else (void)close(fd);
    } else {
        (void)dup2(save0, 0);
    }

    if (c->out_path) {
        int fd = open(c->out_path, c->out_append ? 2 : 1);
        if (fd < 0) return -1;
        if (dup2(fd, 1) < 0) { (void)close(fd); return -1; }
        if (io_out_fd) *io_out_fd = fd;
        else (void)close(fd);
    } else {
        (void)dup2(save1, 1);
    }

    (void)dup2(save2, 2);
    return 0;
}

static int ush_exec_pipeline(const ush_pipeline_t* pl) {
    if (!pl || pl->count <= 0) return 0;

    uint32_t shell_pgid = getpgrp();
    uint32_t job_pgid = 0;

    const int save0 = 60;
    const int save1 = 61;
    const int save2 = 62;

    if (ush_save_stdio(save0, save1, save2) != 0) return -1;

    int* pids = (int*)malloc((size_t)pl->count * sizeof(pids[0]));
    if (!pids) {
        ush_restore_stdio(save0, save1, save2);
        (void)close(save0);
        (void)close(save1);
        (void)close(save2);
        return -1;
    }
    for (int i = 0; i < pl->count; i++) pids[i] = -1;

    int prev_read = -1;
    int spawned = 0;

    int pipe_fds[2] = { -1, -1 };
    int in_fd = -1;
    int out_fd = -1;

    const ush_cmd_t* c = 0;
    for (int i = 0; i < pl->count; i++) {
        c = &pl->cmds[i];
        if (c->argc <= 0 || !c->argv || !c->argv[0]) {
            continue;
        }

        pipe_fds[0] = -1;
        pipe_fds[1] = -1;
        in_fd = -1;
        out_fd = -1;

        if (i + 1 < pl->count) {
            if (pipe(pipe_fds) != 0) {
                write_str(save2, "ush: pipe failed\n");
                goto fail;
            }
        }

        if (c->in_path) {
            in_fd = open(c->in_path, 0);
            if (in_fd < 0) {
                write_str(save2, "ush: open < failed\n");
                goto fail;
            }
        } else if (prev_read >= 0) {
            in_fd = prev_read;
        }

        if (c->out_path) {
            out_fd = open(c->out_path, c->out_append ? 2 : 1);
            if (out_fd < 0) {
                write_str(save2, "ush: open > failed\n");
                goto fail;
            }
        } else if (pipe_fds[1] >= 0) {
            out_fd = pipe_fds[1];
        }

        if (in_fd >= 0) {
            if (dup2(in_fd, 0) < 0) {
                write_str(save2, "ush: dup2 stdin failed\n");
                goto fail;
            }
        } else {
            (void)dup2(save0, 0);
        }

        if (out_fd >= 0) {
            if (dup2(out_fd, 1) < 0) {
                write_str(save2, "ush: dup2 stdout failed\n");
                goto fail;
            }
        } else {
            (void)dup2(save1, 1);
        }
        (void)dup2(save2, 2);

        int pid = spawn_by_name(c->argv[0], c->argc, c->argv);
        if (pid < 0) {
            write_str(save2, "ush: spawn failed\n");
            goto fail;
        }

        if (job_pgid == 0u) {
            job_pgid = (uint32_t)pid;
        }
        (void)setpgid_pid((uint32_t)pid, job_pgid);

        pids[spawned++] = pid;

        ush_close_fd(&pipe_fds[1]);
        ush_close_fd(&prev_read);
        if (c->in_path) ush_close_fd(&in_fd);
        if (c->out_path) ush_close_fd(&out_fd);

        prev_read = pipe_fds[0];
        pipe_fds[0] = -1;

        ush_restore_stdio(save0, save1, save2);
    }

    if (prev_read >= 0) {
        (void)close(prev_read);
        prev_read = -1;
    }

    ush_restore_stdio(save0, save1, save2);
    (void)close(save0);
    (void)close(save1);
    (void)close(save2);

    if (!pl->background) {
        if (job_pgid != 0u) {
            (void)ioctl(0, YOS_TCSETPGRP, &job_pgid);
        }

        for (int i = 0; i < spawned; i++) {
            int st = 0;
            (void)waitpid(pids[i], &st);
        }

        if (shell_pgid != 0u) {
            (void)ioctl(0, YOS_TCSETPGRP, &shell_pgid);
        }
    } else {
        if (spawned > 0) {
            char msg[64];
            int r = snprintf(msg, sizeof(msg), "[%d]\n", pids[spawned - 1]);
            if (r > 0) (void)write_all(1, msg, (uint32_t)r);
        }
    }

    free(pids);
    return 0;

fail:
    if (in_fd >= 0 && in_fd == prev_read) prev_read = -1;
    if (out_fd >= 0 && out_fd == pipe_fds[1]) pipe_fds[1] = -1;

    ush_close_fd(&pipe_fds[0]);
    ush_close_fd(&pipe_fds[1]);
    ush_close_fd(&in_fd);
    ush_close_fd(&out_fd);
    ush_close_fd(&prev_read);
    ush_restore_stdio(save0, save1, save2);
    (void)close(save0);
    (void)close(save1);
    (void)close(save2);

    for (int i = 0; i < spawned; i++) {
        if (pids[i] > 0) (void)kill(pids[i]);
    }
    for (int i = 0; i < spawned; i++) {
        int st = 0;
        if (pids[i] > 0) (void)waitpid(pids[i], &st);
    }
    free(pids);
    return -1;
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
    return spawn_process_resolved(name, argc, argv);
}

static int is_builtin_cmd(const char* name) {
    if (!name || !*name) return 0;
    if (strcmp(name, "exit") == 0) return 1;
    if (strcmp(name, "cd") == 0) return 1;
    if (strcmp(name, "pwd") == 0) return 1;
    if (strcmp(name, "clear") == 0) return 1;
    return 0;
}

typedef enum {
    USH_KEY_NONE = 0,
    USH_KEY_CHAR,
    USH_KEY_ENTER,
    USH_KEY_BACKSPACE,
    USH_KEY_DELETE,
    USH_KEY_LEFT,
    USH_KEY_RIGHT,
    USH_KEY_UP,
    USH_KEY_DOWN,
    USH_KEY_SCROLL_UP,
    USH_KEY_SCROLL_DOWN,
    USH_KEY_HOME,
    USH_KEY_END,
    USH_KEY_CTRL_C,
    USH_KEY_ERROR,
} ush_key_kind_t;

typedef struct {
    ush_key_kind_t kind;
    char ch;
} ush_key_t;

typedef struct {
    char** lines;
    int count;
    int cap;
} ush_history_t;

static void hist_init(ush_history_t* h) {
    if (!h) return;
    h->lines = 0;
    h->count = 0;
    h->cap = 0;
}

static void hist_destroy(ush_history_t* h) {
    if (!h) return;
    for (int i = 0; i < h->count; i++) {
        free(h->lines[i]);
    }
    free(h->lines);
    h->lines = 0;
    h->count = 0;
    h->cap = 0;
}

static int hist_ensure_cap(ush_history_t* h, int need) {
    if (!h) return 0;
    if (need <= h->cap) return 1;
    int new_cap = (h->cap > 0) ? h->cap : 16;
    while (new_cap < need) new_cap *= 2;
    char** nl = (char**)realloc(h->lines, (size_t)new_cap * sizeof(h->lines[0]));
    if (!nl) return 0;
    h->lines = nl;
    h->cap = new_cap;
    return 1;
}

static void hist_add(ush_history_t* h, const char* line) {
    if (!h || !line) return;
    while (*line && is_space(*line)) line++;
    if (!*line) return;

    const int max_hist = 128;
    if (h->count > 0) {
        const char* prev = h->lines[h->count - 1];
        if (prev && strcmp(prev, line) == 0) return;
    }

    char* dup = strdup(line);
    if (!dup) return;

    if (h->count >= max_hist) {
        free(h->lines[0]);
        memmove(&h->lines[0], &h->lines[1], (size_t)(h->count - 1) * sizeof(h->lines[0]));
        h->count--;
    }

    if (!hist_ensure_cap(h, h->count + 1)) {
        free(dup);
        return;
    }
    h->lines[h->count++] = dup;
}

static int term_get_size(int fd, int* out_cols, int* out_rows) {
    if (out_cols) *out_cols = 80;
    if (out_rows) *out_rows = 25;
    yos_winsize_t ws;
    if (ioctl(fd, YOS_TIOCGWINSZ, &ws) != 0) {
        return 0;
    }
    if (ws.ws_col > 0 && out_cols) *out_cols = (int)ws.ws_col;
    if (ws.ws_row > 0 && out_rows) *out_rows = (int)ws.ws_row;
    return 1;
}

static int term_is_ansi(int fd) {
    yos_termios_t t;
    return ioctl(fd, YOS_TCGETS, &t) == 0;
}

static void term_scroll(int fd, int delta) {
    yos_tty_scroll_t s;
    s.delta = delta;
    (void)ioctl(fd, YOS_TTY_SCROLL, &s);
}

static void term_scroll_reset(int fd) {
    yos_tty_scroll_t s;
    s.delta = 0;
    (void)ioctl(fd, YOS_TTY_SCROLL, &s);
}

static int ush_make_prompt(char* out, uint32_t cap) {
    if (!out || cap == 0) return 0;
    char cwd[256];
    int n = getcwd(cwd, (uint32_t)sizeof(cwd));
    if (n > 0) {
        int r = snprintf(out, (size_t)cap, "%s > ", cwd);
        if (r < 0) return 0;
        if ((uint32_t)r >= cap) r = (int)cap - 1;
        return r;
    }
    int r = snprintf(out, (size_t)cap, "> ");
    if (r < 0) return 0;
    if ((uint32_t)r >= cap) r = (int)cap - 1;
    return r;
}

static int read_byte_blocking(int fd_in, char* out) {
    if (!out) return -1;
    for (;;) {
        char c = 0;
        int r = read(fd_in, &c, 1);
        if (r == 1) {
            *out = c;
            return 1;
        }
        if (r == -2) {
            return -2;
        }
        return r;
    }
}

static int read_byte_timeout(int fd_in, char* out, int timeout_ms) {
    if (!out) return 0;
    pollfd_t pfd;
    pfd.fd = fd_in;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0) return 0;
    if ((pfd.revents & POLLIN) == 0) return 0;
    return read_byte_blocking(fd_in, out);
}

static ush_key_t read_key(int fd_in, int timeout_ms) {
    ush_key_t k;
    k.kind = USH_KEY_NONE;
    k.ch = 0;

    char c = 0;
    int r = (timeout_ms < 0) ? read_byte_blocking(fd_in, &c) : read_byte_timeout(fd_in, &c, timeout_ms);
    if (r == 0) {
        return k;
    }
    if (r == -2) {
        k.kind = USH_KEY_CTRL_C;
        return k;
    }
    if (r != 1) {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            write_str(2, "ush: input read error: ");
            write_int(2, r);
            write_str(2, "\n");
        }
        return k;
    }

    if (c == '\r' || c == '\n') {
        k.kind = USH_KEY_ENTER;
        return k;
    }
    if ((uint8_t)c == 0x03u) {
        k.kind = USH_KEY_CTRL_C;
        return k;
    }
    if (c == '\b' || (uint8_t)c == 0x7Fu) {
        k.kind = USH_KEY_BACKSPACE;
        return k;
    }
    if ((uint8_t)c == 0x11u) { k.kind = USH_KEY_LEFT; return k; }
    if ((uint8_t)c == 0x12u) { k.kind = USH_KEY_RIGHT; return k; }
    if ((uint8_t)c == 0x13u) { k.kind = USH_KEY_UP; return k; }
    if ((uint8_t)c == 0x14u) { k.kind = USH_KEY_DOWN; return k; }
    if ((uint8_t)c == 0x80u) { k.kind = USH_KEY_SCROLL_UP; return k; }
    if ((uint8_t)c == 0x81u) { k.kind = USH_KEY_SCROLL_DOWN; return k; }

    if ((uint8_t)c == 0x1Bu) {
        char c1 = 0;
        int r1 = read_byte_timeout(fd_in, &c1, 20);
        if (r1 != 1) {
            return k;
        }
        if (c1 != '[') {
            return k;
        }
        char c2 = 0;
        int r2 = read_byte_timeout(fd_in, &c2, 20);
        if (r2 != 1) {
            return k;
        }
        if (c2 == 'A') { k.kind = USH_KEY_UP; return k; }
        if (c2 == 'B') { k.kind = USH_KEY_DOWN; return k; }
        if (c2 == 'C') { k.kind = USH_KEY_RIGHT; return k; }
        if (c2 == 'D') { k.kind = USH_KEY_LEFT; return k; }
        if (c2 == 'H') { k.kind = USH_KEY_HOME; return k; }
        if (c2 == 'F') { k.kind = USH_KEY_END; return k; }
        if (c2 == '3') {
            char c3 = 0;
            int r3 = read_byte_timeout(fd_in, &c3, 20);
            if (r3 == 1 && c3 == '~') {
                k.kind = USH_KEY_DELETE;
                return k;
            }
        }
        return k;
    }

    if ((uint8_t)c >= 32u && (uint8_t)c <= 126u) {
        k.kind = USH_KEY_CHAR;
        k.ch = c;
        return k;
    }

    return k;
}

static int line_ensure_cap(char** buf, size_t* cap, size_t need) {
    if (!buf || !cap) return 0;
    if (need <= *cap) return 1;
    size_t new_cap = (*cap > 0) ? *cap : 128u;
    while (new_cap < need) new_cap *= 2u;
    char* nb = (char*)realloc(*buf, new_cap);
    if (!nb) return 0;
    *buf = nb;
    *cap = new_cap;
    return 1;
}

static void ansi_write_csi_num(int fd_out, char cmd, int n) {
    char tmp[32];
    if (n <= 0) return;
    int r = snprintf(tmp, sizeof(tmp), "\x1b[%d%c", n, cmd);
    if (r > 0) (void)write_all(fd_out, tmp, (uint32_t)r);
}

static void ansi_clear_line(int fd_out) {
    const char seq[] = "\x1b[2K";
    (void)write_all(fd_out, seq, (uint32_t)(sizeof(seq) - 1u));
}

static void ansi_redraw_line(
    int fd_out,
    const char* prompt,
    int prompt_len,
    const char* line,
    size_t line_len,
    size_t cursor,
    int cols,
    int* io_prev_rows,
    int* io_prev_cursor_row
) {
    if (!prompt || !line || cols <= 0 || !io_prev_rows || !io_prev_cursor_row) return;

    const int prev_rows = (*io_prev_rows > 0) ? *io_prev_rows : 1;
    const int prev_cursor_row = (*io_prev_cursor_row >= 0) ? *io_prev_cursor_row : 0;

    const int total_len = prompt_len + (int)line_len;
    const int rows = (total_len / cols) + 1;

    const int cursor_abs = prompt_len + (int)cursor;
    const int cursor_row = cursor_abs / cols;
    const int cursor_col = cursor_abs % cols;

    const int clear_rows = (rows > prev_rows) ? rows : prev_rows;

    (void)write_all(fd_out, "\r", 1u);
    ansi_write_csi_num(fd_out, 'A', prev_cursor_row);
    (void)write_all(fd_out, "\r", 1u);

    for (int r = 0; r < clear_rows; r++) {
        ansi_clear_line(fd_out);
        if (r + 1 < clear_rows) {
            ansi_write_csi_num(fd_out, 'B', 1);
            (void)write_all(fd_out, "\r", 1u);
        }
    }

    ansi_write_csi_num(fd_out, 'A', clear_rows - 1);
    (void)write_all(fd_out, "\r", 1u);

    (void)write_all(fd_out, prompt, (uint32_t)prompt_len);
    if (line_len) {
        (void)write_all(fd_out, line, (uint32_t)line_len);
    }

    const int end_abs = prompt_len + (int)line_len;
    const int end_row = end_abs / cols;
    ansi_write_csi_num(fd_out, 'A', end_row);
    (void)write_all(fd_out, "\r", 1u);
    ansi_write_csi_num(fd_out, 'B', cursor_row);
    ansi_write_csi_num(fd_out, 'C', cursor_col);

    *io_prev_rows = rows;
    *io_prev_cursor_row = cursor_row;
}

static int read_line_editor(int fd_in, int fd_out, const char* prompt, int prompt_len, ush_history_t* hist, char** out_line) {
    if (!prompt || prompt_len < 0 || !hist || !out_line) return -1;
    *out_line = 0;

    const int ansi = term_is_ansi(fd_out);
    int cols = 80;
    int rows = 25;
    (void)term_get_size(fd_out, &cols, &rows);

    char* line = 0;
    size_t cap = 0;
    size_t len = 0;
    size_t cursor = 0;

    int prev_rows = 1;
    int prev_cursor_row = 0;

    int hist_nav = -1;
    char* hist_saved = 0;

    if (!line_ensure_cap(&line, &cap, 1)) return -1;
    line[0] = '\0';

    if (ansi) {
        (void)term_get_size(fd_out, &cols, &rows);
        ansi_redraw_line(fd_out, prompt, prompt_len, line, len, cursor, cols, &prev_rows, &prev_cursor_row);
    } else {
        (void)write_all(fd_out, prompt, (uint32_t)prompt_len);
    }

    for (;;) {
        ush_key_t k = read_key(fd_in, 100);
        if (k.kind == USH_KEY_ERROR) goto out_err;

        if (cursor > len) {
            cursor = len;
        }
        if (!line_ensure_cap(&line, &cap, len + 1)) {
            goto out_err;
        }
        line[len] = '\0';

        if (k.kind == USH_KEY_NONE) {
            if (ansi) {
                int new_cols = cols;
                int new_rows = rows;
                if (term_get_size(fd_out, &new_cols, &new_rows) && (new_cols != cols || new_rows != rows)) {
                    cols = new_cols;
                    rows = new_rows;
                    prev_rows = 1;
                    prev_cursor_row = 0;
                    ansi_redraw_line(fd_out, prompt, prompt_len, line, len, cursor, cols, &prev_rows, &prev_cursor_row);
                }
            }
            continue;
        }

        if (k.kind == USH_KEY_SCROLL_UP) {
            term_scroll(fd_out, 1);
            continue;
        }

        if (k.kind == USH_KEY_SCROLL_DOWN) {
            term_scroll(fd_out, -1);
            continue;
        }

        term_scroll_reset(fd_out);

        if (k.kind == USH_KEY_CTRL_C) {
            free(hist_saved);
            hist_saved = 0;
            hist_nav = -1;
            len = 0;
            cursor = 0;
            line[0] = '\0';

            (void)write_all(fd_out, "\n", 1u);
            if (ansi) {
                (void)term_get_size(fd_out, &cols, &rows);
                prev_rows = 1;
                prev_cursor_row = 0;
                ansi_redraw_line(fd_out, prompt, prompt_len, line, len, cursor, cols, &prev_rows, &prev_cursor_row);
            } else {
                (void)write_all(fd_out, prompt, (uint32_t)prompt_len);
            }
            continue;
        }

        if (k.kind == USH_KEY_ENTER) {
            (void)write_all(fd_out, "\n", 1u);
            free(hist_saved);
            hist_saved = 0;
            hist_nav = -1;

            if (!line_ensure_cap(&line, &cap, len + 1)) {
                goto out_err;
            }
            line[len] = '\0';
            *out_line = strdup(line);
            if (!*out_line) goto out_err;
            free(line);
            return (int)len;
        }

        int changed = 0;

        if (k.kind == USH_KEY_LEFT) {
            if (ansi && cursor > 0) {
                cursor--;
                changed = 1;
            }
        } else if (k.kind == USH_KEY_RIGHT) {
            if (ansi && cursor < len) {
                cursor++;
                changed = 1;
            }
        } else if (k.kind == USH_KEY_HOME) {
            if (ansi && cursor != 0) {
                cursor = 0;
                changed = 1;
            }
        } else if (k.kind == USH_KEY_END) {
            if (ansi && cursor != len) {
                cursor = len;
                changed = 1;
            }
        } else if (k.kind == USH_KEY_UP) {
            if (ansi && hist->count > 0) {
                if (hist_nav < 0) {
                    hist_saved = strdup(line);
                    hist_nav = hist->count - 1;
                } else if (hist_nav > 0) {
                    hist_nav--;
                }
                const char* src = (hist_nav >= 0 && hist_nav < hist->count) ? hist->lines[hist_nav] : "";
                size_t src_len = strlen(src);
                if (line_ensure_cap(&line, &cap, src_len + 1)) {
                    memcpy(line, src, src_len + 1);
                    len = src_len;
                    cursor = len;
                    changed = 1;
                }
            }
        } else if (k.kind == USH_KEY_DOWN) {
            if (ansi && hist_nav >= 0) {
                if (hist_nav < hist->count - 1) {
                    hist_nav++;
                    const char* src = hist->lines[hist_nav];
                    size_t src_len = strlen(src);
                    if (line_ensure_cap(&line, &cap, src_len + 1)) {
                        memcpy(line, src, src_len + 1);
                        len = src_len;
                        cursor = len;
                        changed = 1;
                    }
                } else {
                    hist_nav = -1;
                    const char* src = hist_saved ? hist_saved : "";
                    size_t src_len = strlen(src);
                    if (line_ensure_cap(&line, &cap, src_len + 1)) {
                        memcpy(line, src, src_len + 1);
                        len = src_len;
                        cursor = len;
                        changed = 1;
                    }
                    free(hist_saved);
                    hist_saved = 0;
                }
            }
        } else if (k.kind == USH_KEY_BACKSPACE) {
            if (cursor > 0) {
                memmove(&line[cursor - 1], &line[cursor], len - cursor);
                len--;
                cursor--;
                line[len] = '\0';
                changed = 1;

                if (!ansi) {
                    const char bs = '\b';
                    (void)write_all(fd_out, &bs, 1u);
                }
            }
        } else if (k.kind == USH_KEY_DELETE) {
            if (ansi && cursor < len) {
                memmove(&line[cursor], &line[cursor + 1], len - cursor - 1);
                len--;
                line[len] = '\0';
                changed = 1;
            }
        } else if (k.kind == USH_KEY_CHAR) {
            if (ansi) {
                if (line_ensure_cap(&line, &cap, len + 2)) {
                    memmove(&line[cursor + 1], &line[cursor], len - cursor);
                    line[cursor] = k.ch;
                    len++;
                    cursor++;
                    line[len] = '\0';
                    changed = 1;
                }
            } else {
                if (line_ensure_cap(&line, &cap, len + 2)) {
                    line[len++] = k.ch;
                    line[len] = '\0';
                    (void)write_all(fd_out, &k.ch, 1u);
                    cursor = len;
                }
            }
        }

        if (ansi && changed) {
            (void)term_get_size(fd_out, &cols, &rows);
            ansi_redraw_line(fd_out, prompt, prompt_len, line, len, cursor, cols, &prev_rows, &prev_cursor_row);
        } else if (!ansi && changed) {
            (void)0;
        }
    }

out_err:
    free(hist_saved);
    free(line);
    *out_line = 0;
    return -1;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    for (int fd = 3; fd < 64; fd++) {
        (void)close(fd);
    }

    set_term_mode(1);

    ush_history_t hist;
    hist_init(&hist);
    char prompt[512];

    for (;;) {
        int prompt_len = ush_make_prompt(prompt, (uint32_t)sizeof(prompt));
        if (prompt_len <= 0) prompt_len = 2;

        char* line = 0;
        int n = read_line_editor(0, 1, prompt, prompt_len, &hist, &line);
        if (n < 0) break;
        if (!line) continue;

        hist_add(&hist, line);

        ush_pipeline_t pl;
        char* perr = 0;
        if (ush_parse_line(line, &pl, &perr) != 0) {
            if (perr) {
                write_str(2, perr);
                free(perr);
            } else {
                write_str(2, "ush: parse error\n");
            }
            free(line);
            continue;
        }

        if (pl.count <= 0) {
            ush_pipeline_destroy(&pl);
            free(line);
            continue;
        }

        if (pl.count == 1 && pl.cmds[0].argc > 0 && pl.cmds[0].argv && pl.cmds[0].argv[0]) {
            ush_cmd_t* c = &pl.cmds[0];

            const int save0 = 60;
            const int save1 = 61;
            const int save2 = 62;
            int b_in = -1;
            int b_out = -1;
            int redir_ok = 1;
            if (c->in_path || c->out_path) {
                if (ush_save_stdio(save0, save1, save2) != 0) {
                    write_str(2, "ush: stdio save failed\n");
                    redir_ok = 0;
                } else {
                    if (ush_apply_single_redirs(c, save0, save1, save2, &b_in, &b_out) != 0) {
                        write_str(2, "ush: redirection failed\n");
                        redir_ok = 0;
                    }
                }
            }

            if (pl.background && is_builtin_cmd(c->argv[0])) {
                write_str(1, "ush: built-in cannot run in background\n");
                ush_pipeline_destroy(&pl);
                free(line);
                continue;
            }

            if (strcmp(c->argv[0], "exit") == 0) {
                if (c->in_path || c->out_path) {
                    ush_close_fd(&b_in);
                    ush_close_fd(&b_out);
                    ush_restore_stdio(save0, save1, save2);
                    (void)close(save0);
                    (void)close(save1);
                    (void)close(save2);
                }
                ush_pipeline_destroy(&pl);
                free(line);
                break;
            }

            if (strcmp(c->argv[0], "cd") == 0) {
                const char* path = (c->argc > 1) ? c->argv[1] : "/";
                if (redir_ok) {
                    if (chdir(path) != 0) {
                        write_str(1, "cd: failed\n");
                    }
                }
                if (c->in_path || c->out_path) {
                    ush_close_fd(&b_in);
                    ush_close_fd(&b_out);
                    ush_restore_stdio(save0, save1, save2);
                    (void)close(save0);
                    (void)close(save1);
                    (void)close(save2);
                }
                ush_pipeline_destroy(&pl);
                free(line);
                continue;
            }

            if (strcmp(c->argv[0], "pwd") == 0) {
                char cwd[256];
                int rn = getcwd(cwd, (uint32_t)sizeof(cwd));
                if (redir_ok) {
                    if (rn > 0) {
                        write_str(1, cwd);
                        write_str(1, "\n");
                    } else {
                        write_str(1, "pwd: failed\n");
                    }
                }
                if (c->in_path || c->out_path) {
                    ush_close_fd(&b_in);
                    ush_close_fd(&b_out);
                    ush_restore_stdio(save0, save1, save2);
                    (void)close(save0);
                    (void)close(save1);
                    (void)close(save2);
                }
                ush_pipeline_destroy(&pl);
                free(line);
                continue;
            }

            if (strcmp(c->argv[0], "clear") == 0) {
                if (redir_ok) {
                    write_str(1, "\x0C");
                }
                if (c->in_path || c->out_path) {
                    ush_close_fd(&b_in);
                    ush_close_fd(&b_out);
                    ush_restore_stdio(save0, save1, save2);
                    (void)close(save0);
                    (void)close(save1);
                    (void)close(save2);
                }
                ush_pipeline_destroy(&pl);
                free(line);
                continue;
            }

            if (c->in_path || c->out_path) {
                ush_close_fd(&b_in);
                ush_close_fd(&b_out);
                ush_restore_stdio(save0, save1, save2);
                (void)close(save0);
                (void)close(save1);
                (void)close(save2);
            }
        }

        (void)ush_exec_pipeline(&pl);
        ush_pipeline_destroy(&pl);
        free(line);
    }

    hist_destroy(&hist);

    return 0;
}
