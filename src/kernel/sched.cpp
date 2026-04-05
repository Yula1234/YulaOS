// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <arch/i386/context.h>
#include <arch/i386/paging.h>
#include <arch/i386/gdt.h>
#include <hal/lock.h>
#include <hal/io.h>
#include <hal/simd.h>
#include <lib/compiler.h>
#include <lib/rbtree.h>
#include <kernel/panic.h>

#include <lib/cpp/lock_guard.h>
 #include <lib/cpp/atomic.h>

#include "sched.h"
#include <kernel/smp/cpu.h>

extern volatile uint32_t timer_ticks;

namespace {

namespace sched_detail {

static constexpr uint32_t nice_0_load = 1024u;
static constexpr uint32_t min_granularity = 1u;
static constexpr uint32_t cpu_cache_invalidate_ticks = 100u;

static constexpr uint32_t best_cpu_runq_mul = 20u;
static constexpr uint32_t best_cpu0_penalty = 25u;

static constexpr uint32_t u32_max = 0xFFFFFFFFu;

static const uint32_t prio_to_weight[40] = {
    88761, 71755, 56483, 46273, 36291,
    29154, 23254, 18705, 14949, 11916,
    9548, 7620, 6100, 4904, 3906,
    3121, 2501, 1991, 1586, 1277,
    1024, 820, 655, 526, 423,
    335, 272, 215, 172, 137,
    110, 87, 70, 56, 45,
    36, 29, 23, 18, 15
};

static const uint32_t prio_to_inv_weight[40] = {
    48356, 59803, 76076, 92842, 118511,
    147493, 185184, 230082, 287896, 361611,
    452355, 566628, 708249, 881426, 1106992,
    1386425, 1732135, 2177249, 2735104, 3400427,
    4194304, 5235962, 6553609, 8153727, 10150640,
    12809091, 15762734, 19901434, 24903704, 31269198,
    38939098, 49275862, 61356675, 76695894, 95443717,
    119304647, 148295351, 186831435, 238609294, 286331153
};

}

}

static constexpr uint64_t cpu_cache_invalid = 0xFFFFFFFF00000000ull;
static __cacheline_aligned kernel::atomic<uint64_t> g_cpu_cache{cpu_cache_invalid};

___inline int prio_to_index(task_prio_t prio) {
    int nice = 10 - static_cast<int>(prio);
    if (nice < -20) nice = -20;
    if (nice > 19) nice = 19;
    return nice + 20;
}

___inline uint32_t calc_weight(task_prio_t prio) {
    return sched_detail::prio_to_weight[prio_to_index(prio)];
}

uint64_t calc_delta_vruntime(uint64_t delta_exec, task_prio_t prio) {
    int idx = prio_to_index(prio);
    uint32_t weight = sched_detail::prio_to_weight[idx];
    
    if (weight == 0) return delta_exec;
    
    uint64_t tmp = delta_exec * sched_detail::nice_0_load;
    return (tmp * sched_detail::prio_to_inv_weight[idx]) >> 32;
}

void sched_init(void) {
    g_cpu_cache.store(cpu_cache_invalid, kernel::memory_order::relaxed);
}

static int get_best_cpu(void) {
    uint32_t current_tick = timer_ticks;
    int active_cpus = 1 + ap_running_count;

    uint64_t cache_val = g_cpu_cache.load(kernel::memory_order::relaxed);
    int cached_cpu = static_cast<int>(cache_val >> 32);
    uint32_t cached_tick = static_cast<uint32_t>(cache_val & 0xFFFFFFFFu);

    if (
        cached_cpu >= 0 &&
        cached_cpu < active_cpus &&
        cached_tick != 0 &&
        (current_tick - cached_tick) < sched_detail::cpu_cache_invalidate_ticks
    ) {
        return cached_cpu;
    }

    int best_cpu = 0;
    uint32_t min_score = sched_detail::u32_max;

    cpu_t* me = cpu_current();
    int start_cpu = me ? me->index : 0;

    for (int ofs = 1; ofs <= active_cpus; ofs++) {
        int i = (start_cpu + ofs) % active_cpus;
        cpu_t* c = &cpus[i];
        
        uint32_t load = c->load_percent;
        uint32_t runq = c->runq_count;
        int weight = c->total_priority_weight;
        
        uint32_t score = load
            + (runq * sched_detail::best_cpu_runq_mul)
            + (weight > 0 ? weight : 0);

        if (i == 0 && active_cpus > 1) {
            score += sched_detail::best_cpu0_penalty;
        }

        if (score < min_score) {
            min_score = score;
            best_cpu = i;
        }
    }

    uint64_t new_cache = (static_cast<uint64_t>(static_cast<uint32_t>(best_cpu)) << 32) | current_tick;
    uint64_t expected = cache_val;
    g_cpu_cache.compare_exchange_weak(
        expected,
        new_cache,
        kernel::memory_order::relaxed,
        kernel::memory_order::relaxed
    );
    
    return best_cpu;
}

