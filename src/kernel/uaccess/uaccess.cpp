// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <kernel/uaccess/uaccess.h>

#include <arch/i386/paging.h>

#include <kernel/proc.h>

#include <mm/vma.h>

#include <stdint.h>

namespace {

__attribute__((no_instrument_function))
static int uaccess_memcpy_from_user_impl(void* dst, const void* user_src, uint32_t size) {
    if (size == 0u) {
        return 0;
    }

    if (!dst || !user_src) {
        return -1;
    }

    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)user_src;

    uint32_t n = size;

    __asm__ volatile goto(
        "1: cld; rep movsb\n"
        ".pushsection .uaccess_fixup, \"a\"\n"
        ".long 1b, %l[fixup]\n"
        ".popsection\n"
        : "+D"(d), "+S"(s), "+c"(n)
        :
        : "memory", "cc"
        : fixup
    );

    return 0;

fixup:
    return -1;
}

__attribute__((no_instrument_function))
static int uaccess_memcpy_to_user_impl(void* user_dst, const void* src, uint32_t size) {
    if (size == 0u) {
        return 0;
    }

    if (!user_dst || !src) {
        return -1;
    }

    uint8_t* d = (uint8_t*)user_dst;
    const uint8_t* s = (const uint8_t*)src;

    uint32_t n = size;

    __asm__ volatile goto(
        "1: cld; rep movsb\n"
        ".pushsection .uaccess_fixup, \"a\"\n"
        ".long 1b, %l[fixup]\n"
        ".popsection\n"
        : "+D"(d), "+S"(s), "+c"(n)
        :
        : "memory", "cc"
        : fixup
    );

    return 0;

fixup:
    return -1;
}

extern "C" int uaccess_copy_from_user(void* dst, const void* user_src, uint32_t size) {
    task_t* curr = proc_current();

    if (!uaccess_check_user_buffer(curr, user_src, size)) {
        return -1;
    }

    return uaccess_memcpy_from_user_impl(dst, user_src, size);
}

extern "C" int uaccess_copy_to_user(void* user_dst, const void* src, uint32_t size) {
    task_t* curr = proc_current();

    if (!uaccess_check_user_buffer(curr, user_dst, size)) {
        return -1;
    }

    return uaccess_memcpy_to_user_impl(user_dst, src, size);
}

static int check_user_range_basic(task_t* task, uintptr_t start, uintptr_t end_excl) {
    if (!task || !task->mem || !task->mem->page_dir) {
        return 0;
    }

    if (end_excl < start) {
        return 0;
    }

    if (start < 0x08000000u || end_excl > 0xC0000000u) {
        return 0;
    }

    return 1;
}

static int user_range_mappable(task_t* t, uintptr_t start, uintptr_t end_excl) {
    if (!t || !t->mem || !t->mem->page_dir) {
        return 0;
    }

    if (end_excl <= start) {
        return 0;
    }

    if (start < 0x08000000u || end_excl > 0xC0000000u) {
        return 0;
    }

    uintptr_t cur = start;
    while (cur < end_excl) {
        const uint32_t v = (uint32_t)cur;

        if (t->stack_bottom < t->stack_top
            && v >= t->stack_bottom
            && v < t->stack_top) {
            uintptr_t lim = (uintptr_t)t->stack_top;
            cur = (end_excl < lim) ? end_excl : lim;
            continue;
        }

        if (t->mem->heap_start < t->mem->prog_break
            && v >= t->mem->heap_start
            && v < t->mem->prog_break) {
            uintptr_t lim = (uintptr_t)t->mem->prog_break;
            cur = (end_excl < lim) ? end_excl : lim;
            continue;
        }

        vma_region_t* region = vma_find(t->mem, v);

        if (!region) {
            return 0;
        }

        uintptr_t lim = (uintptr_t)region->vaddr_end;
        cur = (end_excl < lim) ? end_excl : lim;
    }

    return 1;
}

