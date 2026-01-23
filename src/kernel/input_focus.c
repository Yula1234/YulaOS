// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <kernel/input_focus.h>

static uint32_t g_input_focus_pid;

uint32_t input_focus_get_pid(void) {
    return __atomic_load_n(&g_input_focus_pid, __ATOMIC_RELAXED);
}

void input_focus_set_pid(uint32_t pid) {
    __atomic_store_n(&g_input_focus_pid, pid, __ATOMIC_RELEASE);
}

uint32_t input_focus_exchange_pid(uint32_t pid) {
    return __atomic_exchange_n(&g_input_focus_pid, pid, __ATOMIC_ACQ_REL);
}
