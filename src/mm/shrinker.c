/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <kernel/output/kprintf.h>
#include <kernel/locking/mutex.h>
#include <kernel/sched.h>
#include <kernel/proc.h>

#include <mm/shrinker.h>
#include <mm/heap.h>
#include <mm/pmm.h>


#include <hal/compiler.h>

__cacheline_aligned static mutex_t g_shrinker_mutex;

__cacheline_aligned static dlist_head_t g_shrinker_list;

void register_shrinker(shrinker_t* s) {
    if (!s || !s->reclaim) {
        return;
    }

    mutex_lock(&g_shrinker_mutex);

    dlist_add_tail(&s->list, &g_shrinker_list);
    
    mutex_unlock(&g_shrinker_mutex);
}

void unregister_shrinker(shrinker_t* s) {
    if (!s) {
        return;
    }

    mutex_lock(&g_shrinker_mutex);

    if (s->list.next && s->list.prev) {
        dlist_del(&s->list);
    
        s->list.next = 0;
        s->list.prev = 0;
    }
    
    mutex_unlock(&g_shrinker_mutex);
}

static void run_shrinkers(size_t target_pages) {
    if (target_pages == 0) {
        return;
    }

    mutex_lock(&g_shrinker_mutex);

    size_t freed = 0;
    shrinker_t* s;

    dlist_for_each_entry(s, &g_shrinker_list, list) {
        if (freed >= target_pages) {
            break;
        }

        size_t needed = target_pages - freed;
        size_t reclaimed = s->reclaim(needed, s->ctx);
        
        freed += reclaimed;
    }

    mutex_unlock(&g_shrinker_mutex);
}

static void mshrinker_task_func(void* arg) {
    (void)arg;

    while (1) {
        proc_usleep(1000000);

        uint32_t total = pmm_get_total_blocks();
        uint32_t free = pmm_get_free_blocks();

        if (total == 0) {
            continue;
        }

        if (free < (total / 10u)) {
            uint32_t target_free = (total * 15u) / 100u;
            uint32_t pages_to_free = target_free - free;
            
            kprintf("[mshrinker] Free: %u, Target: %u. Waking up shrinkers.\n", free, target_free);
            
            run_shrinkers(pages_to_free);

            kprintf("[mshrinker] Done.\n");
        }
    }
}

void shrinker_init(void) {
    mutex_init(&g_shrinker_mutex);

    dlist_init(&g_shrinker_list);

    proc_spawn_kthread("mshrinker", PRIO_LOW, mshrinker_task_func, 0);
}