static int check_user_buffer_present_impl(task_t* task, uintptr_t start, uintptr_t end_excl, int require_writable) {
    if (!task || !task->mem || !task->mem->page_dir) {
        return 0;
    }

    (void)require_writable;

    if (end_excl <= start) {
        return 1;
    }

    return check_user_range_basic(task, start, end_excl);
}

}

extern "C" int uaccess_check_user_buffer(task_t* task, const void* buf, uint32_t size) {
    if (!buf) {
        return 0;
    }

    if (size == 0u) {
        return 1;
    }

    const uintptr_t start = (uintptr_t)buf;
    const uintptr_t end_excl = start + (uintptr_t)size;

    if (end_excl < start) {
        return 0;
    }

    return check_user_range_basic(task, start, end_excl);
}

extern "C" void uaccess_prefault_user_read(const void* p, uint32_t len) {
    if (!p || len == 0u) {
        return;
    }

    const uintptr_t addr = (uintptr_t)p;
    const uintptr_t end = addr + (uintptr_t)len - 1u;

    if (end < addr) {
        return;
    }

    uint8_t tmp;
    for (uintptr_t cur = addr; cur <= end;) {
        if (uaccess_copy_from_user(&tmp, (const void*)cur, 1u) != 0) {
            return;
        }

        const uintptr_t next = (cur & ~(uintptr_t)0xFFFu) + (uintptr_t)0x1000u;
        if (next <= cur) {
            return;
        }

        cur = next;
    }
}

extern "C" int uaccess_check_user_buffer_present(task_t* task, const void* buf, uint32_t size) {
    if (!uaccess_check_user_buffer(task, buf, size)) {
        return 0;
    }

    if (size == 0u) {
        return 1;
    }

    const uintptr_t start = (uintptr_t)buf;
    const uintptr_t end_excl = start + (uintptr_t)size;

    return check_user_buffer_present_impl(task, start, end_excl, 0);
}

extern "C" int uaccess_check_user_buffer_writable_present(task_t* task, void* buf, uint32_t size) {
    if (!uaccess_check_user_buffer(task, buf, size)) {
        return 0;
    }

    if (size == 0u) {
        return 1;
    }

    const uintptr_t start = (uintptr_t)buf;
    const uintptr_t end_excl = start + (uintptr_t)size;

    return check_user_buffer_present_impl(task, start, end_excl, 1);
}

extern "C" int uaccess_ensure_user_buffer_writable_mappable(task_t* task, void* buf, uint32_t size) {
    if (!uaccess_check_user_buffer(task, buf, size)) {
        return 0;
    }

    if (size == 0u) {
        return 1;
    }

    const uintptr_t start = (uintptr_t)buf;
    const uintptr_t end_excl = start + (uintptr_t)size;

    if (end_excl < start) {
        return 0;
    }

    if (!user_range_mappable(task, start, end_excl)) {
        return 0;
    }

    uaccess_prefault_user_read((const void*)buf, size);

    return uaccess_check_user_buffer_writable_present(task, buf, size);
}

extern "C" int uaccess_user_range_mappable(task_t* task, uintptr_t start, uintptr_t end_excl) {
    return user_range_mappable(task, start, end_excl);
}

extern "C" int uaccess_copy_user_str_bounded(task_t* task, char* dst, uint32_t dst_size, const char* user_src) {
    if (!task || !dst || dst_size == 0u || !user_src) {
        return -1;
    }

    if (!uaccess_check_user_buffer(task, user_src, 1u)) {
        return -1;
    }

    for (uint32_t i = 0; i < dst_size; i++) {
        const void* p = (const void*)((uintptr_t)user_src + (uintptr_t)i);

        if (!uaccess_check_user_buffer(task, p, 1u)) {
            return -1;
        }

        char c = '\0';
        if (uaccess_copy_from_user(&c, p, 1u) != 0) {
            return -1;
        }
        dst[i] = c;

        if (c == '\0') {
            return 0;
        }
    }

    return -1;
}
