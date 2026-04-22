/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include "ldisc.h"

#include <kernel/sched.h>
#include <kernel/panic.h>
#include <kernel/proc.h>

#include <hal/lock.h>
#include <hal/irq.h>
#include <hal/cpu.h>
#include <hal/apic.h>

#include <mm/heap.h>

#include <lib/compiler.h>
#include <lib/string.h>
#include <lib/dlist.h>

#define LDISC_COOKED_CAP 4096u
#define LDISC_LINE_CAP   1024u

#define LDISC_FLAG_EOL   0x0100u
#define LDISC_FLAG_EOF   0x0200u

extern volatile uint32_t timer_ticks;

typedef struct ldisk_ring {
    uint16_t data_[LDISC_COOKED_CAP];
    size_t   head_;
    size_t   tail_;
    size_t   count_;
} ldisk_ring_t;

struct ldisc {
    spinlock_t   lock_;
    dlist_head_t read_waiters_;

    ldisc_config_t  cfg_;

    ldisk_ring_t    cooked_;

    uint8_t      line_[LDISC_LINE_CAP];
    size_t       line_len_;

    size_t       canon_lines_;

    ldisc_emit_fn_t   echo_emit_;
    void*             echo_ctx_;

    ldisc_signal_fn_t sig_emit_;
    void*             sig_ctx_;
};

static void ring_push(ldisk_ring_t* ring, uint16_t val) {
    if (unlikely(ring->count_ == LDISC_COOKED_CAP)) {
        return;
    }

    ring->data_[ring->head_] = val;
    ring->head_ = (ring->head_ + 1u) % LDISC_COOKED_CAP;
    ring->count_++;
}

static bool ring_pop(ldisk_ring_t* ring, uint16_t* out) {
    if (ring->count_ == 0u) {
        return false;
    }

    *out = ring->data_[ring->tail_];
    ring->tail_ = (ring->tail_ + 1u) % LDISC_COOKED_CAP;
    ring->count_--;

    return true;
}

static void ring_clear(ldisk_ring_t* ring) {
    ring->head_ = 0u;
    ring->tail_ = 0u;
    ring->count_ = 0u;
}

static void ldisc_wake_waiters(ldisc_t* ld) {
    while (!dlist_empty(&ld->read_waiters_)) {
        dlist_head_t* node = ld->read_waiters_.next;
        task_t* t = container_of(node, task_t, sem_node);

        __atomic_fetch_add(&t->in_transit, 1u, __ATOMIC_ACQUIRE);

        dlist_del(node);

        node->next = NULL;
        node->prev = NULL;

        t->blocked_on_sem = NULL;
        t->blocked_kind = TASK_BLOCK_NONE;

        if (likely(t->state != TASK_ZOMBIE 
            && t->state != TASK_UNUSED)) 
        {
            if (proc_change_state(t, TASK_RUNNABLE) == 0) {
                sched_add(t);
            }
        }

        __atomic_fetch_sub(&t->in_transit, 1u, __ATOMIC_RELEASE);
    }
}

ldisc_t* ldisc_create(void) {
    ldisc_t* ld = (ldisc_t*)kmalloc(sizeof(ldisc_t));

    if (unlikely(!ld)) {
        return NULL;
    }

    memset(ld, 0, sizeof(*ld));

    spinlock_init(&ld->lock_);
    dlist_init(&ld->read_waiters_);

    ld->cfg_.canonical_ = true;
    ld->cfg_.echo_      = true;
    ld->cfg_.onlcr_     = true;
    ld->cfg_.icrnl_     = true;
    ld->cfg_.opost_     = true;
    
    ld->cfg_.vmin_      = 1u;
    ld->cfg_.vintr_     = 0x03u;
    ld->cfg_.veof_      = 0x04u;
    ld->cfg_.vquit_     = 0x1Cu;
    ld->cfg_.vsusp_     = 0x1Au;

    return ld;
}

void ldisc_destroy(ldisc_t* ld) {
    if (unlikely(!ld)) {
        return;
    }

    ldisc_wake_waiters(ld);

    kfree(ld);
}

