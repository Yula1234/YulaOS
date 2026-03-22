/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

#include "yulafs.h"
#include "bcache.h"

#include "../drivers/block/bdev.h"
#include "../drivers/vga.h"

#include "../kernel/proc.h"
#include "../kernel/panic.h"
#include "../kernel/output/kprintf.h"
#include "../lib/string.h"
#include "../mm/heap.h"

#include <hal/lock.h>
#include <kernel/smp/cpu.h>
#include <lib/cpp/atomic.h>
#include <lib/cpp/mutex.h>
#include <lib/cpp/semaphore.h>
#include <lib/cpp/unique_ptr.h>
#include <lib/rbtree.h>

#include <lib/hash_map.h>

#define PTRS_PER_BLOCK (YFS_BLOCK_SIZE / 4)

/*
 * YFS keeps the on-disk format small.
 * Lock scopes are short; writeback points are explicit.
 */
namespace yfs {

struct DcacheKey {
    yfs_ino_t parent_ino;
    char name[YFS_NAME_MAX];

    bool operator==(const DcacheKey& other) const noexcept {
        if (parent_ino != other.parent_ino) {
            return false;
        }

        return strcmp(name, other.name) == 0;
    }
};

class FileSystem {
public:
    struct State {
        static constexpr int inode_table_cache_slots = 8;
        static constexpr int scratch_slots = 8;

        struct InodeRuntimeState {
            kernel::atomic<uint32_t> open_refs{0};
            kernel::atomic<uint32_t> runtime_flags{0};

            __cacheline_aligned rwlock_t lock{};
        };

        struct InodeTableCacheSlot {
            uint32_t lba;
            uint32_t stamp;
            kernel::atomic<uint32_t> flags;
            kernel::Semaphore io_done;
            uint8_t data[YFS_BLOCK_SIZE];
        };

        yfs_superblock_t sb;
        int mounted;

        uint32_t last_free_blk_hint;
        uint32_t last_free_ino_hint;

        uint8_t bmap_cache_data[YFS_BLOCK_SIZE];
        uint32_t bmap_cache_lba;
        int bmap_cache_dirty;

        kernel::Mutex alloc_lock;

        HashMap<DcacheKey, yfs_ino_t, 1024> dcache{};

        InodeTableCacheSlot inode_table_cache[MAX_CPUS][inode_table_cache_slots];
        spinlock_t inode_table_cache_lock[MAX_CPUS];
        uint32_t inode_table_cache_stamp[MAX_CPUS];

        uint8_t yfs_scratch[MAX_CPUS][scratch_slots][YFS_BLOCK_SIZE];
        uint8_t yfs_scratch_used[MAX_CPUS][scratch_slots];
        spinlock_t yfs_scratch_lock[MAX_CPUS];

        InodeRuntimeState* inode_state;
        uint32_t inode_state_count;
    };

    void init();
    void format(uint32_t disk_blocks_4k);

    int mkdir(const char* path);
    int create(const char* path);
    int unlink(const char* path);

    int read(yfs_ino_t ino, void* buf, yfs_off_t offset, uint32_t size);
    int write(yfs_ino_t ino, const void* buf, yfs_off_t offset, uint32_t size);
    int append(
        yfs_ino_t ino,
        const void* buf,
        uint32_t size,
        yfs_off_t* out_start_off
    );

    int lookup(const char* path);
    int lookup_in_dir(yfs_ino_t dir_ino, const char* name);

    int getdents(
        yfs_ino_t dir_ino,
        uint32_t* inout_offset,
        yfs_dirent_info_t* out,
        uint32_t out_size
    );

    int stat(yfs_ino_t ino, yfs_inode_t* out);

    void resize(yfs_ino_t ino, uint32_t new_size);

    void get_filesystem_info(uint32_t* total, uint32_t* free, uint32_t* size);
    int rename(const char* old_path, const char* new_path);

    uint8_t* scratch_acquire(int* out_slot);
    void scratch_release(int slot);

    rwlock_t* inode_lock(yfs_ino_t ino);

    void dcache_insert(yfs_ino_t parent, const char* name, yfs_ino_t target);
    yfs_ino_t dcache_lookup(yfs_ino_t parent, const char* name);
    void dcache_invalidate_entry(yfs_ino_t parent, const char* name);

    State& state() {
        return state_;
    }

    const State& state() const {
        return state_;
    }

private:
    State state_{};
};

static FileSystem g_fs;

}

namespace kernel {

template<>
struct HashTraits<yfs::DcacheKey> {
    static uint32_t hash(const yfs::DcacheKey& key) {
        uint32_t h = HashTraits<uint32_t>::hash(static_cast<uint32_t>(key.parent_ino));

        const uint8_t* p = reinterpret_cast<const uint8_t*>(key.name);
        for (; *p != 0u; ++p) {
            h ^= *p;
            h *= 16777619u;
        }

        return h;
    }
};

}

static inline yfs::FileSystem::State::InodeRuntimeState* inode_rt(yfs_ino_t ino);

/* Local aliases reduce noise at call sites. */
static inline rwlock_t* get_inode_lock(yfs_ino_t ino) {
    static rwlock_t fallback;
    static int fallback_inited = 0;

    if (!fallback_inited) {
        rwlock_init(&fallback);
        fallback_inited = 1;
    }

    auto* rt = inode_rt(ino);
    if (!rt) {
        return &fallback;
    }

    return &rt->lock;
}

static yfs::FileSystem::State& yfs_state = yfs::g_fs.state();

static yfs_superblock_t& sb = yfs_state.sb;
static int& fs_mounted = yfs_state.mounted;

static uint32_t& last_free_blk_hint = yfs_state.last_free_blk_hint;
static uint32_t& last_free_ino_hint = yfs_state.last_free_ino_hint;

static uint8_t (&bmap_cache_data)[YFS_BLOCK_SIZE] = yfs_state.bmap_cache_data;
static uint32_t& bmap_cache_lba = yfs_state.bmap_cache_lba;
static int& bmap_cache_dirty = yfs_state.bmap_cache_dirty;

static kernel::Mutex& alloc_lock = yfs_state.alloc_lock;

static yfs::FileSystem::State::InodeTableCacheSlot (&inode_table_cache)[
    MAX_CPUS
][
    yfs::FileSystem::State::inode_table_cache_slots
] = yfs_state.inode_table_cache;

static spinlock_t (&inode_table_cache_lock)[MAX_CPUS] = yfs_state.inode_table_cache_lock;
static uint32_t (&inode_table_cache_stamp)[MAX_CPUS] = yfs_state.inode_table_cache_stamp;

#define INODE_TABLE_CACHE_SLOTS (yfs::FileSystem::State::inode_table_cache_slots)

static constexpr uint32_t k_inode_cache_valid = 1u << 0;
static constexpr uint32_t k_inode_cache_inflight = 1u << 1;

struct KfreeDeleter {
    void operator()(uint8_t* ptr) const noexcept {
        if (ptr) {
            kfree(ptr);
        }
    }
};

#define YFS_SCRATCH_SLOTS (yfs::FileSystem::State::scratch_slots)

static uint8_t (&yfs_scratch_used)[
    MAX_CPUS
][
    yfs::FileSystem::State::scratch_slots
] = yfs_state.yfs_scratch_used;
static spinlock_t (&yfs_scratch_lock)[MAX_CPUS] = yfs_state.yfs_scratch_lock;

static yfs::FileSystem::State::InodeRuntimeState*& inode_state = yfs_state.inode_state;
static uint32_t& inode_state_count = yfs_state.inode_state_count;

static uint8_t* yfs_scratch_acquire(int* out_slot) {
    return yfs::g_fs.scratch_acquire(out_slot);
}

static void yfs_scratch_release(int slot) {
    yfs::g_fs.scratch_release(slot);
}

static void dcache_insert(yfs_ino_t parent, const char* name, yfs_ino_t target) {
    yfs::g_fs.dcache_insert(parent, name, target);
}

static yfs_ino_t dcache_lookup(yfs_ino_t parent, const char* name) {
    return yfs::g_fs.dcache_lookup(parent, name);
}

static void dcache_invalidate_entry(yfs_ino_t parent, const char* name) {
    yfs::g_fs.dcache_invalidate_entry(parent, name);
}

static inline void set_bit(uint8_t* map, int i) {
    map[i / 8] |= 1u << (i % 8);
}

static inline void clr_bit(uint8_t* map, int i) {
    map[i / 8] &= ~(1u << (i % 8));
}

static inline int chk_bit(uint8_t* map, int i) {
    return (map[i / 8] & (1u << (i % 8))) != 0u;
}

static inline yfs::FileSystem::State::InodeRuntimeState* inode_rt(yfs_ino_t ino) {
    if (ino == 0 || ino >= inode_state_count || !inode_state) {
        return nullptr;
    }

    return &inode_state[ino];
}

