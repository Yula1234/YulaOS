// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include "yulafs.h"
#include "bcache.h" 
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../kernel/proc.h"
#include "../drivers/vga.h"
#include <hal/lock.h>
#include <lib/rbtree.h>

#define PTRS_PER_BLOCK      (YFS_BLOCK_SIZE / 4) 
#define INODE_LOCK_BUCKETS  128

static yfs_superblock_t sb;
static int fs_mounted = 0;

static uint32_t last_free_blk_hint = 0;
static uint32_t last_free_ino_hint = 1;

static rwlock_t inode_locks[INODE_LOCK_BUCKETS];

static uint8_t  bmap_cache_data[YFS_BLOCK_SIZE];
static uint32_t bmap_cache_lba = 0; 
static int      bmap_cache_dirty = 0;

extern uint32_t ahci_get_capacity(void);

typedef struct {
    struct rb_node node;
    yfs_ino_t parent_ino;
    char name[YFS_NAME_MAX];
    yfs_ino_t target_ino;
} yfs_dcache_entry_t;

static struct rb_root dcache_root = RB_ROOT;
static spinlock_t dcache_lock;

#define INODE_TABLE_CACHE_SLOTS 4

typedef struct {
    uint32_t lba;
    uint32_t stamp;
    uint8_t  valid;
    uint8_t  data[YFS_BLOCK_SIZE];
} inode_table_cache_slot_t;

static inode_table_cache_slot_t inode_table_cache[INODE_TABLE_CACHE_SLOTS];
static spinlock_t inode_table_cache_lock;
static uint32_t inode_table_cache_stamp;

#define YFS_SCRATCH_SLOTS 4

static uint8_t yfs_scratch[YFS_SCRATCH_SLOTS][YFS_BLOCK_SIZE];
static uint8_t yfs_scratch_used[YFS_SCRATCH_SLOTS];
static spinlock_t yfs_scratch_lock;

static uint8_t* yfs_scratch_acquire(int* out_slot) {
    if (out_slot) *out_slot = -1;

    spinlock_acquire(&yfs_scratch_lock);
    for (int i = 0; i < YFS_SCRATCH_SLOTS; i++) {
        if (!yfs_scratch_used[i]) {
            yfs_scratch_used[i] = 1;
            spinlock_release(&yfs_scratch_lock);
            if (out_slot) *out_slot = i;
            return yfs_scratch[i];
        }
    }
    spinlock_release(&yfs_scratch_lock);
    return 0;
}

static void yfs_scratch_release(int slot) {
    if (slot < 0 || slot >= YFS_SCRATCH_SLOTS) return;
    spinlock_acquire(&yfs_scratch_lock);
    yfs_scratch_used[slot] = 0;
    spinlock_release(&yfs_scratch_lock);
}

static inline rwlock_t* get_inode_lock(yfs_ino_t ino) {
    return &inode_locks[ino % INODE_LOCK_BUCKETS];
}

static inline void set_bit(uint8_t* map, int i) { map[i/8] |= (1 << (i%8)); }
static inline void clr_bit(uint8_t* map, int i) { map[i/8] &= ~(1 << (i%8)); }
static inline int  chk_bit(uint8_t* map, int i) { return (map[i/8] & (1 << (i%8))); }

static int rb_compare(yfs_ino_t p1, const char* n1, yfs_ino_t p2, const char* n2) {
    if (p1 < p2) return -1;
    if (p1 > p2) return 1;
    return strcmp(n1, n2);
}

static void dcache_insert(yfs_ino_t parent, const char* name, yfs_ino_t target) {
    spinlock_acquire(&dcache_lock);

    struct rb_node **new_node = &(dcache_root.rb_node), *parent_node = 0;

    while (*new_node) {
        yfs_dcache_entry_t *this_entry = rb_entry(*new_node, yfs_dcache_entry_t, node);
        parent_node = *new_node;
        int cmp = rb_compare(parent, name, this_entry->parent_ino, this_entry->name);
        if (cmp == 0) {
            this_entry->target_ino = target;
            spinlock_release(&dcache_lock);
            return;
        }
        if (cmp < 0) new_node = &((*new_node)->rb_left);
        else new_node = &((*new_node)->rb_right);
    }

    spinlock_release(&dcache_lock);

    yfs_dcache_entry_t* entry = (yfs_dcache_entry_t*)kmalloc(sizeof(yfs_dcache_entry_t));
    if (!entry) return;

    entry->parent_ino = parent;
    strlcpy(entry->name, name, YFS_NAME_MAX);
    entry->target_ino = target;

    spinlock_acquire(&dcache_lock);

    new_node = &(dcache_root.rb_node);
    parent_node = 0;

    while (*new_node) {
        yfs_dcache_entry_t *this_entry = rb_entry(*new_node, yfs_dcache_entry_t, node);
        parent_node = *new_node;
        int cmp = rb_compare(parent, name, this_entry->parent_ino, this_entry->name);
        if (cmp == 0) {
            this_entry->target_ino = target;
            spinlock_release(&dcache_lock);
            kfree(entry);
            return;
        }
        if (cmp < 0) new_node = &((*new_node)->rb_left);
        else new_node = &((*new_node)->rb_right);
    }

    rb_link_node(&entry->node, parent_node, new_node);
    rb_insert_color(&entry->node, &dcache_root);

    spinlock_release(&dcache_lock);
}