void ldisc_set_config(ldisc_t* ld, const ldisc_config_t* cfg) {
    if (unlikely(!ld || !cfg)) {
        return;
    }

    const uint32_t flags = spinlock_acquire_safe(&ld->lock_);

    ld->cfg_ = *cfg;

    spinlock_release_safe(&ld->lock_, flags);
}

void ldisc_get_config(const ldisc_t* ld, ldisc_config_t* out_cfg) {
    if (unlikely(!ld || !out_cfg)) {
        return;
    }

    const uint32_t flags = spinlock_acquire_safe((spinlock_t*)&ld->lock_);

    *out_cfg = ld->cfg_;

    spinlock_release_safe((spinlock_t*)&ld->lock_, flags);
}

void ldisc_set_callbacks(
    ldisc_t* ld, ldisc_emit_fn_t echo_emit,
    void* echo_ctx, ldisc_signal_fn_t sig_emit,
    void* sig_ctx
) {
    if (unlikely(!ld)) {
        return;
    }

    const uint32_t flags = spinlock_acquire_safe(&ld->lock_);

    ld->echo_emit_ = echo_emit;
    ld->echo_ctx_  = echo_ctx;
    
    ld->sig_emit_  = sig_emit;
    ld->sig_ctx_   = sig_ctx;

    spinlock_release_safe(&ld->lock_, flags);
}

static void echo_byte_locked(ldisc_t* ld, uint8_t b) {
    if (!ld->cfg_.echo_ || !ld->echo_emit_) {
        return;
    }

    ld->echo_emit_(&b, 1u, ld->echo_ctx_);
}

static void echo_erase_locked(ldisc_t* ld) {
    if (!ld->cfg_.echo_ || !ld->echo_emit_) {
        return;
    }

    const uint8_t seq[3] = { '\b', ' ', '\b' };

    ld->echo_emit_(seq, sizeof(seq), ld->echo_ctx_);
}

static void echo_signal_locked(ldisc_t* ld, int sig) {
    if (!ld->cfg_.echo_ || !ld->echo_emit_) {
        return;
    }

    uint8_t seq[4];
    size_t n = 0u;

    seq[n++] = '^';

    if (sig == 2) {
        seq[n++] = 'C';
    } else if (sig == 3) {
        seq[n++] = '\\';
    } else if (sig == 20) {
        seq[n++] = 'Z';
    } else {
        seq[n++] = '?';
    }

    seq[n++] = '\n';

    ld->echo_emit_(seq, n, ld->echo_ctx_);
}

static void flush_input_locked(ldisc_t* ld) {
    ring_clear(&ld->cooked_);
    
    ld->line_len_ = 0u;
    ld->canon_lines_ = 0u;
}

static bool try_isig_locked(ldisc_t* ld, uint8_t b) {
    if (!ld->cfg_.isig_ || !ld->sig_emit_) {
        return false;
    }

    int sig = 0;

    if (b == ld->cfg_.vintr_) {
        sig = 2; 
    } else if (b == ld->cfg_.vquit_) {
        sig = 3; 
    } else if (b == ld->cfg_.vsusp_) {
        sig = 20; 
    }

    if (sig == 0) {
        return false;
    }

    echo_signal_locked(ld, sig);
    flush_input_locked(ld);

    ld->sig_emit_(sig, ld->sig_ctx_);

    ldisc_wake_waiters(ld);

    return true;
}