static void inode_state_init_or_panic(uint32_t total_inodes) {
    if (total_inodes == 0) {
        panic("yulafs: invalid inode_state size");
    }

    if (inode_state) {
        kfree(inode_state);
        inode_state = nullptr;
        inode_state_count = 0;
    }

    inode_state = static_cast<yfs::FileSystem::State::InodeRuntimeState*>(
        kmalloc(sizeof(*inode_state) * total_inodes)
    );
    if (!inode_state) {
        kprintf("yulafs: OOM allocating inode runtime state (%u inodes)\n", total_inodes);
        panic("yulafs: OOM allocating inode runtime state");
    }

    memset(inode_state, 0, sizeof(*inode_state) * total_inodes);

    for (uint32_t i = 0; i < total_inodes; i++) {
        rwlock_init(&inode_state[i].lock);
    }

    inode_state_count = total_inodes;
}

rwlock_t* yfs::FileSystem::inode_lock(yfs_ino_t ino) {
    return get_inode_lock(ino);
}

uint8_t* yfs::FileSystem::scratch_acquire(int* out_slot) {
    if (out_slot) {
        *out_slot = -1;
    }

    const int cpu = hal_cpu_index();
    if (cpu < 0 || cpu >= MAX_CPUS) {
        return nullptr;
    }

    auto try_acquire_from_cpu = [&](int src_cpu) -> uint8_t* {
        if (src_cpu < 0 || src_cpu >= MAX_CPUS) {
            return nullptr;
        }

        spinlock_acquire(&state_.yfs_scratch_lock[src_cpu]);

        for (int i = 0; i < State::scratch_slots; i++) {
            if (!state_.yfs_scratch_used[src_cpu][i]) {
                state_.yfs_scratch_used[src_cpu][i] = 1;
                spinlock_release(&state_.yfs_scratch_lock[src_cpu]);

                if (out_slot) {
                    *out_slot = (src_cpu << 16) | i;
                }

                return state_.yfs_scratch[src_cpu][i];
            }
        }

        spinlock_release(&state_.yfs_scratch_lock[src_cpu]);
        return nullptr;
    };

    if (uint8_t* buf = try_acquire_from_cpu(cpu)) {
        return buf;
    }

    for (int probe = 1; probe < MAX_CPUS; probe++) {
        const int other = (cpu + probe) % MAX_CPUS;

        if (!spinlock_try_acquire(&state_.yfs_scratch_lock[other])) {
            continue;
        }

        for (int i = 0; i < State::scratch_slots; i++) {
            if (!state_.yfs_scratch_used[other][i]) {
                state_.yfs_scratch_used[other][i] = 1;
                spinlock_release(&state_.yfs_scratch_lock[other]);

                if (out_slot) {
                    *out_slot = (other << 16) | i;
                }

                return state_.yfs_scratch[other][i];
            }
        }

        spinlock_release(&state_.yfs_scratch_lock[other]);
    }

    uint32_t backoff = 1;

    for (;;) {
        if (uint8_t* buf = try_acquire_from_cpu(cpu)) {
            return buf;
        }

        for (uint32_t i = 0; i < backoff; i++) {
            __asm__ volatile("pause" ::: "memory");
        }

        if (backoff < 1024u) {
            backoff <<= 1;
        }
    }
}

void yfs::FileSystem::scratch_release(int slot) {
    if (slot < 0) {
        return;
    }

    const int cpu = (slot >> 16) & 0xFFFF;
    const int idx = slot & 0xFFFF;

    if (cpu < 0 || cpu >= MAX_CPUS || idx < 0 || idx >= State::scratch_slots) {
        return;
    }

    spinlock_acquire(&state_.yfs_scratch_lock[cpu]);
    state_.yfs_scratch_used[cpu][idx] = 0;
    spinlock_release(&state_.yfs_scratch_lock[cpu]);
}

void yfs::FileSystem::dcache_insert(yfs_ino_t parent, const char* name, yfs_ino_t target) {
    /*
     * Only cache hits.
     * Skipping misses avoids extra invalidation rules on create/unlink.
     */
    if (!name || name[0] == '\0') {
        return;
    }

    DcacheKey key{};
    key.parent_ino = parent;
    strlcpy(key.name, name, sizeof(key.name));

    state_.dcache.insert_or_assign(key, target);
}

yfs_ino_t yfs::FileSystem::dcache_lookup(yfs_ino_t parent, const char* name) {
    /* 0 is a safe miss value (inode 0 is reserved). */
    if (!name || name[0] == '\0') {
        return 0;
    }

    DcacheKey key{};
    key.parent_ino = parent;
    strlcpy(key.name, name, sizeof(key.name));

    yfs_ino_t out = 0;
    return state_.dcache.try_get(key, out) ? out : 0;
}

void yfs::FileSystem::dcache_invalidate_entry(yfs_ino_t parent, const char* name) {
    if (!name || name[0] == '\0') {
        return;
    }

    DcacheKey key{};
    key.parent_ino = parent;
    strlcpy(key.name, name, sizeof(key.name));

    state_.dcache.remove(key);
}

static void flush_sb(void) {
    kernel::MutexGuard guard(alloc_lock);
    /* Superblock is tiny; rewrite is cheaper than partial updates. */
    bcache_write(1, (uint8_t*)&sb);
    bcache_flush_block(1);
}

static void flush_bitmap_cache_locked(void) {
    /* Single-block bitmap cache; writeback is explicit at call sites. */
    if (bmap_cache_lba != 0 && bmap_cache_dirty) {
        bcache_write(bmap_cache_lba, bmap_cache_data);
        bmap_cache_dirty = 0;
    }
}

static void flush_bitmap_cache(void) {
    kernel::MutexGuard guard(alloc_lock);
    flush_bitmap_cache_locked();
}

static void load_bitmap_block_locked(uint32_t lba) {
    /* Switching cache block is the writeback point for bitmap changes. */
    if (bmap_cache_lba == lba) {
        return;
    }

    flush_bitmap_cache_locked();

    bcache_read(lba, bmap_cache_data);

    bmap_cache_lba = lba;
    bmap_cache_dirty = 0;
}

[[maybe_unused]] static void load_bitmap_block(uint32_t lba) {
    kernel::MutexGuard guard(alloc_lock);
    load_bitmap_block_locked(lba);
}

static void zero_block(yfs_blk_t lba) {
    uint8_t* zeroes = (uint8_t*)kmalloc(YFS_BLOCK_SIZE);
    if (!zeroes) {
        return;
    }

    memset(zeroes, 0, YFS_BLOCK_SIZE);

    bcache_write(lba, zeroes);

    kfree(zeroes);
}

static int find_free_bit_in_block(uint8_t* buf, int start_bit) {
    uint64_t* ptr = (uint64_t*)buf;
    int words = YFS_BLOCK_SIZE / 8;
    int start_word = start_bit / 64;

    for (int i = start_word; i < words; i++) {
        if (ptr[i] != ~0ULL) {
            uint64_t val = ptr[i];
            for (int bit = 0; bit < 64; bit++) {
                int absolute_bit = i * 64 + bit;
                if (absolute_bit < start_bit) {
                    continue;
                }

                if (!((val >> bit) & 1u)) {
                    return absolute_bit;
                }
            }
        }
    }

    return -1;
}

static yfs_blk_t alloc_block(void) {
    kernel::MutexGuard guard(alloc_lock);

    /* 0 means allocation failure. */
    if (sb.free_blocks == 0) {
        return 0;
    }

    uint32_t total_map_blocks = (sb.total_blocks + (YFS_BLOCK_SIZE * 8) - 1) / (YFS_BLOCK_SIZE * 8);
    uint32_t start_sector = last_free_blk_hint / (YFS_BLOCK_SIZE * 8);
    uint32_t start_bit = last_free_blk_hint % (YFS_BLOCK_SIZE * 8);

    for (uint32_t i = 0; i < total_map_blocks; i++) {
        uint32_t sector_idx = (start_sector + i) % total_map_blocks;
        uint32_t map_lba = sb.map_block_start + sector_idx;

        load_bitmap_block_locked(map_lba);

        int bit_search_start = (i == 0) ? start_bit : 0;
        int found_bit = find_free_bit_in_block(bmap_cache_data, bit_search_start);

        if (found_bit != -1) {
            set_bit(bmap_cache_data, found_bit);
            bmap_cache_dirty = 1;
            sb.free_blocks--;

            uint32_t relative_idx = (sector_idx * YFS_BLOCK_SIZE * 8) + found_bit;
            last_free_blk_hint = relative_idx + 1;

            yfs_blk_t lba = sb.data_start + relative_idx;
            zero_block(lba);
            return lba;
        }
    }

    return 0;
}

static void free_block(yfs_blk_t lba) {
    kernel::MutexGuard guard(alloc_lock);

    if (lba < sb.data_start) {
        return;
    }

    uint32_t idx = lba - sb.data_start;
    uint32_t sector = idx / (YFS_BLOCK_SIZE * 8);
    uint32_t bit = idx % (YFS_BLOCK_SIZE * 8);
    uint32_t map_lba = sb.map_block_start + sector;

    load_bitmap_block_locked(map_lba);

    if (chk_bit(bmap_cache_data, bit)) {
        clr_bit(bmap_cache_data, bit);
        bmap_cache_dirty = 1;
        sb.free_blocks++;

        if (idx < last_free_blk_hint) {
            last_free_blk_hint = idx;
        }
    }
}

