#include "arena.h"

#include <yula.h>

namespace netd {

Arena::Arena()
    : m_head(nullptr),
      m_tail(nullptr),
      m_region_count(0),
      m_bytes_committed(0),
      m_bytes_used(0),
      m_peak_bytes_used(0) {
}

Arena::~Arena() {
    release();
}

uint32_t Arena::align_up_u32(uint32_t v, uint32_t align) {
    if (align <= 1u) {
        return v;
    }

    const uint32_t m = align - 1u;
    return (v + m) & ~m;
}

uint32_t Arena::region_header_bytes() {
    return align_up_u32((uint32_t)sizeof(Region), 16u);
}

uintptr_t Arena::align_up_ptr(uintptr_t v, uint32_t align) {
    if (align <= 1u) {
        return v;
    }

    const uintptr_t m = (uintptr_t)align - 1u;
    return (v + m) & ~m;
}

void Arena::update_peak() {
    if (m_bytes_used > m_peak_bytes_used) {
        m_peak_bytes_used = m_bytes_used;
    }
}

bool Arena::init(uint32_t initial_bytes) {
    release();

    if (initial_bytes == 0) {
        initial_bytes = 4096u;
    }

    Region* r = alloc_region(initial_bytes);
    if (!r) {
        return false;
    }

    m_head = r;
    m_tail = r;

    m_region_count = 1;
    m_bytes_committed = r->size;
    m_bytes_used = 0;
    m_peak_bytes_used = 0;

    return true;
}

Arena::Region* Arena::alloc_region(uint32_t bytes) {
    uint32_t size = bytes;

    const uint32_t page = 4096u;
    size = align_up_u32(size, page);

    const int shm_fd = shm_create(size);
    if (shm_fd < 0) {
        return nullptr;
    }

    void* p = mmap(shm_fd, size, MAP_SHARED);
    if (!p) {
        close(shm_fd);
        return nullptr;
    }

    Region* r = (Region*)p;
    r->fd = shm_fd;
    r->base = (uint8_t*)p;
    r->size = size;
    r->used = Arena::region_header_bytes();
    r->next = nullptr;

    return r;
}

Arena::Region* Arena::ensure_region(uint32_t min_bytes) {
    if (m_tail) {
        const uint32_t avail = m_tail->size - m_tail->used;
        if (avail >= min_bytes) {
            return m_tail;
        }
    }

    uint32_t next_size = 0;
    if (m_tail) {
        next_size = m_tail->size;
    }

    if (next_size < 4096u) {
        next_size = 4096u;
    }

    while (next_size < min_bytes) {
        const uint32_t doubled = next_size * 2u;
        if (doubled < next_size) {
            next_size = min_bytes;
            break;
        }
        next_size = doubled;
    }

    Region* r = alloc_region(next_size);
    if (!r) {
        return nullptr;
    }

    if (!m_head) {
        m_head = r;
        m_tail = r;
    } else {
        m_tail->next = r;
        m_tail = r;
    }

    m_region_count++;
    m_bytes_committed += r->size;
    return r;
}

void* Arena::alloc(uint32_t size, uint32_t align) {
    if (size == 0) {
        return nullptr;
    }

    if (align == 0) {
        align = 1u;
    }

    if ((align & (align - 1u)) != 0u) {
        return nullptr;
    }

    const uint32_t worst = size + align;
    Region* r = ensure_region(worst);
    if (!r) {
        return nullptr;
    }

    const uintptr_t base = (uintptr_t)r->base;
    const uintptr_t cur = base + (uintptr_t)r->used;
    const uintptr_t aligned = align_up_ptr(cur, align);

    const uint32_t pad = (uint32_t)(aligned - cur);
    const uint32_t needed = pad + size;

    if (r->used + needed > r->size) {
        r = ensure_region(needed);
        if (!r) {
            return nullptr;
        }

        const uintptr_t base2 = (uintptr_t)r->base;
        const uintptr_t cur2 = base2 + (uintptr_t)r->used;
        const uintptr_t aligned2 = align_up_ptr(cur2, align);

        const uint32_t pad2 = (uint32_t)(aligned2 - cur2);
        const uint32_t needed2 = pad2 + size;

        if (r->used + needed2 > r->size) {
            return nullptr;
        }

        r->used += needed2;
        m_bytes_used += needed2;
        update_peak();
        return (void*)aligned2;
    }

    r->used += needed;
    m_bytes_used += needed;
    update_peak();

    return (void*)aligned;
}

Arena::Checkpoint Arena::checkpoint() const {
    Checkpoint cp{};
    cp.region = m_tail;
    cp.used = m_tail ? m_tail->used : 0u;
    return cp;
}

void Arena::rewind(Checkpoint cp) {
    if (!cp.region) {
        reset();
        return;
    }

    Region* target = (Region*)cp.region;

    Region* cur = m_head;
    while (cur && cur != target) {
        cur = cur->next;
    }

    if (!cur) {
        return;
    }

    Region* to_free = target->next;
    target->next = nullptr;

    while (to_free) {
        Region* next = to_free->next;

        const uint32_t header = Arena::region_header_bytes();
        const uint32_t used = to_free->used;
        const uint32_t contributed = used > header ? (used - header) : 0u;
        if (contributed <= m_bytes_used) {
            m_bytes_used -= contributed;
        } else {
            m_bytes_used = 0;
        }

        (void)munmap(to_free->base, to_free->size);
        close(to_free->fd);

        m_region_count--;
        m_bytes_committed -= to_free->size;

        to_free = next;
    }

    m_tail = target;

    if (cp.used > target->used) {
        return;
    }

    const uint32_t old_used = target->used;
    target->used = cp.used;

    const uint32_t delta = old_used - cp.used;
    if (delta <= m_bytes_used) {
        m_bytes_used -= delta;
    } else {
        m_bytes_used = 0;
    }
}

void Arena::reset() {
    if (!m_head) {
        return;
    }

    Region* keep = m_head;
    Region* to_free = keep->next;

    keep->next = nullptr;
    keep->used = Arena::region_header_bytes();

    while (to_free) {
        Region* next = to_free->next;

        const uint32_t header = Arena::region_header_bytes();
        const uint32_t used = to_free->used;
        const uint32_t contributed = used > header ? (used - header) : 0u;
        if (contributed <= m_bytes_used) {
            m_bytes_used -= contributed;
        } else {
            m_bytes_used = 0;
        }

        (void)munmap(to_free->base, to_free->size);
        close(to_free->fd);

        m_region_count--;
        m_bytes_committed -= to_free->size;

        to_free = next;
    }

    m_tail = keep;
    m_bytes_used = 0;
}

void Arena::release() {
    Region* cur = m_head;
    while (cur) {
        Region* next = cur->next;

        (void)munmap(cur->base, cur->size);
        close(cur->fd);

        cur = next;
    }

    m_head = nullptr;
    m_tail = nullptr;

    m_region_count = 0;
    m_bytes_committed = 0;
    m_bytes_used = 0;
    m_peak_bytes_used = 0;
}

Arena::Stats Arena::stats() const {
    Stats s{};
    s.region_count = m_region_count;
    s.bytes_committed = m_bytes_committed;
    s.bytes_used = m_bytes_used;
    s.peak_bytes_used = m_peak_bytes_used;
    return s;
}

}