static void receive_byte_locked(ldisc_t* ld, uint8_t b) {
    if (ld->cfg_.igncr_ && b == '\r') {
        return;
    }

    if (ld->cfg_.icrnl_ && b == '\r') {
        b = '\n';
    } else if (ld->cfg_.inlcr_ && b == '\n') {
        b = '\r';
    }

    if (try_isig_locked(ld, b)) {
        return;
    }

    if (!ld->cfg_.canonical_) {
        ring_push(&ld->cooked_, (uint16_t)b);
        echo_byte_locked(ld, b);

        ldisc_wake_waiters(ld);
        return;
    }

    if (b == 0x08u || b == 0x7Fu) {
        if (ld->line_len_ > 0u) {
            ld->line_len_--;
            echo_erase_locked(ld);
        }

        return;
    }

    if (b == ld->cfg_.veof_) {
        if (ld->line_len_ == 0u) {
            ring_push(&ld->cooked_, LDISC_FLAG_EOF);
        } else {
            for (size_t i = 0u; i < ld->line_len_ - 1u; i++) {
                ring_push(&ld->cooked_, (uint16_t)ld->line_[i]);
            }

            ring_push(&ld->cooked_, (uint16_t)ld->line_[ld->line_len_ - 1u] | LDISC_FLAG_EOL);
        }

        ld->line_len_ = 0u;
        ld->canon_lines_++;
        
        ldisc_wake_waiters(ld);
        return;
    }

    if (ld->line_len_ < LDISC_LINE_CAP) {
        ld->line_[ld->line_len_++] = b;
    }

    echo_byte_locked(ld, b);

    if (b == '\n') {
        for (size_t i = 0u; i < ld->line_len_ - 1u; i++) {
            ring_push(&ld->cooked_, (uint16_t)ld->line_[i]);
        }

        ring_push(&ld->cooked_, (uint16_t)ld->line_[ld->line_len_ - 1u] | LDISC_FLAG_EOL);

        ld->line_len_ = 0u;
        ld->canon_lines_++;

        ldisc_wake_waiters(ld);
    }
}

void ldisc_receive(ldisc_t* ld, const uint8_t* data, size_t size) {
    if (unlikely(!ld || !data || size == 0u)) {
        return;
    }

    const uint32_t flags = spinlock_acquire_safe(&ld->lock_);

    for (size_t i = 0u; i < size; i++) {
        receive_byte_locked(ld, data[i]);
    }

    spinlock_release_safe(&ld->lock_, flags);
}

static bool is_interrupted(task_t* curr) {
    if (curr && curr->pending_signals != 0u) {
        return true;
    }

    return false;
}

static bool wait_for_data(
    ldisc_t* ld,
    uint32_t deadline,
    uint32_t target_count,
    bool* interrupted,
    uint32_t* io_flags
) {
    task_t* curr = proc_current();

    for (;;) {
        bool ready = false;

        if (ld->cfg_.canonical_) {
            ready = (ld->canon_lines_ > 0u);
        } else {
            ready = (ld->cooked_.count_ >= target_count);
        }

        if (ready) {
            return true;
        }

        if (is_interrupted(curr)) {
            *interrupted = true;
            return false;
        }

        if (deadline != 0xFFFFFFFFu && timer_ticks >= deadline) {
            return false;
        }

        if (curr) {
            curr->blocked_on_sem = ld;
            curr->blocked_kind = TASK_BLOCK_SEM; 
            
            dlist_add_tail(&curr->sem_node, &ld->read_waiters_);

            if (unlikely(proc_change_state(curr, TASK_WAITING) != 0)) {
                dlist_del(&curr->sem_node);
                
                curr->sem_node.next = NULL;
                curr->sem_node.prev = NULL;
                
                curr->blocked_on_sem = NULL;
                curr->blocked_kind = TASK_BLOCK_NONE;
            }
        }

        spinlock_release_safe(&ld->lock_, *io_flags);

        if (curr) {
            if (deadline != 0xFFFFFFFFu) {
                proc_sleep_add(curr, deadline);
            } else {
                sched_yield();
            }
        } else {
            cpu_relax();
        }

        *io_flags = spinlock_acquire_safe(&ld->lock_);

        if (curr && curr->blocked_on_sem == ld) {
            if (curr->sem_node.next && curr->sem_node.prev) {
                dlist_del(&curr->sem_node);
                
                curr->sem_node.next = NULL;
                curr->sem_node.prev = NULL;
            }

            curr->blocked_on_sem = NULL;
            curr->blocked_kind = TASK_BLOCK_NONE;
        }
    }
}