static yfs_ino_t alloc_inode(void) {
    uint8_t* buf = (uint8_t*)kmalloc(YFS_BLOCK_SIZE);
    if (!buf) {
        return 0;
    }

    kernel::MutexGuard guard(alloc_lock);

    /* Inode 0 is reserved; allocator never returns it. */
    if (sb.free_inodes == 0) {
        kfree(buf);
        return 0;
    }

    uint32_t total_map_blocks = (sb.total_inodes + (YFS_BLOCK_SIZE * 8) - 1) / (YFS_BLOCK_SIZE * 8);
    uint32_t start_sector = last_free_ino_hint / (YFS_BLOCK_SIZE * 8);
    uint32_t start_bit = last_free_ino_hint % (YFS_BLOCK_SIZE * 8);

    for (uint32_t i = 0; i < total_map_blocks; i++) {
        uint32_t sector_idx = (start_sector + i) % total_map_blocks;

        bcache_read(sb.map_inode_start + sector_idx, buf);

        int bit_search_start = (i == 0) ? start_bit : 0;
        int found_bit = find_free_bit_in_block(buf, bit_search_start);

        if (found_bit != -1) {
            uint32_t ino = (sector_idx * YFS_BLOCK_SIZE * 8) + found_bit;

            if (ino == 0) {
                continue;
            }

            if (ino >= sb.total_inodes) {
                break;
            }

            set_bit(buf, found_bit);
            bcache_write(sb.map_inode_start + sector_idx, buf);

            sb.free_inodes--;
            last_free_ino_hint = ino + 1;

            kfree(buf);
            return (yfs_ino_t)ino;
        }
    }

    kfree(buf);
    return 0;
}

static void free_inode(yfs_ino_t ino) {
    if (ino == 0 || ino >= sb.total_inodes) {
        return;
    }

    uint32_t sector = ino / (YFS_BLOCK_SIZE * 8);
    uint32_t bit = ino % (YFS_BLOCK_SIZE * 8);

    uint8_t* buf = (uint8_t*)kmalloc(YFS_BLOCK_SIZE);
    if (!buf) {
        return;
    }

    kernel::MutexGuard guard(alloc_lock);

    bcache_read(sb.map_inode_start + sector, buf);

    if (chk_bit(buf, bit)) {
        clr_bit(buf, bit);
        bcache_write(sb.map_inode_start + sector, buf);
        sb.free_inodes++;

        if (ino < last_free_ino_hint) {
            last_free_ino_hint = ino;
        }
    }

    kfree(buf);
}

static int sync_inode(yfs_ino_t ino, yfs_inode_t* data, int write) {
    /*
     * Small inode-table cache avoids hitting bcache on every lookup.
     * Spinlock keeps the cache slot stable across memcpy.
     */
    if (!data || ino == 0 || ino >= sb.total_inodes) {
        return 0;
    }

    if (sb.inode_table_start == 0) {
        return 0;
    }

    uint32_t per_block = YFS_BLOCK_SIZE / sizeof(yfs_inode_t);
    if (per_block == 0) {
        return 0;
    }

    uint32_t block_idx = ino / per_block;
    uint32_t offset = ino % per_block;

    if (offset >= per_block) {
        return 0;
    }

    if (block_idx > 0xFFFFFFFF - sb.inode_table_start) {
        return 0;
    }

    yfs_blk_t lba = sb.inode_table_start + block_idx;

    uint8_t* io_buf = static_cast<uint8_t*>(kmalloc(YFS_BLOCK_SIZE));
    if (!io_buf) {
        return 0;
    }

    auto io_buf_guard = kernel::unique_ptr<uint8_t, KfreeDeleter>(io_buf);

    const int cpu = hal_cpu_index();
    if (cpu < 0 || cpu >= MAX_CPUS) {
        return 0;
    }

    for (;;) {
        spinlock_acquire(&inode_table_cache_lock[cpu]);

        inode_table_cache_stamp[cpu]++;
        const uint32_t stamp = inode_table_cache_stamp[cpu];

        int slot = -1;
        for (int i = 0; i < INODE_TABLE_CACHE_SLOTS; i++) {
            const uint32_t flags = inode_table_cache[cpu][i].flags.load(kernel::memory_order::acquire);
            if ((flags & k_inode_cache_valid) != 0u && inode_table_cache[cpu][i].lba == lba) {
                slot = i;
                break;
            }
        }

        if (slot >= 0) {
            inode_table_cache[cpu][slot].stamp = stamp;

            const uint32_t flags = inode_table_cache[cpu][slot].flags.load(kernel::memory_order::acquire);
            if ((flags & k_inode_cache_inflight) != 0u) {
                spinlock_release(&inode_table_cache_lock[cpu]);
                inode_table_cache[cpu][slot].io_done.wait();
                continue;
            }

            if (offset * sizeof(yfs_inode_t) + sizeof(yfs_inode_t) > YFS_BLOCK_SIZE) {
                spinlock_release(&inode_table_cache_lock[cpu]);
                return 0;
            }

            if (!write) {
                const yfs_inode_t* table = (const yfs_inode_t*)inode_table_cache[cpu][slot].data;
                memcpy(data, &table[offset], sizeof(yfs_inode_t));
                spinlock_release(&inode_table_cache_lock[cpu]);
                return 1;
            }

            inode_table_cache[cpu][slot].flags.store(
                k_inode_cache_valid | k_inode_cache_inflight,
                kernel::memory_order::release
            );
            memcpy(io_buf_guard.get(), inode_table_cache[cpu][slot].data, YFS_BLOCK_SIZE);
            spinlock_release(&inode_table_cache_lock[cpu]);

            yfs_inode_t* table = (yfs_inode_t*)io_buf_guard.get();
            memcpy(&table[offset], data, sizeof(yfs_inode_t));

            const int ok = bcache_write(lba, io_buf_guard.get());

            spinlock_acquire(&inode_table_cache_lock[cpu]);

            if (inode_table_cache[cpu][slot].lba == lba) {
                if (ok) {
                    memcpy(inode_table_cache[cpu][slot].data, io_buf_guard.get(), YFS_BLOCK_SIZE);
                    inode_table_cache[cpu][slot].flags.store(k_inode_cache_valid, kernel::memory_order::release);
                } else {
                    inode_table_cache[cpu][slot].flags.store(0u, kernel::memory_order::release);
                }

                inode_table_cache[cpu][slot].io_done.signal_all();
            }

            spinlock_release(&inode_table_cache_lock[cpu]);

            return ok ? 1 : 0;
        }

        uint32_t best_stamp = 0xFFFFFFFFu;
        for (int i = 0; i < INODE_TABLE_CACHE_SLOTS; i++) {
            const uint32_t flags = inode_table_cache[cpu][i].flags.load(kernel::memory_order::acquire);
            if ((flags & k_inode_cache_inflight) != 0u) {
                continue;
            }

            if ((flags & k_inode_cache_valid) == 0u) {
                slot = i;
                break;
            }

            if (inode_table_cache[cpu][i].stamp < best_stamp) {
                best_stamp = inode_table_cache[cpu][i].stamp;
                slot = i;
            }
        }

        if (slot < 0) {
            inode_table_cache[cpu][0].flags.load(kernel::memory_order::acquire);
            spinlock_release(&inode_table_cache_lock[cpu]);
            inode_table_cache[cpu][0].io_done.wait();
            continue;
        }

        inode_table_cache[cpu][slot].lba = lba;
        inode_table_cache[cpu][slot].stamp = stamp;
        inode_table_cache[cpu][slot].flags.store(k_inode_cache_inflight, kernel::memory_order::release);
        inode_table_cache[cpu][slot].io_done.init(0);

        spinlock_release(&inode_table_cache_lock[cpu]);

        const int ok = bcache_read(lba, io_buf_guard.get());

        spinlock_acquire(&inode_table_cache_lock[cpu]);

        if (inode_table_cache[cpu][slot].lba == lba) {
            if (ok) {
                memcpy(inode_table_cache[cpu][slot].data, io_buf_guard.get(), YFS_BLOCK_SIZE);
                inode_table_cache[cpu][slot].flags.store(k_inode_cache_valid, kernel::memory_order::release);
            } else {
                inode_table_cache[cpu][slot].flags.store(0u, kernel::memory_order::release);
            }

            inode_table_cache[cpu][slot].io_done.signal_all();
        }

        spinlock_release(&inode_table_cache_lock[cpu]);

        if (!ok) {
            return 0;
        }
    }
}

