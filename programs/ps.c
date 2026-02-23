// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <yula.h>

static const char* state_name(uint32_t st) {
    switch (st) {
        case 0: return "UNUSED";
        case 1: return "RUNNABLE";
        case 2: return "RUNNING";
        case 3: return "STOPPED";
        case 4: return "ZOMBIE";
        case 5: return "WAITING";
        default: return "?";
    }
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    uint32_t cap = 64;
    yos_proc_info_t* list = 0;

    for (;;) {
        if (cap == 0 || cap > (0xFFFFFFFFu / (uint32_t)sizeof(*list))) {
            printf("ps: invalid capacity\n");
            if (list) free(list);
            return 1;
        }

        uint32_t bytes = cap * (uint32_t)sizeof(*list);
        yos_proc_info_t* next = (yos_proc_info_t*)realloc(list, bytes);
        if (!next) {
            if (list) free(list);
            printf("ps: out of memory\n");
            return 1;
        }
        list = next;

        int n = proc_list(list, cap);
        if (n < 0) {
            free(list);
            printf("ps: proc_list failed\n");
            return 1;
        }

        if ((uint32_t)n < cap) {
            printf(" PID   PPID   STATE     PRIO  PAGES  TERM  NAME\n");
            for (int i = 0; i < n; i++) {
                const yos_proc_info_t* p = &list[i];
                printf("%5u %6u %-9s %5u %6u %5u  %s\n",
                       p->pid,
                       p->parent_pid,
                       state_name(p->state),
                       p->priority,
                       p->mem_pages,
                       p->term_mode,
                       p->name);
            }
            free(list);
            return 0;
        }

        uint32_t next_cap = cap * 2u;
        if (next_cap <= cap) {
            free(list);
            printf("ps: too many processes\n");
            return 1;
        }
        cap = next_cap;
    }
}
