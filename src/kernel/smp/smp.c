// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <stdint.h>
#include <lib/string.h>
#include <hal/apic.h>
#include <hal/io.h>
#include <hal/simd.h>
#include <hal/lock.h>
#include <mm/heap.h>
#include <mm/pmm.h>
#include <arch/i386/gdt.h>
#include <arch/i386/idt.h>
#include <arch/i386/paging.h>
#include <drivers/fbdev.h>
#include <kernel/sched.h>
#include "cpu.h"

extern uint8_t smp_trampoline_start[];
extern uint8_t smp_trampoline_end[];

volatile int ap_running_count = 0;

#define TLB_REQ_COUNT 16

typedef struct {
    uint32_t start;
    uint32_t end;
    volatile uint32_t pending_mask;
    volatile int in_use;
} tlb_shootdown_req_t;

static spinlock_t tlb_queue_lock = {0};
static tlb_shootdown_req_t tlb_reqs[TLB_REQ_COUNT];

static inline int smp_interrupts_enabled(void) {
    uint32_t flags;
    __asm__ volatile("pushfl; popl %0" : "=r"(flags));
    return (flags & 0x200u) != 0u;
}

void smp_panic_stop_other_cpus(void) {
    if (cpu_count <= 1 || ap_running_count == 0) {
        return;
    }

    cpu_t* me = cpu_current();
    const int me_id = me ? me->id : -1;

    for (int i = 0; i < cpu_count; i++) {
        cpu_t* c = &cpus[i];
        if (!c->started) {
            continue;
        }

        if (c->id < 0) {
            continue;
        }

        if (c->id == me_id) {
            continue;
        }

        lapic_write(LAPIC_ICRHI, (uint32_t)c->id << 24);
        lapic_write(LAPIC_ICRLO, 0x00004000u | 0x00000400u);
    }
}

static inline void tlb_flush_range_local(uint32_t start, uint32_t end) {
    if (end <= start) {
        return;
    }

    start &= ~0xFFFu;
    end = (end + 0xFFFu) & ~0xFFFu;

    const uint32_t pages = (end - start) >> 12;
    if (pages > 16u) {
        uint32_t cr3;
        __asm__ volatile(
            "mov %%cr3, %0\n\t"
            "mov %0, %%cr3"
            : "=r"(cr3)
            :
            : "memory"
        );

        return;
    }

    for (uint32_t addr = start; addr < end; addr += 0x1000u) {
        __asm__ volatile("invlpg (%0)" :: "r" (addr) : "memory");
    }
}

void smp_ap_main(cpu_t* cpu_arg) {
    cpu_t* cpu = cpu_arg; 
    cpu->started = 1;

    gdt_load();
    idt_load();
    
    int tss_selector = (5 + cpu->index) * 8;
    __asm__ volatile("ltr %%ax" : : "a" (tss_selector));
    
    paging_switch(kernel_page_directory);
    paging_init_pat();

    if (fb_ptr && fb_pitch && fb_height) {
        uint32_t fb_base = (uint32_t)fb_ptr;
        uint32_t fb_size = fb_pitch * fb_height;
        paging_init_mtrr_wc(fb_base, fb_size);
    }
    
    lapic_init();
    lapic_timer_init(KERNEL_TIMER_HZ);
    kernel_init_simd();

    __asm__ volatile("sti");
    
    __sync_fetch_and_add(&ap_running_count, 1);
    
    sched_yield();
}

static void mdulay(int ms) {
    for (volatile int i = 0; i < ms * 10000; i++);
}

void smp_boot_aps(void) {
    uint32_t size = smp_trampoline_end - smp_trampoline_start;
    if (size > 4096) return;
    
    memcpy((void*)0x1000, smp_trampoline_start, size);

    volatile uint32_t* tramp_stack = (volatile uint32_t*)(0x1000 + 4);
    volatile uint32_t* tramp_code  = (volatile uint32_t*)(0x1000 + 8);
    volatile uint32_t* tramp_cr3   = (volatile uint32_t*)(0x1000 + 12);
    volatile uint32_t* tramp_arg   = (volatile uint32_t*)(0x1000 + 16);

    *tramp_code = (uint32_t)smp_ap_main;
    *tramp_cr3  = (uint32_t)kernel_page_directory;

    cpu_t* bsp = cpu_current();

    for (int i = 0; i < cpu_count; i++) {
        if (cpus[i].id == bsp->id) continue;

        void* stack = kmalloc_a(4096);
        *tramp_stack = (uint32_t)stack + 4096;
        
        *tramp_arg = (uint32_t)&cpus[i];

        cpus[i].started = 0;

        lapic_write(LAPIC_ICRHI, cpus[i].id << 24);
        lapic_write(LAPIC_ICRLO, 0x00004500);
        mdulay(10);

        lapic_write(LAPIC_ICRHI, cpus[i].id << 24);
        lapic_write(LAPIC_ICRLO, 0x00004601);
        mdulay(1);

        lapic_write(LAPIC_ICRHI, cpus[i].id << 24);
        lapic_write(LAPIC_ICRLO, 0x00004601);
        mdulay(100);
    }
}