static yfs_blk_t resolve_block(yfs_inode_t* node, uint32_t file_block, int alloc) {
    /*
     * Centralized block mapping.
     * Allocation is caller-driven; reads must not allocate.
     */
    if (file_block < YFS_DIRECT_PTRS) {
        if (node->direct[file_block] == 0) {
            if (!alloc) {
                return 0;
            }

            node->direct[file_block] = alloc_block();
        }

        return node->direct[file_block];
    }

    file_block -= YFS_DIRECT_PTRS;

    if (file_block < PTRS_PER_BLOCK) {
        if (node->indirect == 0) {
            if (!alloc) {
                return 0;
            }

            node->indirect = alloc_block();
        }

        yfs_blk_t* table = (yfs_blk_t*)kmalloc(YFS_BLOCK_SIZE);
        if (!table) {
            return 0;
        }

        bcache_read(node->indirect, (uint8_t*)table);

        yfs_blk_t res = table[file_block];
        if (res == 0 && alloc) {
            res = alloc_block();
            table[file_block] = res;
            bcache_write(node->indirect, (uint8_t*)table);
        }

        kfree(table);
        return res;
    }

    file_block -= PTRS_PER_BLOCK;

    if (file_block < (PTRS_PER_BLOCK * PTRS_PER_BLOCK)) {
        if (node->doubly_indirect == 0) {
            if (!alloc) {
                return 0;
            }

            node->doubly_indirect = alloc_block();
        }

        uint32_t l1_idx = file_block / PTRS_PER_BLOCK;
        uint32_t l2_idx = file_block % PTRS_PER_BLOCK;

        yfs_blk_t* l1 = (yfs_blk_t*)kmalloc(YFS_BLOCK_SIZE);
        if (!l1) {
            return 0;
        }

        bcache_read(node->doubly_indirect, (uint8_t*)l1);

        if (l1[l1_idx] == 0) {
            if (!alloc) {
                kfree(l1);
                return 0;
            }

            l1[l1_idx] = alloc_block();
            bcache_write(node->doubly_indirect, (uint8_t*)l1);
        }

        yfs_blk_t l2_blk = l1[l1_idx];
        kfree(l1);

        yfs_blk_t* l2 = (yfs_blk_t*)kmalloc(YFS_BLOCK_SIZE);
        if (!l2) {
            return 0;
        }

        bcache_read(l2_blk, (uint8_t*)l2);

        yfs_blk_t res = l2[l2_idx];
        if (res == 0 && alloc) {
            res = alloc_block();
            l2[l2_idx] = res;
            bcache_write(l2_blk, (uint8_t*)l2);
        }

        kfree(l2);
        return res;
    }

    file_block -= (PTRS_PER_BLOCK * PTRS_PER_BLOCK);

    if (file_block < (PTRS_PER_BLOCK * PTRS_PER_BLOCK * PTRS_PER_BLOCK)) {
        if (node->triply_indirect == 0) {
            if (!alloc) {
                return 0;
            }

            node->triply_indirect = alloc_block();
        }

        uint32_t ptrs_sq = PTRS_PER_BLOCK * PTRS_PER_BLOCK;
        uint32_t i1 = file_block / ptrs_sq;
        uint32_t rem = file_block % ptrs_sq;
        uint32_t i2 = rem / PTRS_PER_BLOCK;
        uint32_t i3 = rem % PTRS_PER_BLOCK;

        yfs_blk_t* l1 = (yfs_blk_t*)kmalloc(YFS_BLOCK_SIZE);
        if (!l1) {
            return 0;
        }

        bcache_read(node->triply_indirect, (uint8_t*)l1);

        if (l1[i1] == 0) {
            if (!alloc) {
                kfree(l1);
                return 0;
            }

            l1[i1] = alloc_block();
            bcache_write(node->triply_indirect, (uint8_t*)l1);
        }

        yfs_blk_t l2_blk = l1[i1];
        kfree(l1);

        yfs_blk_t* l2 = (yfs_blk_t*)kmalloc(YFS_BLOCK_SIZE);
        if (!l2) {
            return 0;
        }

        bcache_read(l2_blk, (uint8_t*)l2);

        if (l2[i2] == 0) {
            if (!alloc) {
                kfree(l2);
                return 0;
            }

            l2[i2] = alloc_block();
            bcache_write(l2_blk, (uint8_t*)l2);
        }

        yfs_blk_t l3_blk = l2[i2];
        kfree(l2);

        yfs_blk_t* l3 = (yfs_blk_t*)kmalloc(YFS_BLOCK_SIZE);
        if (!l3) {
            return 0;
        }

        bcache_read(l3_blk, (uint8_t*)l3);

        yfs_blk_t res = l3[i3];
        if (res == 0 && alloc) {
            res = alloc_block();
            l3[i3] = res;
            bcache_write(l3_blk, (uint8_t*)l3);
        }

        kfree(l3);
        return res;
    }

    return 0;
}

static void free_indir_level(yfs_blk_t block, int level) {
    /*
     * Free an indirect pointer subtree.
     * If table buffer allocation fails, still free the table block.
     */
    if (block == 0) {
        return;
    }

    if (level > 0) {
        yfs_blk_t* table = (yfs_blk_t*)kmalloc(YFS_BLOCK_SIZE);
        if (table) {
            bcache_read(block, (uint8_t*)table);

            for (uint32_t i = 0; i < PTRS_PER_BLOCK; i++) {
                if (table[i]) {
                    free_indir_level(table[i], level - 1);
                }
            }

            kfree(table);
        }
    }

    free_block(block);
}

static void truncate_inode(yfs_inode_t* node) {
    /*
     * Drop all block pointers from inode.
     * Caller serializes and writes inode back.
     */
    for (int i = 0; i < YFS_DIRECT_PTRS; i++) {
        if (node->direct[i]) {
            free_block(node->direct[i]);
        }

        node->direct[i] = 0;
    }

    if (node->indirect) {
        free_indir_level(node->indirect, 1);
        node->indirect = 0;
    }

    if (node->doubly_indirect) {
        free_indir_level(node->doubly_indirect, 2);
        node->doubly_indirect = 0;
    }

    if (node->triply_indirect) {
        free_indir_level(node->triply_indirect, 3);
        node->triply_indirect = 0;
    }

    node->size = 0;
}

static void clear_inode_to_free(yfs_ino_t ino, yfs_inode_t* node) {
    if (!node) {
        return;
    }

    memset(node, 0, sizeof(*node));
    node->id = ino;
    node->type = YFS_TYPE_FREE;
}

static int inode_finalize_deferred_delete_locked(yfs_ino_t ino, yfs_inode_t* node) {
    if (!node) {
        return -1;
    }

    truncate_inode(node);
    clear_inode_to_free(ino, node);
    sync_inode(ino, node, 1);

    free_inode(ino);

    flush_bitmap_cache();
    flush_sb();

    return 0;
}

static void orphan_inode_cleanup(void) {
    if (!fs_mounted || sb.magic != YFS_MAGIC) {
        return;
    }

    if (!inode_state || inode_state_count != sb.total_inodes) {
        return;
    }

    const uint32_t bits_per_block = YFS_BLOCK_SIZE * 8u;
    const uint32_t map_blocks = (sb.total_inodes + bits_per_block - 1u) / bits_per_block;

    uint8_t* map = static_cast<uint8_t*>(kmalloc(YFS_BLOCK_SIZE));
    if (!map) {
        panic("yulafs: OOM during orphan cleanup");
    }

    int freed_any = 0;
    for (uint32_t blk = 0; blk < map_blocks; blk++) {
        if (!bcache_read(sb.map_inode_start + blk, map)) {
            continue;
        }

        for (uint32_t bit = 0; bit < bits_per_block; bit++) {
            const uint32_t ino_u32 = blk * bits_per_block + bit;
            if (ino_u32 == 0 || ino_u32 >= sb.total_inodes) {
                continue;
            }

            if (!chk_bit(map, (int)bit)) {
                continue;
            }

            const yfs_ino_t ino = static_cast<yfs_ino_t>(ino_u32);
            yfs_inode_t node;
            if (!sync_inode(ino, &node, 0)) {
                continue;
            }

            if ((node.flags & YFS_INODE_F_DEFERRED_DELETE) == 0u) {
                continue;
            }

            rwlock_t* lock = get_inode_lock(ino);
            rwlock_acquire_write(lock);

            if (!sync_inode(ino, &node, 0)) {
                rwlock_release_write(lock);
                continue;
            }

            if ((node.flags & YFS_INODE_F_DEFERRED_DELETE) == 0u) {
                rwlock_release_write(lock);
                continue;
            }

            (void)inode_finalize_deferred_delete_locked(ino, &node);
            rwlock_release_write(lock);

            freed_any = 1;
        }
    }

    kfree(map);

    if (freed_any) {
        bcache_sync();
    }
}

static yfs_ino_t dir_find(yfs_inode_t* dir, const char* name) {
    /*
     * dir_find() runs unlocked; higher layers decide when to take inode locks.
     * Cache is updated on hit.
     */
    yfs_ino_t cached = dcache_lookup(dir->id, name);
    if (cached) {
        return cached;
    }

    uint32_t entries_per_block = YFS_BLOCK_SIZE / sizeof(yfs_dirent_t);
    yfs_dirent_t* entries = (yfs_dirent_t*)kmalloc(YFS_BLOCK_SIZE);
    if (!entries) {
        return 0;
    }

    uint32_t blocks = (dir->size + YFS_BLOCK_SIZE - 1) / YFS_BLOCK_SIZE;

    for (uint32_t i = 0; i < blocks; i++) {
        yfs_blk_t lba = resolve_block(dir, i, 0);
        if (!lba) {
            continue;
        }

        bcache_read(lba, (uint8_t*)entries);
        for (uint32_t j = 0; j < entries_per_block; j++) {
            if (entries[j].inode != 0 && strcmp(entries[j].name, name) == 0) {
                dcache_insert(dir->id, name, entries[j].inode);
                yfs_ino_t res = entries[j].inode;

                kfree(entries);
                return res;
            }
        }
    }

    kfree(entries);
    return 0;
}