static yfs_ino_t dcache_lookup(yfs_ino_t parent, const char* name) {
    spinlock_acquire(&dcache_lock);
    
    struct rb_node *node = dcache_root.rb_node;
    while (node) {
        yfs_dcache_entry_t *entry = rb_entry(node, yfs_dcache_entry_t, node);
        int cmp = rb_compare(parent, name, entry->parent_ino, entry->name);
        if (cmp == 0) {
            yfs_ino_t res = entry->target_ino;
            spinlock_release(&dcache_lock);
            return res;
        }
        if (cmp < 0) node = node->rb_left;
        else node = node->rb_right;
    }
    
    spinlock_release(&dcache_lock);
    return 0;
}

static void rb_free_tree(struct rb_node* node) {
    if (!node) return;
    rb_free_tree(node->rb_left);
    rb_free_tree(node->rb_right);
    yfs_dcache_entry_t *entry = rb_entry(node, yfs_dcache_entry_t, node);
    kfree(entry);
}

static void dcache_invalidate_entry(yfs_ino_t parent, const char* name) {
    spinlock_acquire(&dcache_lock);
    
    struct rb_node *node = dcache_root.rb_node;
    while (node) {
        yfs_dcache_entry_t *entry = rb_entry(node, yfs_dcache_entry_t, node);
        int cmp = rb_compare(parent, name, entry->parent_ino, entry->name);
        if (cmp == 0) {
            entry->target_ino = 0;
            break;
        }
        if (cmp < 0) node = node->rb_left;
        else node = node->rb_right;
    }
    
    spinlock_release(&dcache_lock);
}

static void flush_sb(void) {
    bcache_write(1, (uint8_t*)&sb);
    bcache_flush_block(1);
}

static void flush_bitmap_cache(void) {
    if (bmap_cache_lba != 0 && bmap_cache_dirty) {
        bcache_write(bmap_cache_lba, bmap_cache_data);
        bmap_cache_dirty = 0;
    }
}

static void load_bitmap_block(uint32_t lba) {
    if (bmap_cache_lba == lba) return;
    flush_bitmap_cache();
    bcache_read(lba, bmap_cache_data);
    bmap_cache_lba = lba;
    bmap_cache_dirty = 0;
}

static void zero_block(yfs_blk_t lba) {
    uint8_t* zeroes = kmalloc(YFS_BLOCK_SIZE);
    if (!zeroes) return;
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
                if (absolute_bit < start_bit) continue;
                if (!((val >> bit) & 1)) return absolute_bit;
            }
        }
    }
    return -1;
}

