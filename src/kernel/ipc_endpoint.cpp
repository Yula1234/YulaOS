// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <kernel/ipc_endpoint.h>

#include <fs/vfs.h>
#include <fs/pipe.h>

#include <kernel/proc.h>
#include <kernel/poll_waitq.h>

#include <hal/lock.h>
#include <lib/dlist.h>
#include <lib/string.h>
#include <lib/hash_map.h>

#include <mm/heap.h>

static bool ipc_name_valid(const char* name) {
    if (!name) {
        return false;
    }
    const size_t nlen = strlen(name);
    return nlen > 0 && nlen <= IPC_NAME_MAX;
}

class IpcEndpoint;

struct IpcPendingConn {
    dlist_head_t node;
    IpcEndpoint* owner;
    uint32_t client_pid;
    vfs_node_t* c2s_r;
    vfs_node_t* s2c_w;
    spinlock_t lock;
    uint32_t refcount;
    uint32_t queued;

    void init(IpcEndpoint* ep, uint32_t pid, vfs_node_t* in_r, vfs_node_t* out_w) {
        dlist_init(&node);
        owner = ep;
        client_pid = pid;
        c2s_r = in_r;
        s2c_w = out_w;
        queued = 0u;
        refcount = 1u;
        spinlock_init(&lock);
    }

    void retain() {
        uint32_t flags = spinlock_acquire_safe(&lock);
        refcount++;
        spinlock_release_safe(&lock, flags);
    }

    void release();

    void discard_nodes() {
        if (c2s_r) {
            vfs_node_release(c2s_r);
        }
        if (s2c_w) {
            vfs_node_release(s2c_w);
        }
        c2s_r = nullptr;
        s2c_w = nullptr;
    }

    void mark_queued(bool value) {
        queued = value ? 1u : 0u;
    }

    bool is_queued() const {
        return queued != 0u;
    }
};

class IpcEndpoint {
public:
    void init(const char* n, vfs_node_t* node) {
        strlcpy(name, n, sizeof(name));
        spinlock_init(&lock);
        dlist_init(&pending_conns);
        poll_waitq_init(&poll_waitq);
        listen_node = node;
        refcount = 1u;
        closing = 0u;
    }

    const char* endpoint_name() const {
        return name;
    }

    bool retain() {
        uint32_t flags = spinlock_acquire_safe(&lock);
        if (closing != 0u) {
            spinlock_release_safe(&lock, flags);
            return false;
        }
        refcount++;
        spinlock_release_safe(&lock, flags);
        return true;
    }

    void release() {
        bool do_finalize = false;
        uint32_t flags = spinlock_acquire_safe(&lock);
        if (refcount > 0u) {
            refcount--;
        }
        if (closing != 0u && refcount == 0u) {
            do_finalize = true;
        }
        spinlock_release_safe(&lock, flags);

        if (do_finalize) {
            finalize();
        }
    }

    void shutdown() {
        dlist_head_t to_release;
        dlist_init(&to_release);

        uint32_t flags = spinlock_acquire_safe(&lock);
        closing = 1u;
        while (!dlist_empty(&pending_conns)) {
            IpcPendingConn* p = container_of(pending_conns.next, IpcPendingConn, node);
            dlist_del(&p->node);
            p->mark_queued(false);
            dlist_add_tail(&p->node, &to_release);
        }
        spinlock_release_safe(&lock, flags);

        poll_waitq_wake_all(&poll_waitq);

        while (!dlist_empty(&to_release)) {
            IpcPendingConn* p = container_of(to_release.next, IpcPendingConn, node);
            dlist_del(&p->node);
            p->release();
        }
    }

    bool enqueue_pending(IpcPendingConn* conn) {
        uint32_t flags = spinlock_acquire_safe(&lock);
        if (closing != 0u) {
            spinlock_release_safe(&lock, flags);
            return false;
        }
        conn->retain();
        conn->mark_queued(true);
        dlist_add_tail(&conn->node, &pending_conns);
        spinlock_release_safe(&lock, flags);
        poll_waitq_wake_all(&poll_waitq);
        return true;
    }

    bool remove_pending(IpcPendingConn* conn) {
        uint32_t flags = spinlock_acquire_safe(&lock);
        if (!conn->is_queued()) {
            spinlock_release_safe(&lock, flags);
            return false;
        }
        dlist_del(&conn->node);
        conn->mark_queued(false);
        spinlock_release_safe(&lock, flags);
        return true;
    }