static int dir_link(yfs_ino_t dir_ino, yfs_ino_t child_ino, const char* name) {
    /*
     * Directory update under inode write lock.
     * dcache is updated on success path; rename/unlink invalidate explicitly.
     */
    rwlock_t* lock = get_inode_lock(dir_ino);
    rwlock_acquire_write(lock);

    yfs_inode_t dir;
    sync_inode(dir_ino, &dir, 0);

    dcache_insert(dir_ino, name, child_ino);

    uint32_t entries_per_block = YFS_BLOCK_SIZE / sizeof(yfs_dirent_t);
    yfs_dirent_t* entries = (yfs_dirent_t*)kmalloc(YFS_BLOCK_SIZE);
    if (!entries) {
        rwlock_release_write(lock);
        return -1;
    }

    uint32_t blk_idx = 0;
    while (1) {
        yfs_blk_t lba = resolve_block(&dir, blk_idx, 1);
        if (!lba) {
            kfree(entries);
            rwlock_release_write(lock);
            return -1;
        }

        bcache_read(lba, (uint8_t*)entries);

        for (uint32_t i = 0; i < entries_per_block; i++) {
            if (entries[i].inode == 0) {
                entries[i].inode = child_ino;
                strlcpy(entries[i].name, name, YFS_NAME_MAX);
                bcache_write(lba, (uint8_t*)entries);

                uint32_t min_size = (blk_idx + 1) * YFS_BLOCK_SIZE;
                if (dir.size < min_size) {
                    dir.size = min_size;
                    sync_inode(dir_ino, &dir, 1);
                }

                kfree(entries);
                rwlock_release_write(lock);
                return 0;
            }
        }

        blk_idx++;
    }
}

static int dir_unlink(yfs_ino_t dir_ino, const char* name) {
    /*
     * Unlink drops the directory entry.
     * The inode may stay alive until the last open reference is released.
     */
    rwlock_t* lock = get_inode_lock(dir_ino);
    rwlock_acquire_write(lock);

    yfs_inode_t dir;
    sync_inode(dir_ino, &dir, 0);

    dcache_invalidate_entry(dir_ino, name);

    uint32_t entries_per_block = YFS_BLOCK_SIZE / sizeof(yfs_dirent_t);
    yfs_dirent_t* entries = (yfs_dirent_t*)kmalloc(YFS_BLOCK_SIZE);
    if (!entries) {
        rwlock_release_write(lock);
        return -1;
    }

    uint32_t blocks = (dir.size + YFS_BLOCK_SIZE - 1) / YFS_BLOCK_SIZE;

    for (uint32_t i = 0; i < blocks; i++) {
        yfs_blk_t lba = resolve_block(&dir, i, 0);
        if (!lba) {
            continue;
        }

        bcache_read(lba, (uint8_t*)entries);
        for (uint32_t j = 0; j < entries_per_block; j++) {
            if (entries[j].inode != 0 && strcmp(entries[j].name, name) == 0) {
                yfs_ino_t child_id = entries[j].inode;
                entries[j].inode = 0;
                memset(entries[j].name, 0, YFS_NAME_MAX);
                bcache_write(lba, (uint8_t*)entries);

                yfs_inode_t child;

                rwlock_t* child_lock = get_inode_lock(child_id);
                rwlock_acquire_write(child_lock);

                if (sync_inode(child_id, &child, 0)) {
                    const yfs::FileSystem::State::InodeRuntimeState* rt = inode_rt(child_id);
                    const uint32_t open_refs = rt
                        ? rt->open_refs.load(kernel::memory_order::acquire)
                        : 0u;

                    if (open_refs == 0u) {
                        (void)inode_finalize_deferred_delete_locked(child_id, &child);
                    } else {
                        child.flags |= YFS_INODE_F_DEFERRED_DELETE;
                        sync_inode(child_id, &child, 1);
                    }
                }

                rwlock_release_write(child_lock);

                kfree(entries);
                rwlock_release_write(lock);
                return 0;
            }
        }
    }

    kfree(entries);
    rwlock_release_write(lock);
    return -1;
}

static int dir_unlink_entry_only(yfs_ino_t dir_ino, const char* name) {
    /* Used by rename(): drop name without touching target inode. */
    yfs_inode_t dir;
    sync_inode(dir_ino, &dir, 0);

    dcache_invalidate_entry(dir_ino, name);

    uint32_t entries_per_block = YFS_BLOCK_SIZE / sizeof(yfs_dirent_t);
    yfs_dirent_t* entries = (yfs_dirent_t*)kmalloc(YFS_BLOCK_SIZE);
    if (!entries) {
        return -1;
    }

    uint32_t blocks = (dir.size + YFS_BLOCK_SIZE - 1) / YFS_BLOCK_SIZE;

    for (uint32_t i = 0; i < blocks; i++) {
        yfs_blk_t lba = resolve_block(&dir, i, 0);
        if (!lba) {
            continue;
        }

        bcache_read(lba, (uint8_t*)entries);
        for (uint32_t j = 0; j < entries_per_block; j++) {
            if (entries[j].inode != 0 && strcmp(entries[j].name, name) == 0) {
                entries[j].inode = 0;

                memset(entries[j].name, 0, YFS_NAME_MAX);
                bcache_write(lba, (uint8_t*)entries);

                kfree(entries);
                return 0;
            }
        }
    }

    kfree(entries);
    return -1;
}

static yfs_ino_t path_to_inode_impl(const char* path, char* last_element, int allow_special_last) {
    /*
     * Return parent directory inode and final component name.
     */
    if (!path || !*path) {
        if (last_element) {
            last_element[0] = '\0';
        }
        return 0;
    }

    yfs_ino_t curr = (path[0] == '/') ? 1 : proc_current()->cwd_inode;
    if (curr == 0) {
        curr = 1;
    }

    const char* p = path;
    while (*p == '/') {
        p++;
    }

    char name[YFS_NAME_MAX];

    for (;;) {
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            if (last_element) {
                last_element[0] = '\0';
            }
            return 0;
        }

        const char* start = p;
        while (*p && *p != '/') {
            p++;
        }

        const size_t len = (size_t)(p - start);
        if (len == 0 || len >= YFS_NAME_MAX) {
            return 0;
        }

        memcpy(name, start, len);
        name[len] = '\0';

        const char* next = p;
        while (*next == '/') {
            next++;
        }

        const int is_last = (*next == '\0');
        if (is_last) {
            if (!allow_special_last) {
                if (len == 1 && name[0] == '.') {
                    return 0;
                }
                if (len == 2 && name[0] == '.' && name[1] == '.') {
                    return 0;
                }
            }

            if (last_element) {
                strlcpy(last_element, name, YFS_NAME_MAX);
            }
            return curr;
        }

        if (len == 1 && name[0] == '.') {
            p = next;
            continue;
        }

        if (len == 2 && name[0] == '.' && name[1] == '.') {
            int parent_i = yulafs_lookup_in_dir(curr, "..");
            if (parent_i <= 0) {
                return 0;
            }
            curr = (yfs_ino_t)parent_i;
            p = next;
            continue;
        }

        yfs_inode_t dir_node;
        if (!sync_inode(curr, &dir_node, 0)) {
            return 0;
        }

        yfs_ino_t next_ino = dir_find(&dir_node, name);
        if (next_ino == 0) {
            return 0;
        }

        curr = next_ino;
        p = next;
    }
}

static yfs_ino_t path_to_inode(const char* path, char* last_element) {
    return path_to_inode_impl(path, last_element, 1);
}

static yfs_ino_t path_to_inode_for_modify(const char* path, char* last_element) {
    return path_to_inode_impl(path, last_element, 0);
}

static int yulafs_find_child_name_in_dir(yfs_ino_t dir_ino, yfs_ino_t child_ino, char* out_name, uint32_t out_cap) {
    /* inode_to_path() helper; skip dot entries to avoid loops. */
    if (!out_name || out_cap == 0) {
        return -1;
    }
    out_name[0] = '\0';

    if (dir_ino == 0 || child_ino == 0) {
        return -1;
    }

    yfs_dirent_t entries[8];
    uint32_t offset = 0;
    for (;;) {
        int bytes = yulafs_read(dir_ino, (uint8_t*)entries, (yfs_off_t)offset, (uint32_t)sizeof(entries));
        if (bytes <= 0) {
            break;
        }

        int count = bytes / (int)sizeof(yfs_dirent_t);
        if (count <= 0) {
            break;
        }

        for (int i = 0; i < count; i++) {
            if (entries[i].inode != child_ino) {
                continue;
            }
            if (strcmp(entries[i].name, ".") == 0) {
                continue;
            }
            if (strcmp(entries[i].name, "..") == 0) {
                continue;
            }
            strlcpy(out_name, entries[i].name, (size_t)out_cap);
            return 0;
        }

        offset += (uint32_t)bytes;
    }

    return -1;
}

