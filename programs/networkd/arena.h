#ifndef YOS_NETD_ARENA_H
#define YOS_NETD_ARENA_H

#include <stdint.h>
#include <stddef.h>

namespace netd {

class Arena {
public:
    struct Checkpoint {
        void* region;
        uint32_t used;
    };

    struct Stats {
        uint32_t region_count;
        uint32_t bytes_committed;
        uint32_t bytes_used;
        uint32_t peak_bytes_used;
    };

    Arena();

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    ~Arena();

    bool init(uint32_t initial_bytes);

    void* alloc(uint32_t size, uint32_t align);

    Checkpoint checkpoint() const;
    void rewind(Checkpoint cp);

    void reset();
    void release();

    Stats stats() const;

private:
    struct Region {
        int fd;
        uint8_t* base;
        uint32_t size;
        uint32_t used;
        Region* next;
    };

    static uint32_t align_up_u32(uint32_t v, uint32_t align);
    static uintptr_t align_up_ptr(uintptr_t v, uint32_t align);

    static uint32_t region_header_bytes();

    Region* ensure_region(uint32_t min_bytes);
    Region* alloc_region(uint32_t bytes);

    void update_peak();

    Region* m_head;
    Region* m_tail;

    uint32_t m_region_count;
    uint32_t m_bytes_committed;
    uint32_t m_bytes_used;
    uint32_t m_peak_bytes_used;
};

}

#endif
