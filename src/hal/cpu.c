// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <hal/cpu.h>

#include <kernel/smp/cpu.h>

#include <stdint.h>

int hal_cpu_index(void) {
    cpu_t* cpu = cpu_current();
    const int idx = cpu ? cpu->index : 0;

    if (idx < 0 || idx >= MAX_CPUS) {
        return 0;
    }

    return idx;
}