void smp_tlb_ipi_handler(void) {
    cpu_t* cpu = cpu_current();
    uint32_t my_bit = 1u << cpu->index;

    for (int i = 0; i < TLB_REQ_COUNT; i++) {
        tlb_shootdown_req_t* req = &tlb_reqs[i];

        if (!req->in_use) {
            continue;
        }

        uint32_t pending = __atomic_load_n(&req->pending_mask, __ATOMIC_ACQUIRE);
        if ((pending & my_bit) == 0u) {
            continue;
        }

        tlb_flush_range_local(req->start, req->end);

        __sync_fetch_and_and(&req->pending_mask, ~my_bit);
    }
}

static inline uint32_t smp_tlb_cpu_mask_for_page_dir(uint32_t* page_dir) {
    if (!page_dir) {
        return 0u;
    }

    cpu_t* me = cpu_current();
    const int me_id = me ? me->id : -1;

    uint32_t mask = 0u;

    for (int i = 0; i < cpu_count; i++) {
        cpu_t* c = &cpus[i];

        if (!c->started) {
            continue;
        }

        if (c->id < 0) {
            continue;
        }

        if (c->id == me_id) {
            continue;
        }

        task_t* t = c->current_task;
        proc_mem_t* mem = t ? t->mem : 0;
        uint32_t* dir = mem ? mem->page_dir : 0;

        if (dir != page_dir) {
            continue;
        }

        mask |= (1u << c->index);
    }

    return mask;
}

static int tlb_alloc_request(uint32_t start, uint32_t end, uint32_t mask) {
    uint32_t flags = spinlock_acquire_safe(&tlb_queue_lock);

    int idx = -1;
    for (int i = 0; i < TLB_REQ_COUNT; i++) {
        if (!tlb_reqs[i].in_use) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        spinlock_release_safe(&tlb_queue_lock, flags);
        return -1;
    }

    tlb_reqs[idx].start = start;
    tlb_reqs[idx].end = end;
    tlb_reqs[idx].pending_mask = mask;
    tlb_reqs[idx].in_use = 1;

    __sync_synchronize();

    spinlock_release_safe(&tlb_queue_lock, flags);

    return idx;
}

static void tlb_free_request(int idx) {
    if (idx < 0 || idx >= TLB_REQ_COUNT) {
        return;
    }

    uint32_t flags = spinlock_acquire_safe(&tlb_queue_lock);
    tlb_reqs[idx].in_use = 0;
    spinlock_release_safe(&tlb_queue_lock, flags);
}

static void tlb_send_ipi_to_mask(uint32_t mask) {
    for (int i = 0; i < cpu_count; i++) {
        cpu_t* c = &cpus[i];
        uint32_t bit = 1u << c->index;

        if ((mask & bit) == 0u) {
            continue;
        }

        if (c->id < 0) {
            continue;
        }

        lapic_write(LAPIC_ICRHI, (uint32_t)c->id << 24);
        lapic_write(LAPIC_ICRLO, (uint32_t)IPI_TLB_VECTOR | 0x00004000);
    }
}

void smp_tlb_shootdown_range_dir(uint32_t* page_dir, uint32_t start, uint32_t end) {
    if (end <= start) {
        return;
    }

    if (cpu_count <= 1 || ap_running_count == 0) {
        tlb_flush_range_local(start, end);
        return;
    }

    const uint32_t mask = smp_tlb_cpu_mask_for_page_dir(page_dir);
    if (mask == 0u) {
        tlb_flush_range_local(start, end);
        return;
    }

    int req_idx = tlb_alloc_request(start, end, mask);
    if (req_idx < 0) {
        tlb_flush_range_local(start, end);
        return;
    }

    tlb_send_ipi_to_mask(mask);

    tlb_flush_range_local(start, end);

    while (__atomic_load_n(&tlb_reqs[req_idx].pending_mask, __ATOMIC_ACQUIRE) != 0u) {
        __asm__ volatile("pause");
    }

    tlb_free_request(req_idx);
}

void smp_tlb_shootdown_range(uint32_t start, uint32_t end) {
    if (end <= start) {
        return;
    }

    if (cpu_count <= 1 || ap_running_count == 0) {
        tlb_flush_range_local(start, end);
        return;
    }

    cpu_t* me = cpu_current();
    uint32_t mask = 0;

    for (int i = 0; i < cpu_count; i++) {
        cpu_t* c = &cpus[i];
        if (!c->started) continue;
        if (c->id < 0) continue;
        if (c->id == me->id) continue;
        mask |= (1u << c->index);
    }

    if (mask == 0u) {
        tlb_flush_range_local(start, end);
        return;
    }

    int req_idx = tlb_alloc_request(start, end, mask);
    if (req_idx < 0) {
        tlb_flush_range_local(start, end);
        return;
    }

    tlb_send_ipi_to_mask(mask);

    tlb_flush_range_local(start, end);

    while (__atomic_load_n(&tlb_reqs[req_idx].pending_mask, __ATOMIC_ACQUIRE) != 0u) {
        __asm__ volatile("pause");
    }

    tlb_free_request(req_idx);
}

void smp_tlb_shootdown(uint32_t virt) {
    smp_tlb_shootdown_range(virt, virt + 0x1000u);
}