    bool has_pending() {
        uint32_t flags = spinlock_acquire_safe(&lock);
        bool empty = dlist_empty(&pending_conns);
        spinlock_release_safe(&lock, flags);
        return !empty;
    }

    int register_waiter(poll_waiter_t* w, task_t* task) {
        return poll_waitq_register(&poll_waitq, w, task);
    }

    IpcPendingConn* pop_pending() {
        uint32_t flags = spinlock_acquire_safe(&lock);
        if (dlist_empty(&pending_conns)) {
            spinlock_release_safe(&lock, flags);
            return nullptr;
        }
        IpcPendingConn* p = container_of(pending_conns.next, IpcPendingConn, node);
        dlist_del(&p->node);
        p->mark_queued(false);
        spinlock_release_safe(&lock, flags);
        return p;
    }

private:
    void finalize() {
        poll_waitq_detach_all(&poll_waitq);
        kfree(this);
    }

    char name[IPC_NAME_MAX + 1u];
    spinlock_t lock;
    dlist_head_t pending_conns;
    poll_waitq_t poll_waitq;
    vfs_node_t* listen_node;
    uint32_t refcount;
    uint32_t closing;
};

void IpcPendingConn::release() {
    bool destroy = false;
    uint32_t flags = spinlock_acquire_safe(&lock);
    if (refcount > 0u) {
        refcount--;
    }
    destroy = (refcount == 0u);
    spinlock_release_safe(&lock, flags);

    if (!destroy) {
        return;
    }

    if (c2s_r) {
        vfs_node_release(c2s_r);
    }
    if (s2c_w) {
        vfs_node_release(s2c_w);
    }
    if (owner) {
        owner->release();
    }
    kfree(this);
}

struct IpcEndpointName {
    char data[IPC_NAME_MAX + 1u];

    IpcEndpointName() {
        data[0] = 0;
    }

    explicit IpcEndpointName(const char* s) {
        strlcpy(data, s ? s : "", sizeof(data));
    }

    bool operator==(const IpcEndpointName& other) const {
        return strcmp(data, other.data) == 0;
    }
};

class IpcEndpointRegistry {
public:
    bool add(const IpcEndpointName& name, IpcEndpoint* ep) {
        return endpoints.insert_unique(name, ep);
    }

    void remove(const IpcEndpointName& name) {
        endpoints.remove(name);
    }

    bool find_and_retain(const IpcEndpointName& name, IpcEndpoint*& out) {
        return endpoints.with_value(name, [&out](IpcEndpoint* ep) -> bool {
            if (!ep) {
                return false;
            }
            if (!ep->retain()) {
                return false;
            }
            out = ep;
            return true;
        });
    }

private:
    HashMap<IpcEndpointName, IpcEndpoint*, 128> endpoints;
};

template<>
uint32_t HashMap<IpcEndpointName, IpcEndpoint*, 128>::hash(const IpcEndpointName& key) {
    uint32_t h = 5381u;
    const char* s = key.data;
    while (*s) {
        h = ((h << 5) + h) + (uint32_t)(*s);
        s++;
    }
    return h;
}

static IpcEndpointRegistry g_endpoints;

extern "C" {

static int ipc_listen_close(vfs_node_t* node);

static vfs_ops_t ipc_listen_ops = {
    .read = 0,
    .write = 0,
    .open = 0,
    .close = ipc_listen_close,
    .ioctl = 0,
};

struct vfs_node* ipc_listen_create(const char* name) {
    vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!ipc_name_valid(name) || !node) {
        if (node) {
            kfree(node);
        }
        return nullptr;
    }

    IpcEndpoint* ep = (IpcEndpoint*)kmalloc(sizeof(IpcEndpoint));
    if (!ep) {
        kfree(node);
        return nullptr;
    }

    ep->init(name, node);

    memset(node, 0, sizeof(*node));
    strlcpy(node->name, "ipc_listen", sizeof(node->name));
    node->flags = VFS_FLAG_IPC_LISTEN;
    node->refs = 1;
    node->ops = &ipc_listen_ops;
    node->private_data = ep;

    if (!g_endpoints.add(IpcEndpointName(name), ep)) {
        kfree(node);
        ep->shutdown();
        ep->release();
        return nullptr;
    }

    return node;
}

static int ipc_listen_close(vfs_node_t* node) {
    if (!node) return -1;
    IpcEndpoint* ep = (IpcEndpoint*)node->private_data;
    if (!ep) {
        kfree(node);
        return 0;
    }

    g_endpoints.remove(IpcEndpointName(ep->endpoint_name()));

    ep->shutdown();
    ep->release();
    kfree(node);
    return 0;
}

