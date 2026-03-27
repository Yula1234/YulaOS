// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <lib/idr.h>

#include <lib/hash_map.h>

#include <lib/cpp/lock_guard.h>
#include <lib/cpp/new.h>

#include <mm/heap.h>

namespace kernel {

class Idr {
public:
    Idr()
        : next_id_(1u) {
    }

    Idr(const Idr&) = delete;
    Idr& operator=(const Idr&) = delete;

    Idr(Idr&&) = delete;
    Idr& operator=(Idr&&) = delete;

    int alloc(void* ptr) {
        kernel::SpinLockGuard guard(lock_);

        const uint32_t start_id = next_id_;
        uint32_t id = start_id;

        for (;;) {
            const auto res = map_.insert_unique_ex(id, ptr);

            if (res == decltype(map_)::InsertUniqueResult::Inserted) {
                next_id_ = id + 1u;

                if (next_id_ == 0u) {
                    next_id_ = 1u;
                }

                return static_cast<int>(id);
            }

            if (res == decltype(map_)::InsertUniqueResult::OutOfMemory) {
                return -1;
            }

            id++;

            if (id == 0u) {
                id = 1u;
            }

            if (id == start_id) {
                return -1;
            }
        }
    }

    void* find(int id) {
        if (id <= 0) {
            return nullptr;
        }

        void* ptr = nullptr;

        const bool ok = map_.with_value_unlocked(
            static_cast<uint32_t>(id),
            [&ptr](void* v) -> bool {
                ptr = v;
                return true;
            }
        );

        if (!ok) {
            return nullptr;
        }

        return ptr;
    }

    void remove(int id) {
        if (id <= 0) {
            return;
        }

        (void)map_.remove(static_cast<uint32_t>(id));
    }

private:
    HashMap<uint32_t, void*, 64> map_;
    kernel::SpinLock lock_;
    
    uint32_t next_id_;
};

}

extern "C" void idr_init(idr_t* idr) {
    if (!idr) {
        return;
    }

    idr->opaque = new (kernel::nothrow) kernel::Idr();
}

extern "C" void idr_destroy(idr_t* idr) {
    if (!idr || !idr->opaque) {
        return;
    }

    delete static_cast<kernel::Idr*>(idr->opaque);
    idr->opaque = nullptr;
}

extern "C" int idr_alloc(idr_t* idr, void* ptr) {
    if (!idr || !idr->opaque) {
        return -1;
    }

    return static_cast<kernel::Idr*>(idr->opaque)->alloc(ptr);
}

extern "C" void* idr_find(idr_t* idr, int id) {
    if (!idr || !idr->opaque) {
        return nullptr;
    }

    return static_cast<kernel::Idr*>(idr->opaque)->find(id);
}

extern "C" void idr_remove(idr_t* idr, int id) {
    if (!idr || !idr->opaque) {
        return;
    }

    static_cast<kernel::Idr*>(idr->opaque)->remove(id);
}