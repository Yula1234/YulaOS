/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <kernel/locking/spinlock.h>
#include <kernel/locking/mutex.h>
#include <kernel/locking/sem.h>

#include <kernel/workqueue.h>
#include <kernel/sched.h>
#include <kernel/panic.h>
#include <kernel/proc.h>

#include <kernel/waitq/waitqueue.h>

#include <lib/compiler.h>
#include <lib/ringbuf.h>
#include <lib/string.h>

#include <hal/apic.h>
#include <hal/irq.h>
#include <hal/cpu.h>

#include <mm/heap.h>

#include "ldisc.h"

#define LDISC_RX_CAP    4096u
#define LDISC_ECHO_CAP  4096u
#define LDISC_LINE_CAP  1024u
#define LDISC_MAX_LINES 128u

extern volatile uint32_t timer_ticks;

static workqueue_t* g_ldisc_wq = NULL;

struct ldisc {
    mutex_t read_mutex_;
    mutex_t write_mutex_;

    spinlock_t rx_lock_;
    spinlock_t echo_lock_;

    waitqueue_t read_waitq_;

    semaphore_t write_sem_;

    ldisc_config_t cfg_;

    ldisc_emit_fn_t   echo_emit_;
    void*             echo_ctx_;

    ldisc_signal_fn_t sig_emit_;
    void*             sig_ctx_;

    ringbuf_t rx_ring_;
    uint8_t   rx_buf_[LDISC_RX_CAP];

    uint8_t  line_buf_[LDISC_LINE_CAP];
    uint32_t line_len_;

    uint32_t canon_lens_[LDISC_MAX_LINES];
    uint32_t canon_head_;
    uint32_t canon_tail_;
    uint32_t canon_count_;

    work_struct_t echo_work_;

    ringbuf_t echo_ring_;
    uint8_t   echo_buf_[LDISC_ECHO_CAP];
};

static void detach_read_waiters_locked(ldisc_t* ld, dlist_head_t* out_list) {
    waitqueue_detach_all_locked(&ld->read_waitq_, out_list);
}

static void echo_worker_task(work_struct_t* work) {
    ldisc_t* ld = container_of(work, ldisc_t, echo_work_);
    uint8_t buf[128];

    for (;;) {
        uint32_t to_emit = 0u;

        {
            guard(spinlock_safe)(&ld->echo_lock_);

            to_emit = (uint32_t)ringbuf_read(&ld->echo_ring_, buf, sizeof(buf));
        }

        if (to_emit == 0u)
            break;

        if (ld->echo_emit_)
            ld->echo_emit_(buf, to_emit, ld->echo_ctx_);
    }
}

static void queue_echo_bytes_locked(ldisc_t* ld, const uint8_t* data, size_t len) {
    if (!ld->cfg_.echo_
        || !ld->echo_emit_
        || len == 0u)
        return;

    guard(spinlock_safe)(&ld->echo_lock_);

    (void)ringbuf_write(&ld->echo_ring_, data, len);

    if (g_ldisc_wq)
        queue_work(g_ldisc_wq, &ld->echo_work_);
}

___inline void queue_echo_char_locked(ldisc_t* ld, uint8_t c) {
    queue_echo_bytes_locked(ld, &c, 1u);
}

___inline void queue_echo_erase_locked(ldisc_t* ld) {
    const uint8_t seq[3] = { '\b', ' ', '\b' };

    queue_echo_bytes_locked(ld, seq, sizeof(seq));
}

___inline void queue_echo_signal_locked(ldisc_t* ld, int sig) {
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

    queue_echo_bytes_locked(ld, seq, n);
}

static bool try_isig_locked(ldisc_t* ld, uint8_t b) {
    if (!ld->cfg_.isig_
        || !ld->sig_emit_)
        return false;

    int sig = 0;

    if (b == ld->cfg_.vintr_) {
        sig = 2; 
    } else if (b == ld->cfg_.vquit_) {
        sig = 3; 
    } else if (b == ld->cfg_.vsusp_) {
        sig = 20; 
    }

    if (sig == 0)
        return false;

    queue_echo_signal_locked(ld, sig);

    ringbuf_clear(&ld->rx_ring_);

    ld->line_len_ = 0u;
    ld->canon_head_ = 0u;
    ld->canon_tail_ = 0u;
    ld->canon_count_ = 0u;

    ld->sig_emit_(sig, ld->sig_ctx_);

    return true;
}

static bool receive_byte_locked(ldisc_t* ld, uint8_t b) {
    if (ld->cfg_.igncr_ && b == '\r')
        return true;

    if (ld->cfg_.icrnl_ && b == '\r')
        b = '\n';
    else if (ld->cfg_.inlcr_ && b == '\n')
        b = '\r';

    if (try_isig_locked(ld, b))
        return true;

    if (!ld->cfg_.canonical_) {
        if (ringbuf_free_space(&ld->rx_ring_) == 0u)
            return false;

        (void)ringbuf_push(&ld->rx_ring_, b);

        queue_echo_char_locked(ld, b);
        return true;
    }

    if (b == 0x08u || b == 0x7Fu) {
        if (ld->line_len_ > 0u) {
            ld->line_len_--;

            queue_echo_erase_locked(ld);
        }

        return true;
    }

    if (b == ld->cfg_.veof_ || b == '\n') {
        uint32_t needed = ld->line_len_;
        
        if (b == '\n')
            needed++;

        if (ringbuf_free_space(&ld->rx_ring_) < needed)
            return false;

        if (ld->canon_count_ >= LDISC_MAX_LINES)
            return false;

        if (b == '\n')
            ld->line_buf_[ld->line_len_++] = b;

        (void)ringbuf_write(&ld->rx_ring_, ld->line_buf_, ld->line_len_);
        
        ld->canon_lens_[ld->canon_head_] = ld->line_len_;
        ld->canon_head_ = (ld->canon_head_ + 1u) % LDISC_MAX_LINES;
        ld->canon_count_++;

        ld->line_len_ = 0u;
        return true;
    }

    if (ld->line_len_ >= LDISC_LINE_CAP)
        return false;

    ld->line_buf_[ld->line_len_++] = b;

    queue_echo_char_locked(ld, b);
    
    return true;
}

ldisc_t* ldisc_create(void) {
    ldisc_t* ld = (ldisc_t*)kmalloc(sizeof(ldisc_t));

    if (unlikely(!ld))
        return NULL;

    memset(ld, 0, sizeof(*ld));

    mutex_init(&ld->read_mutex_);
    mutex_init(&ld->write_mutex_);

    spinlock_init(&ld->rx_lock_);
    spinlock_init(&ld->echo_lock_);

    waitqueue_init(&ld->read_waitq_, ld, TASK_BLOCK_SEM);

    sem_init(&ld->write_sem_, 0);

    init_work(&ld->echo_work_, echo_worker_task);

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

    ringbuf_init(&ld->rx_ring_, ld->rx_buf_, LDISC_RX_CAP);
    ringbuf_init(&ld->echo_ring_, ld->echo_buf_, LDISC_ECHO_CAP);

    return ld;
}

void ldisc_destroy(ldisc_t* ld) {
    if (unlikely(!ld))
        return;

    dlist_head_t local_waiters;

    {
        guard(spinlock_safe)(&ld->rx_lock_);

        detach_read_waiters_locked(ld, &local_waiters);
    }

    waitqueue_wake_detached_list(&local_waiters);

    flush_work(&ld->echo_work_);

    kfree(ld);
}

void ldisc_set_config(ldisc_t* ld, const ldisc_config_t* cfg) {
    if (unlikely(!ld
        || !cfg))
        return;

    guard(spinlock_safe)(&ld->rx_lock_);

    ld->cfg_ = *cfg;
}

void ldisc_get_config(const ldisc_t* ld, ldisc_config_t* out_cfg) {
    if (unlikely(!ld
        || !out_cfg))
        return;

    guard(spinlock_safe)((spinlock_t*)&ld->rx_lock_);

    *out_cfg = ld->cfg_;
}

void ldisc_set_callbacks(
    ldisc_t* ld, ldisc_emit_fn_t echo_emit,
    void* echo_ctx, ldisc_signal_fn_t sig_emit,
    void* sig_ctx
) {
    if (unlikely(!ld))
        return;

    guard(spinlock_safe)(&ld->rx_lock_);

    ld->echo_emit_ = echo_emit;
    ld->echo_ctx_  = echo_ctx;
    
    ld->sig_emit_  = sig_emit;
    ld->sig_ctx_   = sig_ctx;
}

size_t ldisc_receive(ldisc_t* ld, const uint8_t* data, size_t size) {
    if (unlikely(!ld
        || !data
        || size == 0u))
        return 0u;

    dlist_head_t local_waiters;
    size_t written = 0;

    dlist_init(&local_waiters);

    {
        guard(spinlock_safe)(&ld->rx_lock_);

        for (size_t i = 0u; i < size; i++) {
            if (!receive_byte_locked(ld, data[i]))
                break;

            written++;
        }

        detach_read_waiters_locked(ld, &local_waiters);
    }

    waitqueue_wake_detached_list(&local_waiters);

    return written;
}

void ldisc_wait_space(ldisc_t* ld) {
    sem_wait(&ld->write_sem_);
}

___inline bool is_interrupted(task_t* curr) {
    if (curr && curr->pending_signals != 0u)
        return true;

    return false;
}

static bool wait_for_data_locked(
    ldisc_t* ld, uint32_t deadline, uint32_t target_count,
    bool* interrupted, uint32_t* io_flags
) {
    task_t* curr = proc_current();

    for (;;) {
        bool ready = false;

        if (ld->cfg_.canonical_)
            ready = (ld->canon_count_ > 0u);
        else
            ready = (ringbuf_size(&ld->rx_ring_) >= target_count);

        if (ready)
            return true;

        if (is_interrupted(curr)) {
            *interrupted = true;

            return false;
        }

        if (deadline != 0xFFFFFFFFu && timer_ticks >= deadline)
            return false;

        if (curr)
            (void)waitqueue_wait_prepare_locked(&ld->read_waitq_, curr);

        spinlock_release_safe(&ld->rx_lock_, *io_flags);

        if (curr) {
            if (deadline != 0xFFFFFFFFu)
                proc_sleep_add(curr, deadline);
            else
                sched_yield();

        } else {
            cpu_relax();
        }

        *io_flags = spinlock_acquire_safe(&ld->rx_lock_);

        if (curr)
            waitqueue_wait_cancel_locked(&ld->read_waitq_, curr);
    }
}

size_t ldisc_read(ldisc_t* ld, void* out, size_t size) {
    if (unlikely(!ld
        || !out
        || size == 0u))
        return 0u;

    guard(mutex)(&ld->read_mutex_);

    uint8_t* dst = (uint8_t*)out;

    size_t n = 0u;

    bool intr = false;

    uint32_t flags = irq_save();

    guard(spinlock_safe)(&ld->rx_lock_);

    if (ld->cfg_.canonical_) {
        wait_for_data_locked(ld, 0xFFFFFFFFu, 1u, &intr, &flags);

        if (intr && ld->canon_count_ == 0u)
            return (size_t)-2; 

        if (ld->canon_count_ > 0u) {
            uint32_t line_len = ld->canon_lens_[ld->canon_tail_];
            uint32_t to_copy = (size < line_len) ? (uint32_t)size : line_len;

            (void)ringbuf_read(&ld->rx_ring_, dst, to_copy);

            sem_signal_all(&ld->write_sem_);

            n += to_copy;

            if (to_copy < line_len) {
                ld->canon_lens_[ld->canon_tail_] -= to_copy;
            } else {
                ld->canon_tail_ = (ld->canon_tail_ + 1u) % LDISC_MAX_LINES;

                ld->canon_count_--;
            }
        }

        return n;
    }
    
    const uint8_t vmin = ld->cfg_.vmin_;
    const uint8_t vtime = ld->cfg_.vtime_;

    if (vmin == 0u && vtime == 0u) {}
    else if (vmin > 0u && vtime == 0u) {
        wait_for_data_locked(ld, 0xFFFFFFFFu, vmin, &intr, &flags);
    } else if (vmin == 0u && vtime > 0u) {
        uint32_t deadline = timer_ticks + ((uint32_t)vtime * KERNEL_TIMER_HZ) / 10u;

        wait_for_data_locked(ld, deadline, 1u, &intr, &flags);
    } else {
        wait_for_data_locked(ld, 0xFFFFFFFFu, 1u, &intr, &flags);

        size_t rx_size = ringbuf_size(&ld->rx_ring_);

        if (!intr && rx_size > 0u) {
            while (rx_size < vmin) {
                uint32_t deadline = timer_ticks + ((uint32_t)vtime * KERNEL_TIMER_HZ) / 10u;
                
                if (!wait_for_data_locked(ld, deadline, (uint32_t)rx_size + 1u, &intr, &flags)) {
                    break;
                }

                rx_size = ringbuf_size(&ld->rx_ring_);
            }
        }
    }

    if (intr && ringbuf_is_empty(&ld->rx_ring_))
        return (size_t)-2;

    uint32_t rx_count = (uint32_t)ringbuf_size(&ld->rx_ring_);
    uint32_t to_copy = (size < rx_count) ? (uint32_t)size : rx_count;

    (void)ringbuf_read(&ld->rx_ring_, dst, to_copy);

    n += to_copy;

    sem_signal_all(&ld->write_sem_);

    return n;
}

size_t ldisc_write_transform(
    ldisc_t* ld, const void* in,
    size_t size, ldisc_emit_fn_t emit,
    void* ctx
) {
    if (unlikely(!ld
        || !in
        || size == 0u
        || !emit))
        return 0u;

    guard(mutex)(&ld->write_mutex_);

    bool opost, onlcr;

    {
        guard(spinlock_safe)(&ld->rx_lock_);

        opost = ld->cfg_.opost_;
        onlcr = ld->cfg_.onlcr_;
    }

    const uint8_t* src = (const uint8_t*)in;
    size_t produced = 0u;

    if (!opost
        || !onlcr)
        return emit(src, size, ctx);

    size_t start = 0u;

    for (size_t i = 0u; i < size; i++) {
        if (src[i] == '\n') {
            if (i > start)
                produced += emit(src + start, i - start, ctx);

            const uint8_t seq[2] = { '\r', '\n' };

            size_t w = emit(seq, sizeof(seq), ctx);
            
            produced += w;
            
            if (w != sizeof(seq))
                return produced;
            
            start = i + 1u;
        }
    }

    if (size > start)
        produced += emit(src + start, size - start, ctx);

    return produced;
}

bool ldisc_has_readable(const ldisc_t* ld) {
    if (unlikely(!ld))
        return false;
 
    guard(spinlock_safe)((spinlock_t*)&ld->rx_lock_);
 
    if (ld->cfg_.canonical_)
        return ld->canon_count_ > 0u;
 
    return !ringbuf_is_empty(&ld->rx_ring_);
}

void ldisc_bootinit_wq(void) {
    if(!g_ldisc_wq)
        g_ldisc_wq = create_workqueue("ldisc");
}