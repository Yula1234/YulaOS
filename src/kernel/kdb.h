// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef KERNEL_KDB_H
#define KERNEL_KDB_H

#include "proc.h"

void kdb_enter(const char* reason, task_t* faulty_process);

#endif