int ipc_connect(const char* name,
                struct vfs_node** out_c2s_w,
                struct vfs_node** out_s2c_r,
                void** out_pending_handle) {
    
    if (out_c2s_w) *out_c2s_w = 0;
    if (out_s2c_r) *out_s2c_r = 0;
    if (out_pending_handle) *out_pending_handle = 0;

    if (!out_c2s_w || !out_s2c_r || !out_pending_handle) return -1;
    if (!ipc_name_valid(name)) return -1;

    IpcEndpoint* ep = nullptr;
    if (!g_endpoints.find_and_retain(IpcEndpointName(name), ep)) {
        return -1;
    }

    vfs_node_t* c2s_r = 0;
    vfs_node_t* c2s_w = 0;
    vfs_node_t* s2c_r = 0;
    vfs_node_t* s2c_w = 0;

    if (vfs_create_pipe(&c2s_r, &c2s_w) != 0) {
        ep->release();
        return -1;
    }
    if (vfs_create_pipe(&s2c_r, &s2c_w) != 0) {
        vfs_node_release(c2s_r);
        vfs_node_release(c2s_w);
        ep->release();
        return -1;
    }

    IpcPendingConn* p = (IpcPendingConn*)kmalloc(sizeof(IpcPendingConn));
    if (!p) {
        vfs_node_release(c2s_r);
        vfs_node_release(c2s_w);
        vfs_node_release(s2c_r);
        vfs_node_release(s2c_w);
        ep->release();
        return -1;
    }

    task_t* curr = proc_current();
    p->init(ep, curr ? curr->pid : 0u, c2s_r, s2c_w);
    if (!ep->enqueue_pending(p)) {
        p->release();
        return -1;
    }

    *out_c2s_w = c2s_w;
    *out_s2c_r = s2c_r;
    *out_pending_handle = (void*)p;

    return 0;
}

void ipc_connect_commit(void* pending_handle) {
    IpcPendingConn* p = (IpcPendingConn*)pending_handle;
    if (!p) return;
    p->release();
}

void ipc_connect_cancel(void* pending_handle) {
    IpcPendingConn* p = (IpcPendingConn*)pending_handle;
    if (!p) return;
    IpcEndpoint* ep = p->owner;
    if (ep && ep->remove_pending(p)) {
        p->release();
    }
    p->release();
}

int ipc_listen_poll_ready(struct vfs_node* listen_node) {
    if (!listen_node || (listen_node->flags & VFS_FLAG_IPC_LISTEN) == 0) return 0;
    IpcEndpoint* ep = (IpcEndpoint*)listen_node->private_data;
    if (!ep) return 0;
    return ep->has_pending();
}

int ipc_listen_poll_waitq_register(struct vfs_node* listen_node, poll_waiter_t* w, task_t* task) {
    if (!listen_node || !w || !task) return -1;
    if ((listen_node->flags & VFS_FLAG_IPC_LISTEN) == 0) return -1;
    IpcEndpoint* ep = (IpcEndpoint*)listen_node->private_data;
    if (!ep) return -1;
    return ep->register_waiter(w, task);
}

int ipc_accept(struct vfs_node* listen_node, struct vfs_node** out_c2s_r, struct vfs_node** out_s2c_w) {
    if (out_c2s_r) *out_c2s_r = 0;
    if (out_s2c_w) *out_s2c_w = 0;

    if (!listen_node || !out_c2s_r || !out_s2c_w) return -1;
    if ((listen_node->flags & VFS_FLAG_IPC_LISTEN) == 0) return -1;

    IpcEndpoint* ep = (IpcEndpoint*)listen_node->private_data;
    if (!ep) return -1;

    for (;;) {
        IpcPendingConn* p = ep->pop_pending();
        if (!p) return 0;

        bool ok = true;
        if (p->client_pid != 0) {
            task_t* t = proc_find_by_pid(p->client_pid);
            if (!t || t->state == TASK_ZOMBIE || t->state == TASK_UNUSED) {
                ok = false;
            }
        }

        if (!ok) {
            p->discard_nodes();
            p->release();
            continue;
        }

        *out_c2s_r = p->c2s_r;
        *out_s2c_w = p->s2c_w;

        p->c2s_r = nullptr;
        p->s2c_w = nullptr;
        p->release();

        return 1;
    }
}

}
