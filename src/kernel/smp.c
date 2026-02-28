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
#include <drivers/vga.h>
#include <drivers/fbdev.h>
#include <kernel/sched.h>
#include "cpu.h"

extern uint8_t smp_trampoline_start[];
extern uint8_t smp_trampoline_end[];

volatile int ap_running_count = 0;

static volatile uint32_t tlb_shootdown_lock = 0;
static volatile uint32_t tlb_shootdown_addr = 0;
static volatile uint32_t tlb_shootdown_pending = 0;

static inline int smp_interrupts_enabled(void) {
    uint32_t flags;
    __asm__ volatile("pushfl; popl %0" : "=r"(flags));
    return (flags & 0x200u) != 0u;
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

    if (fb_phys && fb_pitch && fb_height) {
        uint32_t fb_size = fb_pitch * fb_height;
        paging_init_mtrr_wc(fb_phys, fb_size);
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
    uint32_t addr = tlb_shootdown_addr;
    __asm__ volatile("invlpg (%0)" :: "r" (addr) : "memory");

    cpu_t* cpu = cpu_current();
    uint32_t bit = 1u << cpu->index;
    __sync_fetch_and_and(&tlb_shootdown_pending, ~bit);
}

void smp_tlb_shootdown(uint32_t virt) {
    if (cpu_count <= 1 || ap_running_count == 0) {
        __asm__ volatile("invlpg (%0)" :: "r" (virt) : "memory");
        return;
    }

    if (!smp_interrupts_enabled()) {
        __asm__ volatile("invlpg (%0)" :: "r" (virt) : "memory");
        return;
    }

    while (__sync_lock_test_and_set(&tlb_shootdown_lock, 1)) {
        __asm__ volatile("pause");
    }

    if (cpu_count <= 1 || ap_running_count == 0 || !smp_interrupts_enabled()) {
        __sync_lock_release(&tlb_shootdown_lock);
        __asm__ volatile("invlpg (%0)" :: "r" (virt) : "memory");
        return;
    }

    tlb_shootdown_addr = virt;

    cpu_t* me = cpu_current();
    uint32_t mask = 0;

    for (int i = 0; i < cpu_count; i++) {
        cpu_t* c = &cpus[i];
        if (!c->started) continue;
        if (c->id < 0) continue;
        if (c->id == me->id) continue;
        mask |= (1u << c->index);
    }

    tlb_shootdown_pending = mask;
    __sync_synchronize();

    for (int i = 0; i < cpu_count; i++) {
        cpu_t* c = &cpus[i];
        uint32_t bit = 1u << c->index;
        if (!(mask & bit)) continue;
        if (c->id < 0) continue;

        lapic_write(LAPIC_ICRHI, (uint32_t)c->id << 24);
        lapic_write(LAPIC_ICRLO, (uint32_t)IPI_TLB_VECTOR | 0x00004000);
    }

    __asm__ volatile("invlpg (%0)" :: "r" (virt) : "memory");

    while (tlb_shootdown_pending != 0) {
        __asm__ volatile("pause");
    }

    __sync_lock_release(&tlb_shootdown_lock);
}

static volatile uint32_t blit_lock = 0;

typedef struct {
    volatile uint32_t pending_mask;
    volatile uint32_t active_mask;
    uint32_t* page_dir;
    const void* src;
    uint32_t src_stride;
    int x;
    int y;
    int w;
    int h;
} blit_job_t;

static blit_job_t blit_job;

static uint8_t blit_fpu_state[MAX_CPUS][4096] __attribute__((aligned(64)));

__attribute__((always_inline))
static inline uint32_t popcount32(uint32_t v) {
    v = v - ((v >> 1) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    v = (v + (v >> 4)) & 0x0F0F0F0Fu;
    v = v + (v >> 8);
    v = v + (v >> 16);
    return v & 0x3Fu;
}

__attribute__((always_inline))
static inline void smp_blit_do_work_for_cpu(cpu_t* cpu) {
    if (!cpu) return;

    uint32_t bit = 1u << cpu->index;
    uint32_t active = blit_job.active_mask;
    if ((active & bit) == 0u) return;

    uint32_t total = popcount32(active);
    if (total == 0) return;

    uint32_t ordinal = popcount32(active & (bit - 1u));
    int per = (blit_job.h + (int)total - 1) / (int)total;
    if (per <= 0) return;

    int y1 = blit_job.y + (int)ordinal * per;
    int y2 = y1 + per;
    int y_end = blit_job.y + blit_job.h;
    if (y1 < blit_job.y) y1 = blit_job.y;
    if (y2 > y_end) y2 = y_end;
    if (y1 >= y2) return;

    vga_present_rect(blit_job.src, blit_job.src_stride, blit_job.x, y1, blit_job.w, y2 - y1);
}

void smp_blit_ipi_handler(void) {
    cpu_t* cpu = cpu_current();
    if (!cpu) return;

    uint32_t bit = 1u << cpu->index;
    if ((blit_job.active_mask & bit) == 0u) return;

    uint32_t* old_dir = paging_get_dir();
    if (blit_job.page_dir && old_dir != blit_job.page_dir) {
        paging_switch(blit_job.page_dir);
    }

    uint32_t fpu_sz = fpu_state_size();
    if (fpu_sz != 0 && fpu_sz <= 4096u) {
        uint8_t* fpu_tmp = &blit_fpu_state[cpu->index][0];
        fpu_save(fpu_tmp);

        smp_blit_do_work_for_cpu(cpu);

        fpu_restore(fpu_tmp);
    } else {
        smp_blit_do_work_for_cpu(cpu);
    }

    if (blit_job.page_dir && old_dir != blit_job.page_dir) {
        paging_switch(old_dir);
    }

    __sync_fetch_and_and(&blit_job.pending_mask, ~bit);
}

int smp_fb_present_rect(task_t* owner, const void* src, uint32_t src_stride, int x, int y, int w, int h) {
    if (!owner || !owner->mem || !owner->mem->page_dir) {
        vga_present_rect(src, src_stride, x, y, w, h);
        return 0;
    }

    if (cpu_count <= 1 || ap_running_count == 0) {
        vga_present_rect(src, src_stride, x, y, w, h);
        return 0;
    }

    if (h < 64) {
        vga_present_rect(src, src_stride, x, y, w, h);
        return 0;
    }

    while (__sync_lock_test_and_set(&blit_lock, 1)) {
        __asm__ volatile("pause");
    }

    if (cpu_count <= 1 || ap_running_count == 0) {
        __sync_lock_release(&blit_lock);
        vga_present_rect(src, src_stride, x, y, w, h);
        return 0;
    }

    cpu_t* me = cpu_current();
    if (!me) {
        __sync_lock_release(&blit_lock);
        vga_present_rect(src, src_stride, x, y, w, h);
        return 0;
    }

    uint32_t active_mask = 0;
    for (int i = 0; i < cpu_count; i++) {
        cpu_t* c = &cpus[i];
        if (!c->started) continue;
        if (c->id < 0) continue;
        active_mask |= (1u << c->index);
    }
    active_mask |= (1u << me->index);

    uint32_t other_mask = active_mask & ~(1u << me->index);
    if (other_mask == 0u) {
        __sync_lock_release(&blit_lock);
        vga_present_rect(src, src_stride, x, y, w, h);
        return 0;
    }

    blit_job.page_dir = owner->mem->page_dir;
    blit_job.src = src;
    blit_job.src_stride = src_stride;
    blit_job.x = x;
    blit_job.y = y;
    blit_job.w = w;
    blit_job.h = h;
    blit_job.active_mask = active_mask;
    blit_job.pending_mask = other_mask;
    __sync_synchronize();

    for (int i = 0; i < cpu_count; i++) {
        cpu_t* c = &cpus[i];
        uint32_t bit = 1u << c->index;
        if ((other_mask & bit) == 0u) continue;
        if (!c->started) continue;
        if (c->id < 0) continue;

        lapic_write(LAPIC_ICRHI, (uint32_t)c->id << 24);
        lapic_write(LAPIC_ICRLO, (uint32_t)IPI_BLIT_VECTOR | 0x00004000);
    }

    smp_blit_do_work_for_cpu(me);

    while (blit_job.pending_mask != 0u) {
        __asm__ volatile("pause");
    }

    __sync_lock_release(&blit_lock);
    return 0;
}