int yulafs_inode_to_path(yfs_ino_t inode, char* out, uint32_t out_size) {
    /* Walk ".." to root; resolve names by scanning parent dirents. */
    if (!out || out_size == 0) {
        return -1;
    }

    yfs_ino_t cur = inode ? inode : 1u;
    if (cur == 1u) {
        if (out_size < 2u) {
            return -1;
        }
        out[0] = '/';
        out[1] = '\0';
        return 1;
    }

    char parts[64][YFS_NAME_MAX];
    uint32_t depth = 0;

    while (cur != 1u) {
        if (depth >= (uint32_t)(sizeof(parts) / sizeof(parts[0]))) {
            return -1;
        }

        int parent_i = yulafs_lookup_in_dir(cur, "..");
        if (parent_i <= 0) {
            return -1;
        }

        yfs_ino_t parent = (yfs_ino_t)parent_i;

        if (yulafs_find_child_name_in_dir(parent, cur, parts[depth], (uint32_t)sizeof(parts[depth])) != 0) {
            return -1;
        }

        depth++;
        if (parent == cur) {
            return -1;
        }
        cur = parent;
    }

    if (out_size < 2u) {
        return -1;
    }

    uint32_t len = 1;
    out[0] = '/';

    for (uint32_t i = depth; i > 0; i--) {
        const char* part = parts[i - 1];
        const size_t plen = strlen(part);
        if (plen == 0) {
            continue;
        }

        if (len > 1) {
            if (len + 1 >= out_size) {
                return -1;
            }
            out[len++] = '/';
        }

        if (len + plen >= out_size) {
            return -1;
        }

        memcpy(out + len, part, plen);
        len += (uint32_t)plen;
    }

    out[len] = '\0';
    return (int)len;
}

void yfs::FileSystem::format(uint32_t disk_blocks_4k) {
    /* Full format. */
    uint8_t* zero = (uint8_t*)kmalloc(YFS_BLOCK_SIZE);
    uint8_t* map = (uint8_t*)kmalloc(YFS_BLOCK_SIZE);
    yfs_dirent_t* dots = (yfs_dirent_t*)kmalloc(YFS_BLOCK_SIZE);

    if (!zero || !map || !dots) {
        if (zero) {
            kfree(zero);
        }
        if (map) {
            kfree(map);
        }
        if (dots) {
            kfree(dots);
        }
        return;
    }

    memset(zero, 0, YFS_BLOCK_SIZE);
    memset(map, 0, YFS_BLOCK_SIZE);
    memset(dots, 0, YFS_BLOCK_SIZE);

    memset(&sb, 0, sizeof(sb));
    sb.magic = YFS_MAGIC;
    sb.version = YFS_VERSION;
    sb.block_size = YFS_BLOCK_SIZE;
    sb.total_blocks = disk_blocks_4k;

    sb.total_inodes = disk_blocks_4k / 8;
    if (sb.total_inodes < 128) {
        sb.total_inodes = 128;
    }

    uint32_t sec_per_map = (sb.total_blocks + (YFS_BLOCK_SIZE * 8) - 1) / (YFS_BLOCK_SIZE * 8);
    uint32_t sec_per_imap = (sb.total_inodes + (YFS_BLOCK_SIZE * 8) - 1) / (YFS_BLOCK_SIZE * 8);
    uint32_t sec_inodes = (sb.total_inodes * sizeof(yfs_inode_t) + YFS_BLOCK_SIZE - 1) / YFS_BLOCK_SIZE;

    sb.map_inode_start = 2;
    sb.map_block_start = sb.map_inode_start + sec_per_imap;
    sb.inode_table_start = sb.map_block_start + sec_per_map;
    sb.data_start = sb.inode_table_start + sec_inodes;

    sb.free_inodes = sb.total_inodes;
    sb.free_blocks = sb.total_blocks - sb.data_start;

    for (uint32_t i = 0; i < sec_per_imap; i++) {
        bcache_write(sb.map_inode_start + i, zero);
    }

    for (uint32_t i = 0; i < sec_per_map; i++) {
        bcache_write(sb.map_block_start + i, zero);
    }

    for (uint32_t i = 0; i < sec_inodes; i++) {
        bcache_write(sb.inode_table_start + i, zero);
    }

    set_bit(map, 0);
    set_bit(map, 1);
    bcache_write(sb.map_inode_start, map);
    sb.free_inodes -= 2;

    yfs_inode_t root;
    memset(&root, 0, sizeof(root));
    root.id = 1;
    root.type = YFS_TYPE_DIR;
    root.size = YFS_BLOCK_SIZE;
    root.direct[0] = alloc_block();

    dots[0].inode = 1;
    strlcpy(dots[0].name, ".", YFS_NAME_MAX);
    dots[1].inode = 1;
    strlcpy(dots[1].name, "..", YFS_NAME_MAX);
    bcache_write(root.direct[0], (uint8_t*)dots);

    sync_inode(1, &root, 1);
    flush_sb();
    flush_bitmap_cache();
    bcache_sync();

    fs_mounted = 1;
    last_free_blk_hint = 0;
    last_free_ino_hint = 2;

    inode_state_init_or_panic(sb.total_inodes);

    yfs_state.dcache.clear();

    yulafs_mkdir("/bin");
    yulafs_mkdir("/home");
    yulafs_mkdir("/dev");
    bcache_sync();

    kfree(zero);
    kfree(map);
    kfree(dots);
}

void yfs::FileSystem::init() {
    /* Mount on valid magic, otherwise format. */
    bcache_init();

    for (int cpu = 0; cpu < cpu_count; cpu++) {
        spinlock_init(&inode_table_cache_lock[cpu]);
        inode_table_cache_stamp[cpu] = 0;

        for (int i = 0; i < INODE_TABLE_CACHE_SLOTS; i++) {
            inode_table_cache[cpu][i].lba = 0;
            inode_table_cache[cpu][i].stamp = 0;
            inode_table_cache[cpu][i].flags.store(0, kernel::memory_order::relaxed);
            inode_table_cache[cpu][i].io_done.init(0);
            memset(inode_table_cache[cpu][i].data, 0, sizeof(inode_table_cache[cpu][i].data));
        }

        spinlock_init(&yfs_scratch_lock[cpu]);

        for (int i = 0; i < YFS_SCRATCH_SLOTS; i++) {
            yfs_scratch_used[cpu][i] = 0;
        }
    }

    bmap_cache_lba = 0;
    bmap_cache_dirty = 0;

    state_.dcache.clear();

    uint8_t* buf = (uint8_t*)kmalloc(YFS_BLOCK_SIZE);
    if (!buf) {
        return;
    }

    if (!bcache_read(1, buf)) {
        kfree(buf);
        return;
    }

    memcpy(&sb, buf, sizeof(sb));

    kfree(buf);

    if (sb.magic != YFS_MAGIC) {
        uint64_t capacity_sectors = 0;
        if (block_device_t* root = bdev_root()) {
            capacity_sectors = root->sector_count;
        }

        if (capacity_sectors == 0) {
            capacity_sectors = 131072;
        }

        yulafs_format((uint32_t)(capacity_sectors / 8ull));
    } else {
        fs_mounted = 1;
        last_free_blk_hint = 0;
        last_free_ino_hint = 1;

        inode_state_init_or_panic(sb.total_inodes);
        orphan_inode_cleanup();
    }
}

void yulafs_format(uint32_t disk_blocks_4k) {
    yfs::g_fs.format(disk_blocks_4k);
}

static void inode_get_open(yfs_ino_t ino) {
    auto* rt = inode_rt(ino);
    if (!rt) {
        return;
    }

    rt->open_refs.fetch_add(1u, kernel::memory_order::acquire);
}

static uint32_t inode_put_open(yfs_ino_t ino) {
    auto* rt = inode_rt(ino);
    if (!rt) {
        return 0;
    }

    const uint32_t prev = rt->open_refs.fetch_sub(1u, kernel::memory_order::release);
    if (prev == 0u) {
        panic("yulafs: inode open_refs underflow");
    }

    return prev - 1u;
}

static int inode_finalize_if_needed(yfs_ino_t ino) {
    yfs_inode_t node;
    if (!sync_inode(ino, &node, 0)) {
        return -1;
    }

    if ((node.flags & YFS_INODE_F_DEFERRED_DELETE) == 0u) {
        return 0;
    }

    rwlock_t* lock = get_inode_lock(ino);
    rwlock_acquire_write(lock);

    if (!sync_inode(ino, &node, 0)) {
        rwlock_release_write(lock);
        return -1;
    }

    if ((node.flags & YFS_INODE_F_DEFERRED_DELETE) == 0u) {
        rwlock_release_write(lock);
        return 0;
    }

    const int rc = inode_finalize_deferred_delete_locked(ino, &node);
    rwlock_release_write(lock);
    return rc;
}

int yulafs_inode_open(yfs_ino_t ino) {
    if (!fs_mounted) {
        return -1;
    }

    inode_get_open(ino);
    return 0;
}

int yulafs_inode_close(yfs_ino_t ino) {
    if (!fs_mounted) {
        return -1;
    }

    const uint32_t refs = inode_put_open(ino);
    if (refs == 0u) {
        return inode_finalize_if_needed(ino);
    }

    return 0;
}