static yfs_blk_t alloc_block(void) {
    if (sb.free_blocks == 0) return 0;

    uint32_t total_map_blocks = (sb.total_blocks + (YFS_BLOCK_SIZE * 8) - 1) / (YFS_BLOCK_SIZE * 8);
    uint32_t start_sector = last_free_blk_hint / (YFS_BLOCK_SIZE * 8);
    uint32_t start_bit    = last_free_blk_hint % (YFS_BLOCK_SIZE * 8);
    
    for (uint32_t i = 0; i < total_map_blocks; i++) {
        uint32_t sector_idx = (start_sector + i) % total_map_blocks;
        uint32_t map_lba = sb.map_block_start + sector_idx;
        
        load_bitmap_block(map_lba);
        
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
    if (lba < sb.data_start) return;
    
    uint32_t idx = lba - sb.data_start;
    uint32_t sector = idx / (YFS_BLOCK_SIZE * 8);
    uint32_t bit    = idx % (YFS_BLOCK_SIZE * 8);
    uint32_t map_lba = sb.map_block_start + sector;

    load_bitmap_block(map_lba);
    
    if (chk_bit(bmap_cache_data, bit)) {
        clr_bit(bmap_cache_data, bit);
        bmap_cache_dirty = 1;
        sb.free_blocks++;
        if (idx < last_free_blk_hint) last_free_blk_hint = idx;
    }
}

static yfs_ino_t alloc_inode(void) {
    if (sb.free_inodes == 0) return 0;

    uint8_t* buf = kmalloc(YFS_BLOCK_SIZE);
    if (!buf) return 0;

    uint32_t total_map_blocks = (sb.total_inodes + (YFS_BLOCK_SIZE * 8) - 1) / (YFS_BLOCK_SIZE * 8);
    uint32_t start_sector = last_free_ino_hint / (YFS_BLOCK_SIZE * 8);
    uint32_t start_bit    = last_free_ino_hint % (YFS_BLOCK_SIZE * 8);

    for (uint32_t i = 0; i < total_map_blocks; i++) {
        uint32_t sector_idx = (start_sector + i) % total_map_blocks;
        
        bcache_read(sb.map_inode_start + sector_idx, buf);
        
        int bit_search_start = (i == 0) ? start_bit : 0;
        int found_bit = find_free_bit_in_block(buf, bit_search_start);
        
        if (found_bit != -1) {
            uint32_t ino = (sector_idx * YFS_BLOCK_SIZE * 8) + found_bit;
            if (ino == 0) continue; 
            if (ino >= sb.total_inodes) break;

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
    if (ino == 0 || ino >= sb.total_inodes) return;

    uint32_t sector = ino / (YFS_BLOCK_SIZE * 8);
    uint32_t bit    = ino % (YFS_BLOCK_SIZE * 8);

    uint8_t* buf = kmalloc(YFS_BLOCK_SIZE);
    if (!buf) return;

    bcache_read(sb.map_inode_start + sector, buf);
    
    if (chk_bit(buf, bit)) {
        clr_bit(buf, bit);
        bcache_write(sb.map_inode_start + sector, buf);
        sb.free_inodes++;
        if (ino < last_free_ino_hint) last_free_ino_hint = ino;
    }
    kfree(buf);
}

static int sync_inode(yfs_ino_t ino, yfs_inode_t* data, int write) {
    if (!data || ino == 0 || ino >= sb.total_inodes) return 0;
    
    if (sb.inode_table_start == 0) return 0;

    uint32_t per_block = YFS_BLOCK_SIZE / sizeof(yfs_inode_t);
    if (per_block == 0) return 0;
    
    uint32_t block_idx = ino / per_block;
    uint32_t offset    = ino % per_block;
    
    if (offset >= per_block) return 0;
    
    if (block_idx > 0xFFFFFFFF - sb.inode_table_start) return 0;
    
    yfs_blk_t lba = sb.inode_table_start + block_idx;

    spinlock_acquire(&inode_table_cache_lock);

    inode_table_cache_stamp++;
    uint32_t stamp = inode_table_cache_stamp;

    int slot = -1;
    for (int i = 0; i < INODE_TABLE_CACHE_SLOTS; i++) {
        if (inode_table_cache[i].valid && inode_table_cache[i].lba == lba) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        uint32_t best_stamp = 0xFFFFFFFF;
        for (int i = 0; i < INODE_TABLE_CACHE_SLOTS; i++) {
            if (!inode_table_cache[i].valid) {
                slot = i;
                break;
            }
            if (inode_table_cache[i].stamp < best_stamp) {
                best_stamp = inode_table_cache[i].stamp;
                slot = i;
            }
        }

        inode_table_cache[slot].lba = lba;
        inode_table_cache[slot].valid = 1;
        inode_table_cache[slot].stamp = stamp;

        if (!bcache_read(lba, inode_table_cache[slot].data)) {
            inode_table_cache[slot].valid = 0;
            spinlock_release(&inode_table_cache_lock);
            return 0;
        }
    } else {
        inode_table_cache[slot].stamp = stamp;
    }

    yfs_inode_t* table = (yfs_inode_t*)inode_table_cache[slot].data;

    if (offset * sizeof(yfs_inode_t) + sizeof(yfs_inode_t) > YFS_BLOCK_SIZE) {
        spinlock_release(&inode_table_cache_lock);
        return 0;
    }

    if (write) {
        memcpy(&table[offset], data, sizeof(yfs_inode_t));
        bcache_write(lba, inode_table_cache[slot].data);
    } else {
        memcpy(data, &table[offset], sizeof(yfs_inode_t));
    }

    spinlock_release(&inode_table_cache_lock);
    return 1;
}

static yfs_blk_t resolve_block(yfs_inode_t* node, uint32_t file_block, int alloc) {
    if (file_block < YFS_DIRECT_PTRS) {
        if (node->direct[file_block] == 0) {
            if (!alloc) return 0;
            node->direct[file_block] = alloc_block();
        }
        return node->direct[file_block];
    }
    file_block -= YFS_DIRECT_PTRS;

    if (file_block < PTRS_PER_BLOCK) {
        if (node->indirect == 0) {
            if (!alloc) return 0;
            node->indirect = alloc_block();
        }
        
        yfs_blk_t* table = kmalloc(YFS_BLOCK_SIZE);
        if (!table) return 0;

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
            if (!alloc) return 0;
            node->doubly_indirect = alloc_block();
        }

        uint32_t l1_idx = file_block / PTRS_PER_BLOCK;
        uint32_t l2_idx = file_block % PTRS_PER_BLOCK;

        yfs_blk_t* l1 = kmalloc(YFS_BLOCK_SIZE);
        if (!l1) return 0;
        bcache_read(node->doubly_indirect, (uint8_t*)l1);

        if (l1[l1_idx] == 0) {
            if (!alloc) { kfree(l1); return 0; }
            l1[l1_idx] = alloc_block();
            bcache_write(node->doubly_indirect, (uint8_t*)l1);
        }
        yfs_blk_t l2_blk = l1[l1_idx];
        kfree(l1);

        yfs_blk_t* l2 = kmalloc(YFS_BLOCK_SIZE);
        if (!l2) return 0;
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
            if (!alloc) return 0;
            node->triply_indirect = alloc_block();
        }

        uint32_t ptrs_sq = PTRS_PER_BLOCK * PTRS_PER_BLOCK;
        uint32_t i1 = file_block / ptrs_sq;
        uint32_t rem = file_block % ptrs_sq;
        uint32_t i2 = rem / PTRS_PER_BLOCK;
        uint32_t i3 = rem % PTRS_PER_BLOCK;

        yfs_blk_t* l1 = kmalloc(YFS_BLOCK_SIZE);
        if (!l1) return 0;
        bcache_read(node->triply_indirect, (uint8_t*)l1);

        if (l1[i1] == 0) {
            if (!alloc) { kfree(l1); return 0; }
            l1[i1] = alloc_block();
            bcache_write(node->triply_indirect, (uint8_t*)l1);
        }
        yfs_blk_t l2_blk = l1[i1];
        kfree(l1);

        yfs_blk_t* l2 = kmalloc(YFS_BLOCK_SIZE);
        if (!l2) return 0;
        bcache_read(l2_blk, (uint8_t*)l2);
        
        if (l2[i2] == 0) {
            if (!alloc) { kfree(l2); return 0; }
            l2[i2] = alloc_block();
            bcache_write(l2_blk, (uint8_t*)l2);
        }
        yfs_blk_t l3_blk = l2[i2];
        kfree(l2);

        yfs_blk_t* l3 = kmalloc(YFS_BLOCK_SIZE);
        if (!l3) return 0;
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
    if (block == 0) return;
    
    if (level > 0) {
        yfs_blk_t* table = kmalloc(YFS_BLOCK_SIZE);
        if (table) {
            bcache_read(block, (uint8_t*)table);
            for (uint32_t i = 0; i < PTRS_PER_BLOCK; i++) {
                if (table[i]) free_indir_level(table[i], level - 1);
            }
            kfree(table);
        }
    }
    free_block(block);
}

static void truncate_inode(yfs_inode_t* node) {
    for (int i = 0; i < YFS_DIRECT_PTRS; i++) {
        if (node->direct[i]) free_block(node->direct[i]);
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

static yfs_ino_t dir_find(yfs_inode_t* dir, const char* name) {
    yfs_ino_t cached = dcache_lookup(dir->id, name);
    if (cached) return cached;

    uint32_t entries_per_block = YFS_BLOCK_SIZE / sizeof(yfs_dirent_t);
    yfs_dirent_t* entries = kmalloc(YFS_BLOCK_SIZE);
    if (!entries) return 0;

    uint32_t blocks = (dir->size + YFS_BLOCK_SIZE - 1) / YFS_BLOCK_SIZE;

    for (uint32_t i = 0; i < blocks; i++) {
        yfs_blk_t lba = resolve_block(dir, i, 0);
        if (!lba) continue;
        
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
    rwlock_t* lock = get_inode_lock(dir_ino);
    rwlock_acquire_write(lock);

    yfs_inode_t dir;
    sync_inode(dir_ino, &dir, 0);

    dcache_insert(dir_ino, name, child_ino);

    uint32_t entries_per_block = YFS_BLOCK_SIZE / sizeof(yfs_dirent_t);
    yfs_dirent_t* entries = kmalloc(YFS_BLOCK_SIZE);
    if (!entries) { rwlock_release_write(lock); return -1; }

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
    rwlock_t* lock = get_inode_lock(dir_ino);
    rwlock_acquire_write(lock);

    yfs_inode_t dir;
    sync_inode(dir_ino, &dir, 0);
    
    dcache_invalidate_entry(dir_ino, name);

    uint32_t entries_per_block = YFS_BLOCK_SIZE / sizeof(yfs_dirent_t);
    yfs_dirent_t* entries = kmalloc(YFS_BLOCK_SIZE);
    if (!entries) { rwlock_release_write(lock); return -1; }

    uint32_t blocks = (dir.size + YFS_BLOCK_SIZE - 1) / YFS_BLOCK_SIZE;

    for (uint32_t i = 0; i < blocks; i++) {
        yfs_blk_t lba = resolve_block(&dir, i, 0);
        if (!lba) continue;

        bcache_read(lba, (uint8_t*)entries);
        for (uint32_t j = 0; j < entries_per_block; j++) {
            if (entries[j].inode != 0 && strcmp(entries[j].name, name) == 0) {
                yfs_ino_t child_id = entries[j].inode;
                yfs_inode_t child;
                sync_inode(child_id, &child, 0);

                truncate_inode(&child);
                free_inode(child_id);

                entries[j].inode = 0;
                bcache_write(lba, (uint8_t*)entries);
                
                flush_bitmap_cache();
                flush_sb();
                
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
    yfs_inode_t dir;
    sync_inode(dir_ino, &dir, 0);
    
    dcache_invalidate_entry(dir_ino, name);

    uint32_t entries_per_block = YFS_BLOCK_SIZE / sizeof(yfs_dirent_t);
    yfs_dirent_t* entries = kmalloc(YFS_BLOCK_SIZE);
    if (!entries) return -1;

    uint32_t blocks = (dir.size + YFS_BLOCK_SIZE - 1) / YFS_BLOCK_SIZE;

    for (uint32_t i = 0; i < blocks; i++) {
        yfs_blk_t lba = resolve_block(&dir, i, 0);
        if (!lba) continue;

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

static yfs_ino_t path_to_inode(const char* path, char* last_element) {
    yfs_ino_t curr = (path[0] == '/') ? 1 : proc_current()->cwd_inode;
    const char* p = (path[0] == '/') ? path + 1 : path;
    
    char buffer[256]; strlcpy(buffer, p, 256);
    char* token = buffer;
    char* next_token = NULL;

    while (*token) {
        char* slash = token;
        while (*slash && *slash != '/') slash++;
        
        if (*slash == '/') {
            *slash = 0;
            next_token = slash + 1;
        } else {
            next_token = NULL;
        }

        if (next_token == NULL) {
            if (last_element) strlcpy(last_element, token, YFS_NAME_MAX);
            return curr;
        }

        yfs_inode_t dir_node;
        sync_inode(curr, &dir_node, 0);
        
        yfs_ino_t next_ino = dir_find(&dir_node, token);
        if (next_ino == 0) return 0;

        curr = next_ino;
        token = next_token;
    }
    return curr;
}

void yulafs_format(uint32_t disk_blocks_4k) {
    uint8_t* zero = kmalloc(YFS_BLOCK_SIZE);
    uint8_t* map  = kmalloc(YFS_BLOCK_SIZE);
    yfs_dirent_t* dots = kmalloc(YFS_BLOCK_SIZE);
    
    if (!zero || !map || !dots) {
        if(zero) kfree(zero);
        if(map) kfree(map);
        if(dots) kfree(dots);
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
    if (sb.total_inodes < 128) sb.total_inodes = 128;

    uint32_t sec_per_map = (sb.total_blocks + (YFS_BLOCK_SIZE*8) - 1) / (YFS_BLOCK_SIZE*8);
    uint32_t sec_per_imap = (sb.total_inodes + (YFS_BLOCK_SIZE*8) - 1) / (YFS_BLOCK_SIZE*8);
    uint32_t sec_inodes  = (sb.total_inodes * sizeof(yfs_inode_t) + YFS_BLOCK_SIZE - 1) / YFS_BLOCK_SIZE;

    sb.map_inode_start   = 2;
    sb.map_block_start   = sb.map_inode_start + sec_per_imap;
    sb.inode_table_start = sb.map_block_start + sec_per_map;
    sb.data_start        = sb.inode_table_start + sec_inodes;

    sb.free_inodes = sb.total_inodes;
    sb.free_blocks = sb.total_blocks - sb.data_start;

    for(uint32_t i=0; i<sec_per_imap; i++) bcache_write(sb.map_inode_start + i, zero);
    for(uint32_t i=0; i<sec_per_map; i++) bcache_write(sb.map_block_start + i, zero);
    for(uint32_t i=0; i<sec_inodes; i++) bcache_write(sb.inode_table_start + i, zero);

    set_bit(map, 0);
    set_bit(map, 1);
    bcache_write(sb.map_inode_start, map);
    sb.free_inodes -= 2;

    yfs_inode_t root; memset(&root, 0, sizeof(root));
    root.id = 1;
    root.type = YFS_TYPE_DIR;
    root.size = YFS_BLOCK_SIZE;
    root.direct[0] = alloc_block();
    
    dots[0].inode = 1; strlcpy(dots[0].name, ".", YFS_NAME_MAX);
    dots[1].inode = 1; strlcpy(dots[1].name, "..", YFS_NAME_MAX);
    bcache_write(root.direct[0], (uint8_t*)dots);

    sync_inode(1, &root, 1);
    flush_sb();
    flush_bitmap_cache();
    bcache_sync();
    
    fs_mounted = 1;
    last_free_blk_hint = 0;
    last_free_ino_hint = 2;

    spinlock_init(&dcache_lock);
    if (dcache_root.rb_node) { rb_free_tree(dcache_root.rb_node); dcache_root = RB_ROOT; }

    yulafs_mkdir("/bin");
    yulafs_mkdir("/home");
    yulafs_mkdir("/dev");
    bcache_sync();
    
    kfree(zero); kfree(map); kfree(dots);
}

void yulafs_init(void) {
    bcache_init();
    spinlock_init(&dcache_lock);
    spinlock_init(&inode_table_cache_lock);
    inode_table_cache_stamp = 0;
    memset(inode_table_cache, 0, sizeof(inode_table_cache));
 
    spinlock_init(&yfs_scratch_lock);
    memset(yfs_scratch_used, 0, sizeof(yfs_scratch_used));

    bmap_cache_lba = 0;
    bmap_cache_dirty = 0;
    dcache_root = RB_ROOT;
    
    uint8_t* buf = kmalloc(YFS_BLOCK_SIZE);
    if (!buf) return;

    if (!bcache_read(1, buf)) {
    } else {
        memcpy(&sb, buf, sizeof(sb));
    }
    kfree(buf);

    for (int i = 0; i < INODE_LOCK_BUCKETS; i++) rwlock_init(&inode_locks[i]);

    if (sb.magic != YFS_MAGIC) {
        uint32_t capacity = ahci_get_capacity();
        if (capacity == 0) capacity = 131072; 
        yulafs_format(capacity / 8); 
    } else {
        fs_mounted = 1;
        last_free_blk_hint = 0;
        last_free_ino_hint = 1;
    }
}

int yulafs_read(yfs_ino_t ino, void* buf, yfs_off_t offset, uint32_t size) {
    if (!fs_mounted || !buf) return -1;
    
    if (size == 0) return 0;
    if (offset > 0xFFFFFFFF - size) return -1;
    
    rwlock_t* lock = get_inode_lock(ino);
    rwlock_acquire_read(lock);

    yfs_inode_t node;
    if(!sync_inode(ino, &node, 0)) { rwlock_release_read(lock); return -1; }

    if (node.size > 0xFFFFFFFF) { rwlock_release_read(lock); return -1; }
    
    if (offset >= node.size) { rwlock_release_read(lock); return 0; }
    
    uint64_t total = (uint64_t)offset + (uint64_t)size;
    if (total > node.size) {
        size = node.size - offset;
    }

    uint32_t read_count = 0;
    int scratch_slot = -1;
    uint8_t* scratch = yfs_scratch_acquire(&scratch_slot);
    int scratch_heap = 0;
    if (!scratch) {
        scratch = kmalloc_a(YFS_BLOCK_SIZE);
        scratch_heap = 1;
    }
    if (!scratch) { rwlock_release_read(lock); return -1; }

    uint32_t last_prefetched_log_blk = 0xFFFFFFFF;
    
    while (read_count < size) {
        uint32_t log_blk = (offset + read_count) / YFS_BLOCK_SIZE;
        uint32_t blk_off = (offset + read_count) % YFS_BLOCK_SIZE;
        uint32_t phys_blk = resolve_block(&node, log_blk, 0);

        uint32_t copy_len = YFS_BLOCK_SIZE - blk_off;
        if (copy_len > size - read_count) copy_len = size - read_count;
        
        if (copy_len > YFS_BLOCK_SIZE || copy_len > size - read_count) {
            if (scratch_heap) kfree(scratch);
            else yfs_scratch_release(scratch_slot);
            rwlock_release_read(lock);
            return -1;
        }

        if (phys_blk) {
            if(!bcache_read(phys_blk, scratch)) { 
                if (scratch_heap) kfree(scratch);
                else yfs_scratch_release(scratch_slot);
                rwlock_release_read(lock); 
                return -1; 
            }
            
            if (read_count + copy_len > size || 
                blk_off + copy_len > YFS_BLOCK_SIZE) {
                if (scratch_heap) kfree(scratch);
                else yfs_scratch_release(scratch_slot);
                rwlock_release_read(lock);
                return -1;
            }
            
            memcpy((uint8_t*)buf + read_count, scratch + blk_off, copy_len);
            
            if (log_blk != last_prefetched_log_blk) {
                uint32_t next_log_blk = log_blk + 1;
                uint32_t next_phys_blk = resolve_block(&node, next_log_blk, 0);
                if (next_phys_blk) {
                    uint32_t blocks_remaining = ((node.size - (offset + read_count)) + YFS_BLOCK_SIZE - 1) / YFS_BLOCK_SIZE;
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

    if (scratch_heap) kfree(scratch);
    else yfs_scratch_release(scratch_slot);
    rwlock_release_read(lock);
    return read_count;
}

static int yulafs_write_locked(yfs_ino_t ino, yfs_inode_t* node, const void* buf, yfs_off_t offset, uint32_t size);

int yulafs_write(yfs_ino_t ino, const void* buf, yfs_off_t offset, uint32_t size) {
    if (!fs_mounted || !buf) return -1;
    
    if (size == 0) return 0;
    if (offset > 0xFFFFFFFF - size) return -1;
    
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

static int yulafs_write_locked(yfs_ino_t ino, yfs_inode_t* node, const void* buf, yfs_off_t offset, uint32_t size) {
    if (!node || !buf) return -1;

    uint32_t written = 0;
    int dirty = 0;
    int blocks_allocated = 0;

    int scratch_slot = -1;
    uint8_t* scratch = yfs_scratch_acquire(&scratch_slot);
    int scratch_heap = 0;
    if (!scratch) {
        scratch = kmalloc(YFS_BLOCK_SIZE);
        scratch_heap = 1;
    }
    if (!scratch) return -1;

    while (written < size) {
        uint32_t log_blk = (offset + written) / YFS_BLOCK_SIZE;
        uint32_t blk_off = (offset + written) % YFS_BLOCK_SIZE;

        yfs_blk_t phys_blk = resolve_block(node, log_blk, 1);
        if (!phys_blk) break;
        blocks_allocated = 1;

        uint32_t copy_len = YFS_BLOCK_SIZE - blk_off;
        if (copy_len > size - written) copy_len = size - written;

        if (copy_len > YFS_BLOCK_SIZE || copy_len > size - written) {
            if (scratch_heap) kfree(scratch);
            else yfs_scratch_release(scratch_slot);
            return written > 0 ? (int)written : -1;
        }

        if (copy_len < YFS_BLOCK_SIZE) {
            if (!bcache_read(phys_blk, scratch)) {
                if (scratch_heap) kfree(scratch);
                else yfs_scratch_release(scratch_slot);
                return written > 0 ? (int)written : -1;
            }
        }

        if (written + copy_len > size || blk_off + copy_len > YFS_BLOCK_SIZE) {
            if (scratch_heap) kfree(scratch);
            else yfs_scratch_release(scratch_slot);
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

    if (dirty) sync_inode(ino, node, 1);
    if (blocks_allocated) {
        flush_bitmap_cache();
        flush_sb();
    }

    if (scratch_heap) kfree(scratch);
    else yfs_scratch_release(scratch_slot);

    return (int)written;
}

int yulafs_append(yfs_ino_t ino, const void* buf, uint32_t size, yfs_off_t* out_start_off) {
    if (!fs_mounted || !buf) return -1;
    if (size == 0) {
        if (out_start_off) *out_start_off = 0;
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
    if (out_start_off) *out_start_off = start;

    int rc = yulafs_write_locked(ino, &node, buf, start, size);
    rwlock_release_write(lock);
    return rc;
}

int yulafs_create_obj(const char* path, int type) {
    char name[YFS_NAME_MAX];
    yfs_ino_t dir_ino = path_to_inode(path, name);
    if (!dir_ino) return -1;

    yfs_inode_t dir; sync_inode(dir_ino, &dir, 0);
    if (dir_find(&dir, name) != 0) return -1;

    yfs_ino_t new_ino = alloc_inode();
    if (!new_ino) return -1;

    yfs_inode_t obj; memset(&obj, 0, sizeof(obj));
    obj.id = new_ino;
    obj.type = type;
    obj.size = 0;

    if (type == YFS_TYPE_DIR) {
        obj.size = YFS_BLOCK_SIZE;
        obj.direct[0] = alloc_block();
        
        uint8_t* buf = kmalloc(YFS_BLOCK_SIZE);
        if (buf) {
            memset(buf, 0, YFS_BLOCK_SIZE);
            yfs_dirent_t* dots = (yfs_dirent_t*)buf;
            dots[0].inode = new_ino; strlcpy(dots[0].name, ".", YFS_NAME_MAX);
            dots[1].inode = dir_ino; strlcpy(dots[1].name, "..", YFS_NAME_MAX);
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

int yulafs_mkdir(const char* path) { return yulafs_create_obj(path, YFS_TYPE_DIR); }
int yulafs_create(const char* path) { return yulafs_create_obj(path, YFS_TYPE_FILE); }
int yulafs_unlink(const char* path) {
    char name[YFS_NAME_MAX];
    yfs_ino_t dir_ino = path_to_inode(path, name);
    if (!dir_ino) return -1;
    return dir_unlink(dir_ino, name);
}

int yulafs_lookup(const char* path) {
    if (!path || !*path) return (int)proc_current()->cwd_inode;
    if (strcmp(path, "/") == 0) return 1;
    char name[YFS_NAME_MAX];
    yfs_ino_t parent_dir = path_to_inode(path, name);
    if (parent_dir == 0) return -1;
    yfs_inode_t dir_node;
    sync_inode(parent_dir, &dir_node, 0);
    yfs_ino_t target = dir_find(&dir_node, name);
    if (target == 0) return -1;
    return (int)target;
}

int yulafs_lookup_in_dir(yfs_ino_t dir_ino, const char* name) {
    if (!name || !*name) return -1;

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

    return ino ? (int)ino : -1;
}

int yulafs_getdents(yfs_ino_t dir_ino, uint32_t* inout_offset, yfs_dirent_info_t* out, uint32_t out_size) {
    if (!inout_offset || !out) return -1;
    if (out_size < sizeof(yfs_dirent_info_t)) return -1;

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
    if (((*inout_offset) % (uint32_t)sizeof(yfs_dirent_t)) != 0) idx++;

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

        for (; ent_idx < entries_per_block && idx < total_entries && out_count < max_entries; ent_idx++, idx++) {
            if (ents[ent_idx].inode == 0) continue;

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

    if (scratch_heap) kfree(scratch_heap);
    else yfs_scratch_release(scratch_slot);

    rwlock_release_read(lock);
    return (int)(out_count * (uint32_t)sizeof(yfs_dirent_info_t));
}

int yulafs_stat(yfs_ino_t ino, yfs_inode_t* out) {
    sync_inode(ino, out, 0);
    return 0;
}

void yulafs_resize(yfs_ino_t ino, uint32_t new_size) {
    rwlock_t* lock = get_inode_lock(ino);
    rwlock_acquire_write(lock);
    yfs_inode_t node; sync_inode(ino, &node, 0);
    if (new_size < node.size && new_size == 0) { truncate_inode(&node); flush_bitmap_cache(); flush_sb(); }
    node.size = new_size;
    sync_inode(ino, &node, 1);
    rwlock_release_write(lock);
}

void yulafs_get_filesystem_info(uint32_t* total, uint32_t* free, uint32_t* size) {
    if (fs_mounted) { *total=sb.total_blocks; *free=sb.free_blocks; *size=sb.block_size; }
    else { *total=0; *free=0; *size=0; }
}

int yulafs_rename(const char* old_path, const char* new_path) {
    char old_name[YFS_NAME_MAX], new_name[YFS_NAME_MAX];
    yfs_ino_t old_dir = path_to_inode(old_path, old_name);
    if (!old_dir) return -1;
    yfs_inode_t old_node; sync_inode(old_dir, &old_node, 0);
    yfs_ino_t target = dir_find(&old_node, old_name);
    if (!target) return -1;
    yfs_ino_t new_dir = path_to_inode(new_path, new_name);
    if (!new_dir) return -1;
    if (dir_link(new_dir, target, new_name) != 0) return -1;
    dir_unlink_entry_only(old_dir, old_name);
    flush_bitmap_cache(); flush_sb();
    return 0;
}