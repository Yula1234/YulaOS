// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef KERNEL_SCHED_H
#define KERNEL_SCHED_H

#include "proc.h"

#ifdef __cplusplus
extern "C" {
#endif

void sched_init(void);
void sched_add(task_t* t);
void sched_start(task_t* first);
void sched_yield(void);
void sched_remove(task_t* t);

void sched_set_current(task_t* t);
void sem_remove_task(task_t* t);

uint32_t calc_weight(task_prio_t prio);
uint64_t calc_delta_vruntime(uint64_t delta_exec, uint32_t weight);

#ifdef __cplusplus
}
#endif

#endif