void yulafs_init(void) {
    yfs::g_fs.init();
}

int yfs::FileSystem::read(yfs_ino_t ino, void* buf, yfs_off_t offset, uint32_t size) {
    if (!fs_mounted || !buf) {
        return -1;
    }

    if (size == 0) {
        return 0;
    }

    if (offset > 0xFFFFFFFF - size) {
        return -1;
    }

    rwlock_t* lock = get_inode_lock(ino);
    rwlock_acquire_read(lock);

    yfs_inode_t node;
    if (!sync_inode(ino, &node, 0)) {
        rwlock_release_read(lock);
        return -1;
    }

    if (node.size > 0xFFFFFFFF) {
        rwlock_release_read(lock);
        return -1;
    }

    if (offset >= node.size) {
        rwlock_release_read(lock);
        return 0;
    }

    uint64_t total = (uint64_t)offset + (uint64_t)size;
    if (total > node.size) {
        size = node.size - offset;
    }

    uint32_t read_count = 0;

    int scratch_slot = -1;
    uint8_t* scratch = yfs_scratch_acquire(&scratch_slot);
    int scratch_heap = 0;

    if (!scratch) {
        scratch = (uint8_t*)kmalloc_a(YFS_BLOCK_SIZE);
        scratch_heap = 1;
    }

    if (!scratch) {
        rwlock_release_read(lock);
        return -1;
    }

    uint32_t last_prefetched_log_blk = 0xFFFFFFFF;

    while (read_count < size) {
        uint32_t log_blk = (offset + read_count) / YFS_BLOCK_SIZE;
        uint32_t blk_off = (offset + read_count) % YFS_BLOCK_SIZE;
        uint32_t phys_blk = resolve_block(&node, log_blk, 0);

        uint32_t copy_len = YFS_BLOCK_SIZE - blk_off;
        if (copy_len > size - read_count) {
            copy_len = size - read_count;
        }

        if (copy_len > YFS_BLOCK_SIZE || copy_len > size - read_count) {
            if (scratch_heap) {
                kfree(scratch);
            } else {
                yfs_scratch_release(scratch_slot);
            }

            rwlock_release_read(lock);
            return -1;
        }

        if (phys_blk) {
            if (!bcache_read(phys_blk, scratch)) {
                if (scratch_heap) {
                    kfree(scratch);
                } else {
                    yfs_scratch_release(scratch_slot);
                }

                rwlock_release_read(lock);
                return -1;
            }

            if (
                read_count + copy_len > size ||
                blk_off + copy_len > YFS_BLOCK_SIZE
            ) {
                if (scratch_heap) {
                    kfree(scratch);
                } else {
                    yfs_scratch_release(scratch_slot);
                }

                rwlock_release_read(lock);
                return -1;
            }

            memcpy((uint8_t*)buf + read_count, scratch + blk_off, copy_len);

            if (log_blk != last_prefetched_log_blk) {
                uint32_t next_log_blk = log_blk + 1;
                uint32_t next_phys_blk = resolve_block(&node, next_log_blk, 0);

                if (next_phys_blk) {
                    uint32_t blocks_remaining =
                        ((node.size - (offset + read_count)) + YFS_BLOCK_SIZE - 1) / YFS_BLOCK_SIZE;

                    if (blocks_remaining > 0) {
                        bcache_readahead(phys_blk, blocks_remaining > 8 ? 8 : blocks_remaining);
                    }
                }

                last_prefetched_log_blk = log_blk;
            }
        } else {
            memset((uint8_t*)buf + read_count, 0, copy_len);
        }

        read_count += copy_len;
    }

    if (scratch_heap) {
        kfree(scratch);
    } else {
        yfs_scratch_release(scratch_slot);
    }

    rwlock_release_read(lock);

    return read_count;
}

int yulafs_read(yfs_ino_t ino, void* buf, yfs_off_t offset, uint32_t size) {
    return yfs::g_fs.read(ino, buf, offset, size);
}

static int yulafs_write_locked(yfs_ino_t ino, yfs_inode_t* node, const void* buf, yfs_off_t offset, uint32_t size);

int yfs::FileSystem::write(yfs_ino_t ino, const void* buf, yfs_off_t offset, uint32_t size) {
    if (!fs_mounted || !buf) {
        return -1;
    }

    if (size == 0) {
        return 0;
    }

    if (offset > 0xFFFFFFFF - size) {
        return -1;
    }

    rwlock_t* lock = get_inode_lock(ino);
    rwlock_acquire_write(lock);

    yfs_inode_t node;
    if (!sync_inode(ino, &node, 0)) {
        rwlock_release_write(lock);
        return -1;
    }

    int rc = yulafs_write_locked(ino, &node, buf, offset, size);

    rwlock_release_write(lock);

    return rc;
}

int yulafs_write(yfs_ino_t ino, const void* buf, yfs_off_t offset, uint32_t size) {
    return yfs::g_fs.write(ino, buf, offset, size);
}

static int yulafs_write_locked(
    yfs_ino_t ino,
    yfs_inode_t* node,
    const void* buf,
    yfs_off_t offset,
    uint32_t size
) {
    if (!node || !buf) {
        return -1;
    }

    uint32_t written = 0;

    int dirty = 0;
    int blocks_allocated = 0;

    int scratch_slot = -1;
    uint8_t* scratch = yfs_scratch_acquire(&scratch_slot);
    int scratch_heap = 0;

    if (!scratch) {
        scratch = (uint8_t*)kmalloc(YFS_BLOCK_SIZE);
        scratch_heap = 1;
    }

    if (!scratch) {
        return -1;
    }

    while (written < size) {
        uint32_t log_blk = (offset + written) / YFS_BLOCK_SIZE;
        uint32_t blk_off = (offset + written) % YFS_BLOCK_SIZE;

        yfs_blk_t phys_blk = resolve_block(node, log_blk, 1);
        if (!phys_blk) {
            break;
        }

        blocks_allocated = 1;

        uint32_t copy_len = YFS_BLOCK_SIZE - blk_off;
        if (copy_len > size - written) {
            copy_len = size - written;
        }

        if (copy_len > YFS_BLOCK_SIZE || copy_len > size - written) {
            if (scratch_heap) {
                kfree(scratch);
            } else {
                yfs_scratch_release(scratch_slot);
            }

            return written > 0 ? (int)written : -1;
        }

        if (copy_len < YFS_BLOCK_SIZE) {
            if (!bcache_read(phys_blk, scratch)) {
                if (scratch_heap) {
                    kfree(scratch);
                } else {
                    yfs_scratch_release(scratch_slot);
                }

                return written > 0 ? (int)written : -1;
            }
        }

        if (written + copy_len > size || blk_off + copy_len > YFS_BLOCK_SIZE) {
            if (scratch_heap) {
                kfree(scratch);
            } else {
                yfs_scratch_release(scratch_slot);
            }

            return written > 0 ? (int)written : -1;
        }

        memcpy(scratch + blk_off, (const uint8_t*)buf + written, copy_len);
        bcache_write(phys_blk, scratch);

        written += copy_len;
        dirty = 1;
    }

    if (offset + written > node->size) {
        node->size = offset + written;
        dirty = 1;
    }

    if (dirty) {
        sync_inode(ino, node, 1);
    }

    if (blocks_allocated) {
        flush_bitmap_cache();
        flush_sb();
    }

    if (scratch_heap) {
        kfree(scratch);
    } else {
        yfs_scratch_release(scratch_slot);
    }

    return (int)written;
}

int yfs::FileSystem::append(yfs_ino_t ino, const void* buf, uint32_t size, yfs_off_t* out_start_off) {
    if (!fs_mounted || !buf) {
        return -1;
    }

    if (size == 0) {
        if (out_start_off) {
            *out_start_off = 0;
        }
        return 0;
    }

    rwlock_t* lock = get_inode_lock(ino);
    rwlock_acquire_write(lock);

    yfs_inode_t node;
    if (!sync_inode(ino, &node, 0)) {
        rwlock_release_write(lock);
        return -1;
    }

    yfs_off_t start = (yfs_off_t)node.size;
    if (out_start_off) {
        *out_start_off = start;
    }

    int rc = yulafs_write_locked(ino, &node, buf, start, size);
    rwlock_release_write(lock);

    return rc;
}

int yulafs_create_obj(const char* path, int type) {
    char name[YFS_NAME_MAX];
    yfs_ino_t dir_ino = path_to_inode_for_modify(path, name);
    if (!dir_ino) {
        return -1;
    }

    yfs_inode_t dir;
    sync_inode(dir_ino, &dir, 0);

    if (dir_find(&dir, name) != 0) {
        return -1;
    }

    yfs_ino_t new_ino = alloc_inode();
    if (!new_ino) {
        return -1;
    }

    yfs_inode_t obj;
    memset(&obj, 0, sizeof(obj));
    obj.id = new_ino;
    obj.type = type;
    obj.size = 0;

    if (type == YFS_TYPE_DIR) {
        obj.size = YFS_BLOCK_SIZE;
        obj.direct[0] = alloc_block();

        uint8_t* buf = (uint8_t*)kmalloc(YFS_BLOCK_SIZE);
        if (buf) {
            memset(buf, 0, YFS_BLOCK_SIZE);

            yfs_dirent_t* dots = (yfs_dirent_t*)buf;

            dots[0].inode = new_ino;
            strlcpy(dots[0].name, ".", YFS_NAME_MAX);

            dots[1].inode = dir_ino;
            strlcpy(dots[1].name, "..", YFS_NAME_MAX);

            bcache_write(obj.direct[0], buf);

            kfree(buf);
        }
    }

    sync_inode(new_ino, &obj, 1);

    dir_link(dir_ino, new_ino, name);

    flush_bitmap_cache();
    flush_sb();

    return new_ino;
}

