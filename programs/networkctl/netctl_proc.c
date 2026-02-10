// SPDX-License-Identifier: GPL-2.0

#include "netctl_proc.h"

const char* netctl_proc_state_name(uint32_t st) {
    switch (st) {
        case 0: return "UNUSED";
        case 1: return "RUNNABLE";
        case 2: return "RUNNING";
        case 3: return "ZOMBIE";
        case 4: return "WAITING";
        default: return "?";
    }
}

static const char* netctl_basename(const char* path) {
    if (!path) {
        return 0;
    }

    const char* base = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/') {
            base = p + 1;
        }
    }

    return base;
}

static int netctl_name_equals_strip_exe(const char* name, const char* want) {
    if (!name || !want) {
        return 0;
    }

    size_t name_len = strlen(name);
    if (name_len >= 4u && strcmp(name + (name_len - 4u), ".exe") == 0) {
        name_len -= 4u;
    }

    size_t want_len = strlen(want);
    if (want_len != name_len) {
        return 0;
    }

    return memcmp(name, want, want_len) == 0;
}

static int netctl_proc_name_matches(const char* proc_name, const char* want_name) {
    if (!proc_name || !want_name) {
        return 0;
    }

    if (netctl_name_equals_strip_exe(proc_name, want_name)) {
        return 1;
    }

    const char* base = netctl_basename(proc_name);
    if (base && netctl_name_equals_strip_exe(base, want_name)) {
        return 1;
    }

    return 0;
}

int netctl_find_process(const char* name, yos_proc_info_t* out) {
    if (!name || !*name || !out) {
        return 0;
    }

    uint32_t cap = 32;
    yos_proc_info_t* list = 0;

    for (;;) {
        if (cap == 0 || cap > (0xFFFFFFFFu / (uint32_t)sizeof(*list))) {
            if (list) {
                free(list);
            }
            return 0;
        }

        uint32_t bytes = cap * (uint32_t)sizeof(*list);
        yos_proc_info_t* next = (yos_proc_info_t*)realloc(list, bytes);
        if (!next) {
            if (list) {
                free(list);
            }
            return 0;
        }
        list = next;

        int n = proc_list(list, cap);
        if (n < 0) {
            free(list);
            return 0;
        }

        if ((uint32_t)n == cap) {
            uint32_t next_cap = cap * 2u;
            if (next_cap <= cap) {
                free(list);
                return 0;
            }
            cap = next_cap;
            continue;
        }

        for (int i = 0; i < n; i++) {
            if (netctl_proc_name_matches(list[i].name, name)) {
                *out = list[i];
                free(list);
                return 1;
            }
        }

        free(list);
        return 0;
    }
}