size_t ldisc_read(ldisc_t* ld, void* out, size_t size) {
    if (unlikely(!ld || !out || size == 0u)) {
        return 0u;
    }

    uint8_t* dst = (uint8_t*)out;
    size_t n = 0u;
    bool intr = false;

    uint32_t flags = spinlock_acquire_safe(&ld->lock_);

    if (ld->cfg_.canonical_) {
        wait_for_data(ld, 0xFFFFFFFFu, 1u, &intr, &flags);

        if (intr && ld->canon_lines_ == 0u) {
            spinlock_release_safe(&ld->lock_, flags);
            return (size_t)-2; 
        }

        while (n < size) {
            uint16_t val;

            if (!ring_pop(&ld->cooked_, &val)) {
                break;
            }

            if (val & LDISC_FLAG_EOF) {
                ld->canon_lines_--;
                break;
            }

            dst[n++] = (uint8_t)(val & 0xFFu);

            if (val & LDISC_FLAG_EOL) {
                ld->canon_lines_--;
                break;
            }
        }

        spinlock_release_safe(&ld->lock_, flags);
        return n;
    }
    
    const uint8_t vmin = ld->cfg_.vmin_;
    const uint8_t vtime = ld->cfg_.vtime_;

    if (vmin == 0u && vtime == 0u) {}
    else if (vmin > 0u && vtime == 0u) {
        wait_for_data(ld, 0xFFFFFFFFu, vmin, &intr, &flags);
    } else if (vmin == 0u && vtime > 0u) {
        uint32_t deadline = timer_ticks + ((uint32_t)vtime * KERNEL_TIMER_HZ) / 10u;
        wait_for_data(ld, deadline, 1u, &intr, &flags);
    } else {
        wait_for_data(ld, 0xFFFFFFFFu, 1u, &intr, &flags);

        if (!intr && ld->cooked_.count_ > 0u) {
            while (ld->cooked_.count_ < vmin) {
                uint32_t deadline = timer_ticks + ((uint32_t)vtime * KERNEL_TIMER_HZ) / 10u;
                
                if (!wait_for_data(ld, deadline, ld->cooked_.count_ + 1u, &intr, &flags)) {
                    break;
                }
            }
        }
    }

    if (intr && ld->cooked_.count_ == 0u) {
        spinlock_release_safe(&ld->lock_, flags);
        return (size_t)-2;
    }

    while (n < size) {
        uint16_t val;

        if (!ring_pop(&ld->cooked_, &val)) {
            break;
        }

        if (val & LDISC_FLAG_EOF) {
            continue;
        }

        dst[n++] = (uint8_t)(val & 0xFFu);
    }

    spinlock_release_safe(&ld->lock_, flags);
    return n;
}

size_t ldisc_write_transform(
    ldisc_t* ld,
    const void* in,
    size_t size,
    ldisc_emit_fn_t emit,
    void* ctx
) {
    if (unlikely(!ld || !in || size == 0u || !emit)) {
        return 0u;
    }

    const uint8_t* src = (const uint8_t*)in;
    size_t produced = 0u;

    const uint32_t flags = spinlock_acquire_safe(&ld->lock_);

    for (size_t i = 0u; i < size; i++) {
        uint8_t b = src[i];

        if (ld->cfg_.opost_ && ld->cfg_.onlcr_ && b == '\n') {
            const uint8_t seq[2] = { '\r', '\n' };
            size_t w = emit(seq, sizeof(seq), ctx);
            
            produced += w;
            
            if (w != sizeof(seq)) {
                break;
            }
            
            continue;
        }

        size_t w = emit(&b, 1u, ctx);
        produced += w;

        if (w != 1u) {
            break;
        }
    }

    spinlock_release_safe(&ld->lock_, flags);
    
    return produced;
}

bool ldisc_has_readable(const ldisc_t* ld) {
    if (unlikely(!ld)) {
        return false;
    }

    const uint32_t flags = spinlock_acquire_safe((spinlock_t*)&ld->lock_);

    bool ready = false;

    if (ld->cfg_.canonical_) {
        ready = (ld->canon_lines_ > 0u);
    } else {
        ready = (ld->cooked_.count_ > 0u);
    }

    spinlock_release_safe((spinlock_t*)&ld->lock_, flags);

    return ready;
}