___inline void enqueue_task(cpu_t* cpu, task_t* p) {
    struct rb_node **link = &cpu->runq_root.rb_node;
    struct rb_node *parent = nullptr;
    struct task *entry;
    int leftmost = 1;

    while (*link) {
        parent = *link;
        entry = rb_entry(parent, struct task, rb_node);
        
        if (p->vruntime < entry->vruntime) {
            link = &parent->rb_left;
        } else {
            link = &parent->rb_right;
            leftmost = 0;
        }
    }

    rb_link_node(&p->rb_node, parent, link);
    rb_insert_color(&p->rb_node, &cpu->runq_root);
    
    if (leftmost)
        cpu->runq_leftmost = p;
        
    __atomic_fetch_add(&cpu->runq_count, 1, __ATOMIC_RELAXED);
}

___inline void dequeue_task(cpu_t* cpu, task_t* p) {
    if (cpu->runq_leftmost == p) {
        struct rb_node *next = rb_next(&p->rb_node);
        cpu->runq_leftmost = next ? rb_entry(next, struct task, rb_node) : nullptr;
    }

    rb_erase(&p->rb_node, &cpu->runq_root);

    __atomic_fetch_sub(&cpu->runq_count, 1, __ATOMIC_RELAXED);
}

void sched_add(task_t* t) {
    if (!t || t->pid == 0) {
        return;
    }

    if (t->assigned_cpu == -1) {
        t->assigned_cpu = get_best_cpu();
    }
    
    int target_cpu_idx = t->assigned_cpu;
    cpu_t* target = &cpus[target_cpu_idx];

    kernel::SpinLockNativeSafeGuard guard(target->lock);

    {
        kernel::SpinLockNativeGuard state_guard(t->state_lock);
        if (t->state == TASK_ZOMBIE || t->state == TASK_UNUSED) {
            return;
        }
    }

    if (t->is_queued) {
        return; 
    }

    t->is_queued = 1;

    uint32_t base_quantum;
    if (t->priority >= PRIO_GUI) {
        base_quantum = 8;
    } else if (t->priority >= PRIO_USER) {
        base_quantum = 4;
    } else {
        base_quantum = 2;
    }

    t->quantum = base_quantum;
    t->ticks_left = t->quantum;

    uint64_t min_vruntime;
    if (target->runq_leftmost) {
        min_vruntime = target->runq_leftmost->vruntime;
    } else {
        min_vruntime = static_cast<uint64_t>(target->sched_ticks) * sched_detail::nice_0_load;
    }

    if (t->vruntime == 0) {
        t->vruntime = min_vruntime;
    } else {
        const uint64_t max_sleep_bonus = static_cast<uint64_t>(sched_detail::nice_0_load) * 2ull;
        const uint64_t threshold = (min_vruntime > max_sleep_bonus) ? (min_vruntime - max_sleep_bonus) : 0ull;

        if (t->vruntime < threshold) {
            t->vruntime = threshold;
        }
    }
    
    t->exec_start = 0;
    
    target->total_priority_weight += t->priority;
    
    __atomic_fetch_add(&target->total_task_count, 1, __ATOMIC_RELAXED);

    enqueue_task(target, t);

    g_cpu_cache.store(cpu_cache_invalid, kernel::memory_order::relaxed);
}

___inline task_t* pick_next_cfs(cpu_t* cpu) {
    task_t* left = cpu->runq_leftmost;

    if (kernel::unlikely(!left)) {
        return nullptr;
    }
    
    dequeue_task(cpu, left);
    left->is_queued = 0;
    
    return left;
}

___inline bool sched_should_pin_task(const task_t* t) {
    return t && t->pid != 0u;
}

___inline void sched_task_pin(task_t* t) {
    if (!sched_should_pin_task(t)) {
        return;
    }

    (void)proc_task_retain(t);
}

___inline void sched_task_unpin(task_t* t) {
    if (!sched_should_pin_task(t)) {
        return;
    }

    proc_task_put(t);
}

extern "C" void sched_on_task_entry(void) {
    cpu_t* me = cpu_current();
    if (!me) {
        return;
    }

    __atomic_store_n(&me->in_kernel, 1u, __ATOMIC_RELEASE);
    rcu_qs_count_inc();

    task_t* switched_out = me->prev_task_during_switch;
    if (!switched_out) {
        return;
    }

    me->prev_task_during_switch = nullptr;
    sched_task_unpin(switched_out);
}

