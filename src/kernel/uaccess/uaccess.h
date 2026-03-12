// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#ifndef KERNEL_UACCESS_UACCESS_H
#define KERNEL_UACCESS_UACCESS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct task;

typedef struct task task_t;

int uaccess_check_user_buffer(task_t* task, const void* buf, uint32_t size);

int uaccess_check_user_buffer_present(task_t* task, const void* buf, uint32_t size);

int uaccess_check_user_buffer_writable_present(task_t* task, void* buf, uint32_t size);

int uaccess_ensure_user_buffer_writable_mappable(task_t* task, void* buf, uint32_t size);

int uaccess_user_range_mappable(task_t* task, uintptr_t start, uintptr_t end_excl);

void uaccess_prefault_user_read(const void* p, uint32_t len);

int uaccess_copy_user_str_bounded(task_t* task, char* dst, uint32_t dst_size, const char* user_src);

int uaccess_copy_from_user(void* dst, const void* user_src, uint32_t size);

int uaccess_copy_to_user(void* user_dst, const void* src, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif
