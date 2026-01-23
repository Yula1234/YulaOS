// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <kernel/ipc_endpoint.h>

#include <fs/vfs.h>
#include <fs/pipe.h>

#include <kernel/proc.h>

#include <hal/lock.h>
#include <lib/dlist.h>
#include <lib/string.h>

#include <mm/heap.h>

#define IPC_MAX_ENDPOINTS 16
#define IPC_NAME_MAX      31u

typedef struct ipc_pending_conn {
    dlist_head_t node;

    uint32_t client_pid;

    vfs_node_t* c2s_r;
    vfs_node_t* s2c_w;
} ipc_pending_conn_t;

typedef struct ipc_endpoint {
    int in_use;
    char name[32];

    spinlock_t lock;
    dlist_head_t pending;

    vfs_node_t* listen_node;
} ipc_endpoint_t;

static spinlock_t g_ipc_lock;
static int g_ipc_inited;
static ipc_endpoint_t g_ipc_eps[IPC_MAX_ENDPOINTS];

static void ipc_init_once(void) {
    if (__sync_bool_compare_and_swap(&g_ipc_inited, 0, 1)) {
        spinlock_init(&g_ipc_lock);
        for (int i = 0; i < IPC_MAX_ENDPOINTS; i++) {
            g_ipc_eps[i].in_use = 0;
            g_ipc_eps[i].listen_node = 0;
            spinlock_init(&g_ipc_eps[i].lock);
            dlist_init(&g_ipc_eps[i].pending);
            memset(g_ipc_eps[i].name, 0, sizeof(g_ipc_eps[i].name));
        }
    }
}

static inline void vfs_node_release(vfs_node_t* node) {
    if (!node) return;
    if (__sync_sub_and_fetch(&node->refs, 1) == 0) {
        if (node->ops && node->ops->close) {
            node->ops->close(node);
        } else {
            kfree(node);
        }
    }
}

static inline uint32_t ipc_name_len_bounded(const char* s) {
    if (!s) return 0;
    uint32_t n = 0;
    while (n < IPC_NAME_MAX && s[n]) n++;
    return n;
}

static ipc_endpoint_t* ipc_find_endpoint_locked(const char* name) {
    for (int i = 0; i < IPC_MAX_ENDPOINTS; i++) {
        if (!g_ipc_eps[i].in_use) continue;
        if (strcmp(g_ipc_eps[i].name, name) == 0) {
            return &g_ipc_eps[i];
        }
    }
    return 0;
}

static int ipc_listen_close(vfs_node_t* node) {
    if (!node) return -1;

    ipc_endpoint_t* ep = (ipc_endpoint_t*)node->private_data;
    if (!ep) {
        kfree(node);
        return 0;
    }

    uint32_t gl_flags = spinlock_acquire_safe(&g_ipc_lock);

    if (ep->in_use && ep->listen_node == node) {
        ep->listen_node = 0;
        ep->in_use = 0;
        memset(ep->name, 0, sizeof(ep->name));
    }

    spinlock_release_safe(&g_ipc_lock, gl_flags);

    uint32_t ep_flags = spinlock_acquire_safe(&ep->lock);
    while (!dlist_empty(&ep->pending)) {
        ipc_pending_conn_t* p = container_of(ep->pending.next, ipc_pending_conn_t, node);
        dlist_del(&p->node);

        vfs_node_t* c2s_r = p->c2s_r;
        vfs_node_t* s2c_w = p->s2c_w;
        p->c2s_r = 0;
        p->s2c_w = 0;

        spinlock_release_safe(&ep->lock, ep_flags);

        vfs_node_release(c2s_r);
        vfs_node_release(s2c_w);
        kfree(p);

        ep_flags = spinlock_acquire_safe(&ep->lock);
    }
    spinlock_release_safe(&ep->lock, ep_flags);

    kfree(node);
    return 0;
}

static vfs_ops_t ipc_listen_ops = {
    .read = 0,
    .write = 0,
    .open = 0,
    .close = ipc_listen_close,
};

struct vfs_node* ipc_listen_create(const char* name) {
    ipc_init_once();

    if (!name) return 0;

    uint32_t nlen = ipc_name_len_bounded(name);
    if (nlen == 0) return 0;
    if (name[nlen] != '\0') return 0;

    uint32_t gl_flags = spinlock_acquire_safe(&g_ipc_lock);

    ipc_endpoint_t* ep = ipc_find_endpoint_locked(name);
    if (ep) {
        if (ep->listen_node) {
            spinlock_release_safe(&g_ipc_lock, gl_flags);
            return 0;
        }
    } else {
        for (int i = 0; i < IPC_MAX_ENDPOINTS; i++) {
            if (!g_ipc_eps[i].in_use) {
                ep = &g_ipc_eps[i];
                ep->in_use = 1;
                strlcpy(ep->name, name, sizeof(ep->name));
                ep->listen_node = 0;
                dlist_init(&ep->pending);
                break;
            }
        }
    }

    if (!ep) {
        spinlock_release_safe(&g_ipc_lock, gl_flags);
        return 0;
    }

    vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!node) {
        ep->in_use = 0;
        memset(ep->name, 0, sizeof(ep->name));
        ep->listen_node = 0;
        dlist_init(&ep->pending);
        spinlock_release_safe(&g_ipc_lock, gl_flags);
        return 0;
    }

    memset(node, 0, sizeof(*node));
    strlcpy(node->name, "ipc_listen", sizeof(node->name));
    node->flags = VFS_FLAG_IPC_LISTEN;
    node->size = 0;
    node->inode_idx = 0;
    node->refs = 1;
    node->ops = &ipc_listen_ops;
    node->private_data = ep;

    ep->listen_node = node;

    spinlock_release_safe(&g_ipc_lock, gl_flags);
    return node;
}

int ipc_connect(const char* name,
                struct vfs_node** out_c2s_w,
                struct vfs_node** out_s2c_r,
                void** out_pending_handle) {
    ipc_init_once();

    if (out_c2s_w) *out_c2s_w = 0;
    if (out_s2c_r) *out_s2c_r = 0;
    if (out_pending_handle) *out_pending_handle = 0;

    if (!name || !out_c2s_w || !out_s2c_r || !out_pending_handle) return -1;

    uint32_t nlen = ipc_name_len_bounded(name);
    if (nlen == 0) return -1;
    if (name[nlen] != '\0') return -1;

    uint32_t gl_flags = spinlock_acquire_safe(&g_ipc_lock);
    ipc_endpoint_t* ep = ipc_find_endpoint_locked(name);
    if (!ep || !ep->listen_node) {
        spinlock_release_safe(&g_ipc_lock, gl_flags);
        return -1;
    }

    spinlock_release_safe(&g_ipc_lock, gl_flags);

    vfs_node_t* c2s_r = 0;
    vfs_node_t* c2s_w = 0;
    vfs_node_t* s2c_r = 0;
    vfs_node_t* s2c_w = 0;

    if (vfs_create_pipe(&c2s_r, &c2s_w) != 0) {
        return -1;
    }

    if (vfs_create_pipe(&s2c_r, &s2c_w) != 0) {
        vfs_node_release(c2s_r);
        vfs_node_release(c2s_w);
        return -1;
    }

    ipc_pending_conn_t* p = (ipc_pending_conn_t*)kmalloc(sizeof(*p));
    if (!p) {
        vfs_node_release(c2s_r);
        vfs_node_release(c2s_w);
        vfs_node_release(s2c_r);
        vfs_node_release(s2c_w);
        return -1;
    }

    memset(p, 0, sizeof(*p));
    dlist_init(&p->node);

    task_t* curr = proc_current();
    p->client_pid = curr ? curr->pid : 0;
    p->c2s_r = c2s_r;
    p->s2c_w = s2c_w;

    uint32_t ep_flags = spinlock_acquire_safe(&ep->lock);
    dlist_add_tail(&p->node, &ep->pending);
    spinlock_release_safe(&ep->lock, ep_flags);

    *out_c2s_w = c2s_w;
    *out_s2c_r = s2c_r;
    *out_pending_handle = (void*)p;

    return 0;
}

void ipc_connect_cancel(void* pending_handle) {
    ipc_init_once();

    ipc_pending_conn_t* p = (ipc_pending_conn_t*)pending_handle;
    if (!p) return;

    uint32_t gl_flags = spinlock_acquire_safe(&g_ipc_lock);

    for (int i = 0; i < IPC_MAX_ENDPOINTS; i++) {
        ipc_endpoint_t* ep = &g_ipc_eps[i];
        if (!ep->in_use) continue;

        uint32_t ep_flags = spinlock_acquire_safe(&ep->lock);

        ipc_pending_conn_t* pos;
        ipc_pending_conn_t* n;
        dlist_for_each_entry_safe(pos, n, &ep->pending, node) {
            if (pos == p) {
                dlist_del(&pos->node);
                spinlock_release_safe(&ep->lock, ep_flags);
                spinlock_release_safe(&g_ipc_lock, gl_flags);

                vfs_node_release(pos->c2s_r);
                vfs_node_release(pos->s2c_w);
                kfree(pos);
                return;
            }
        }

        spinlock_release_safe(&ep->lock, ep_flags);
    }

    spinlock_release_safe(&g_ipc_lock, gl_flags);
}

int ipc_accept(struct vfs_node* listen_node, struct vfs_node** out_c2s_r, struct vfs_node** out_s2c_w) {
    ipc_init_once();

    if (out_c2s_r) *out_c2s_r = 0;
    if (out_s2c_w) *out_s2c_w = 0;

    if (!listen_node || !out_c2s_r || !out_s2c_w) return -1;
    if ((listen_node->flags & VFS_FLAG_IPC_LISTEN) == 0) return -1;

    ipc_endpoint_t* ep = (ipc_endpoint_t*)listen_node->private_data;
    if (!ep) return -1;

    for (;;) {
        uint32_t ep_flags = spinlock_acquire_safe(&ep->lock);
        if (dlist_empty(&ep->pending)) {
            spinlock_release_safe(&ep->lock, ep_flags);
            return 0;
        }

        ipc_pending_conn_t* p = container_of(ep->pending.next, ipc_pending_conn_t, node);
        dlist_del(&p->node);
        spinlock_release_safe(&ep->lock, ep_flags);

        int ok = 1;
        if (p->client_pid != 0) {
            task_t* t = proc_find_by_pid(p->client_pid);
            if (!t || t->state == TASK_ZOMBIE || t->state == TASK_UNUSED) {
                ok = 0;
            }
        }

        if (!ok) {
            vfs_node_release(p->c2s_r);
            vfs_node_release(p->s2c_w);
            kfree(p);
            continue;
        }

        *out_c2s_r = p->c2s_r;
        *out_s2c_w = p->s2c_w;

        p->c2s_r = 0;
        p->s2c_w = 0;
        kfree(p);

        return 1;
    }
}