int yfs::FileSystem::mkdir(const char* path) {
    return yulafs_create_obj(path, YFS_TYPE_DIR);
}

int yulafs_mkdir(const char* path) {
    return yfs::g_fs.mkdir(path);
}

int yfs::FileSystem::create(const char* path) {
    return yulafs_create_obj(path, YFS_TYPE_FILE);
}

int yulafs_create(const char* path) {
    return yfs::g_fs.create(path);
}

int yfs::FileSystem::unlink(const char* path) {
    char name[YFS_NAME_MAX];
    yfs_ino_t dir_ino = path_to_inode_for_modify(path, name);

    if (!dir_ino) {
        return -1;
    }

    return dir_unlink(dir_ino, name);
}

int yulafs_unlink(const char* path) {
    return yfs::g_fs.unlink(path);
}

int yfs::FileSystem::lookup(const char* path) {
    if (!path || !*path) {
        return (int)proc_current()->cwd_inode;
    }

    if (strcmp(path, "/") == 0) {
        return 1;
    }

    char name[YFS_NAME_MAX];
    yfs_ino_t parent_dir = path_to_inode(path, name);

    if (parent_dir == 0) {
        return -1;
    }

    yfs_inode_t dir_node;
    sync_inode(parent_dir, &dir_node, 0);

    yfs_ino_t target = dir_find(&dir_node, name);
    if (target == 0) {
        return -1;
    }

    return (int)target;
}

int yulafs_lookup(const char* path) {
    return yfs::g_fs.lookup(path);
}

int yfs::FileSystem::lookup_in_dir(yfs_ino_t dir_ino, const char* name) {
    if (!name || !*name) {
        return -1;
    }

    rwlock_t* lock = get_inode_lock(dir_ino);
    rwlock_acquire_read(lock);

    yfs_inode_t dir;
    if (!sync_inode(dir_ino, &dir, 0)) {
        rwlock_release_read(lock);
        return -1;
    }

    if (dir.type != YFS_TYPE_DIR) {
        rwlock_release_read(lock);
        return -1;
    }

    yfs_ino_t ino = dir_find(&dir, name);
    rwlock_release_read(lock);

    if (!ino) {
        return -1;
    }

    return (int)ino;
}

int yulafs_lookup_in_dir(yfs_ino_t dir_ino, const char* name) {
    return yfs::g_fs.lookup_in_dir(dir_ino, name);
}

int yfs::FileSystem::getdents(
    yfs_ino_t dir_ino,
    uint32_t* inout_offset,
    yfs_dirent_info_t* out,
    uint32_t out_size
) {
    if (!inout_offset || !out) {
        return -1;
    }

    if (out_size < sizeof(yfs_dirent_info_t)) {
        return -1;
    }

    uint32_t max_entries = out_size / (uint32_t)sizeof(yfs_dirent_info_t);
    uint32_t out_count = 0;

    rwlock_t* lock = get_inode_lock(dir_ino);
    rwlock_acquire_read(lock);

    yfs_inode_t dir;
    if (!sync_inode(dir_ino, &dir, 0)) {
        rwlock_release_read(lock);
        return -1;
    }

    if (dir.type != YFS_TYPE_DIR) {
        rwlock_release_read(lock);
        return -1;
    }

    uint32_t entries_per_block = YFS_BLOCK_SIZE / (uint32_t)sizeof(yfs_dirent_t);
    uint32_t total_entries = (dir.size + (uint32_t)sizeof(yfs_dirent_t) - 1) / (uint32_t)sizeof(yfs_dirent_t);

    uint32_t idx = (*inout_offset) / (uint32_t)sizeof(yfs_dirent_t);
    if (((*inout_offset) % (uint32_t)sizeof(yfs_dirent_t)) != 0) {
        idx++;
    }

    int scratch_slot = -1;
    uint8_t* scratch = yfs_scratch_acquire(&scratch_slot);
    uint8_t* scratch_heap = 0;

    if (!scratch) {
        scratch = (uint8_t*)kmalloc(YFS_BLOCK_SIZE);
        scratch_heap = scratch;
        if (!scratch) {
            rwlock_release_read(lock);
            return -1;
        }
    }

    while (idx < total_entries && out_count < max_entries) {
        uint32_t blk_idx = idx / entries_per_block;
        uint32_t ent_idx = idx % entries_per_block;

        yfs_blk_t lba = resolve_block(&dir, blk_idx, 0);
        if (!lba) {
            idx = (blk_idx + 1) * entries_per_block;
            continue;
        }

        bcache_read(lba, scratch);
        yfs_dirent_t* ents = (yfs_dirent_t*)scratch;

        for (
            ; ent_idx < entries_per_block && idx < total_entries && out_count < max_entries;
            ent_idx++, idx++
        ) {
            if (ents[ent_idx].inode == 0) {
                continue;
            }

            yfs_dirent_info_t* d = &out[out_count++];
            d->inode = ents[ent_idx].inode;
            strlcpy(d->name, ents[ent_idx].name, YFS_NAME_MAX);

            yfs_inode_t child;
            if (sync_inode(d->inode, &child, 0)) {
                d->type = child.type;
                d->size = child.size;
            } else {
                d->type = 0;
                d->size = 0;
            }
        }
    }

    *inout_offset = idx * (uint32_t)sizeof(yfs_dirent_t);

    if (scratch_heap) {
        kfree(scratch_heap);
    } else {
        yfs_scratch_release(scratch_slot);
    }

    rwlock_release_read(lock);

    return (int)(out_count * (uint32_t)sizeof(yfs_dirent_info_t));
}

int yulafs_getdents(yfs_ino_t dir_ino, uint32_t* inout_offset, yfs_dirent_info_t* out, uint32_t out_size) {
    return yfs::g_fs.getdents(dir_ino, inout_offset, out, out_size);
}

int yfs::FileSystem::stat(yfs_ino_t ino, yfs_inode_t* out) {
    sync_inode(ino, out, 0);
    return 0;
}

int yulafs_stat(yfs_ino_t ino, yfs_inode_t* out) {
    return yfs::g_fs.stat(ino, out);
}

void yfs::FileSystem::resize(yfs_ino_t ino, uint32_t new_size) {
    rwlock_t* lock = get_inode_lock(ino);
    rwlock_acquire_write(lock);

    yfs_inode_t node;
    sync_inode(ino, &node, 0);

    if (new_size < node.size && new_size == 0) {
        truncate_inode(&node);
        flush_bitmap_cache();
        flush_sb();
    }

    node.size = new_size;
    sync_inode(ino, &node, 1);

    rwlock_release_write(lock);
}

void yulafs_resize(yfs_ino_t ino, uint32_t new_size) {
    yfs::g_fs.resize(ino, new_size);
}

void yfs::FileSystem::get_filesystem_info(uint32_t* total, uint32_t* free, uint32_t* size) {
    if (fs_mounted) {
        *total = sb.total_blocks;
        *free = sb.free_blocks;
        *size = sb.block_size;
    } else {
        *total = 0;
        *free = 0;
        *size = 0;
    }
}

void yulafs_get_filesystem_info(uint32_t* total, uint32_t* free, uint32_t* size) {
    yfs::g_fs.get_filesystem_info(total, free, size);
}

int yfs::FileSystem::rename(const char* old_path, const char* new_path) {
    char old_name[YFS_NAME_MAX];
    char new_name[YFS_NAME_MAX];

    yfs_ino_t old_dir = path_to_inode_for_modify(old_path, old_name);
    if (!old_dir) {
        return -1;
    }

    yfs_inode_t old_node;
    sync_inode(old_dir, &old_node, 0);

    yfs_ino_t target = dir_find(&old_node, old_name);
    if (!target) {
        return -1;
    }

    yfs_ino_t new_dir = path_to_inode_for_modify(new_path, new_name);
    if (!new_dir) {
        return -1;
    }

    if (dir_link(new_dir, target, new_name) != 0) {
        return -1;
    }

    dir_unlink_entry_only(old_dir, old_name);

    flush_bitmap_cache();
    flush_sb();

    return 0;
}

extern "C" int yulafs_append(yfs_ino_t ino, const void* buf, uint32_t size, yfs_off_t* out_start_off) {
    return yfs::g_fs.append(ino, buf, size, out_start_off);
}

extern "C" int yulafs_rename(const char* old_path, const char* new_path) {
    return yfs::g_fs.rename(old_path, new_path);
}