void sched_set_current(task_t* t) {
    cpu_t* cpu = cpu_current();

    rcu_qs_count_inc();

    task_t* old = cpu->current_task;
    if (old != t) {
        sched_task_pin(t);
        cpu->current_task = t;
        sched_task_unpin(old);
    }
    
    uint32_t kstack_top = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(t->kstack)) + t->kstack_size;
    kstack_top &= ~0xF; 
    
    tss_set_stack(cpu->index, kstack_top);

    uint32_t* target_dir = (t->mem && t->mem->page_dir) ? t->mem->page_dir : kernel_page_directory;
    if (paging_get_dir() != target_dir) {
        paging_switch(target_dir);
    }
}

void sched_start(task_t* first) {
    sched_set_current(first);
    (void)proc_change_state(first, TASK_RUNNING);
    fpu_set_ts();
    ctx_start(first->esp);
    for (;;) cpu_hlt();
}

void sched_yield(void) {
    cpu_t* me = cpu_current();
    task_t* prev = me->current_task;

    rcu_qs_count_inc();

    if (kernel::likely(prev && prev->state == TASK_RUNNING && prev->pid != 0)) {
        task_t* leftmost = me->runq_leftmost;

        if (kernel::unlikely(!leftmost)) {
            return;
        }

        if (kernel::likely(prev->vruntime <= leftmost->vruntime)) {
            return;
        }
    }

    uint32_t irq_flags;
    __asm__ volatile(
        "pushfl\n\t"
        "popl %0\n\t"
        "cli"
        : "=r"(irq_flags)
        :
        : "memory"
    );
    const bool irq_was_enabled = (irq_flags & 0x200u) != 0u;

    if (kernel::likely(prev && prev->state == TASK_RUNNING && prev != me->idle_task && prev->pid != 0)) {
        (void)proc_change_state(prev, TASK_RUNNABLE);

        if (prev->exec_start > 0) {
            uint64_t delta_exec = me->sched_ticks - prev->exec_start;
            if (delta_exec > 0) {
                uint64_t delta_vruntime = calc_delta_vruntime(delta_exec, prev->priority);
                prev->vruntime += delta_vruntime;
            }
        }
        prev->exec_start = 0;

        {
            kernel::SpinLockNativeSafeGuard guard(me->lock);

            prev->is_queued = 1;
            enqueue_task(me, prev);
        }
    }

    while (1) {
        task_t* next = nullptr;

        {
            kernel::SpinLockNativeSafeGuard guard(me->lock);

            next = pick_next_cfs(me);
        }

        if (kernel::unlikely(!next)) {
            next = me->idle_task;
        }

        if (kernel::unlikely(!next)) {
            me->current_task = nullptr;
            __asm__ volatile("sti; hlt" ::: "memory");
            continue;
        }

        if (kernel::likely(next->state != TASK_ZOMBIE)) {
            (void)proc_change_state(next, TASK_RUNNING);
        }

        if (kernel::unlikely(next == prev)) {
            if (next->pid == 0) {
                __asm__ volatile("sti; hlt" ::: "memory");
            }

            if (irq_was_enabled) {
                __asm__ volatile("sti" ::: "memory");
            }
            return;
        }

        if (kernel::unlikely(me->prev_task_during_switch != prev)) {
            sched_task_pin(prev);
            me->prev_task_during_switch = prev;
        }

        next->exec_start = me->sched_ticks;

        sched_set_current(next);

        fpu_set_ts();

        if (kernel::likely(prev)) {
            ctx_switch(&prev->esp, next->esp);

            __atomic_store_n(&me->in_kernel, 1u, __ATOMIC_RELEASE);

            if (me->prev_task_during_switch) {
                task_t* switched_out = me->prev_task_during_switch;
                me->prev_task_during_switch = nullptr;
                sched_task_unpin(switched_out);
            }

            __asm__ volatile("sti" ::: "memory");
            return;
        }

        me->prev_task_during_switch = nullptr;

        __asm__ volatile("sti" ::: "memory");

        ctx_start(next->esp);
        __builtin_unreachable();
    }
}
void sched_remove(task_t* t) {
    int cpu_idx = t->assigned_cpu;
    if (cpu_idx < 0 || cpu_idx >= MAX_CPUS) return;
    
    cpu_t* target = &cpus[cpu_idx];
    
    kernel::SpinLockNativeSafeGuard guard(target->lock);
    
    if (target->total_priority_weight >= static_cast<int>(t->priority))
        target->total_priority_weight -= t->priority;
    else 
        target->total_priority_weight = 0;

    if (target->total_task_count > 0) {
        __atomic_fetch_sub(&target->total_task_count, 1, __ATOMIC_RELAXED);
    }

    g_cpu_cache.store(cpu_cache_invalid, kernel::memory_order::relaxed);

    if (t->is_queued) {
        dequeue_task(target, t);
        t->is_queued = 0;
    }
}
