#include "yulafs.h"
#include "bcache.h" 
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../kernel/proc.h"
#include "../drivers/vga.h"
#include <hal/lock.h>

#define PTRS_PER_BLOCK      (YFS_BLOCK_SIZE / 4) 
#define INODE_LOCK_BUCKETS  128
#define DCACHE_SIZE         32  

static yfs_superblock_t sb;
static int fs_mounted = 0;

static uint32_t last_free_blk_hint = 0;
static uint32_t last_free_ino_hint = 1;

static rwlock_t inode_locks[INODE_LOCK_BUCKETS];

typedef struct {
    yfs_ino_t parent;
    char name[YFS_NAME_MAX];
    yfs_ino_t target;
    int valid;
} dcache_entry_t;

static dcache_entry_t dcache[DCACHE_SIZE];
static int dcache_ptr = 0;
static spinlock_t dcache_lock;

static uint8_t  bmap_cache_data[YFS_BLOCK_SIZE];
static uint32_t bmap_cache_lba = 0; 
static int      bmap_cache_dirty = 0;

extern uint32_t ahci_get_capacity(void);

static inline rwlock_t* get_inode_lock(yfs_ino_t ino) {
    return &inode_locks[ino % INODE_LOCK_BUCKETS];
}

static inline void set_bit(uint8_t* map, int i) { map[i/8] |= (1 << (i%8)); }
static inline void clr_bit(uint8_t* map, int i) { map[i/8] &= ~(1 << (i%8)); }
static inline int  chk_bit(uint8_t* map, int i) { return (map[i/8] & (1 << (i%8))); }

static void dcache_put(yfs_ino_t parent, const char* name, yfs_ino_t target) {
    uint32_t flags = spinlock_acquire_safe(&dcache_lock);
    dcache[dcache_ptr].parent = parent;
    strlcpy(dcache[dcache_ptr].name, name, YFS_NAME_MAX);
    dcache[dcache_ptr].target = target;
    dcache[dcache_ptr].valid = 1;
    dcache_ptr = (dcache_ptr + 1) % DCACHE_SIZE;
    spinlock_release_safe(&dcache_lock, flags);
}

static yfs_ino_t dcache_get(yfs_ino_t parent, const char* name) {
    uint32_t flags = spinlock_acquire_safe(&dcache_lock);
    yfs_ino_t res = 0;
    for (int i = 0; i < DCACHE_SIZE; i++) {
        if (dcache[i].valid && dcache[i].parent == parent && strcmp(dcache[i].name, name) == 0) {
            res = dcache[i].target;
            break;
        }
    }
    spinlock_release_safe(&dcache_lock, flags);
    return res;
}

static void dcache_invalidate(yfs_ino_t parent) {
    uint32_t flags = spinlock_acquire_safe(&dcache_lock);
    for (int i = 0; i < DCACHE_SIZE; i++) {
        if (dcache[i].valid && dcache[i].parent == parent) {
            dcache[i].valid = 0;
        }
    }
    spinlock_release_safe(&dcache_lock, flags);
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
    if (ino >= sb.total_inodes) return 0;

    uint32_t per_block = YFS_BLOCK_SIZE / sizeof(yfs_inode_t);
    uint32_t block_idx = ino / per_block;
    uint32_t offset    = ino % per_block;
    
    yfs_blk_t lba = sb.inode_table_start + block_idx;
    
    uint8_t* buf = kmalloc(YFS_BLOCK_SIZE);
    if (!buf) return 0;
    
    if (!bcache_read(lba, buf)) {
        kfree(buf);
        return 0;
    }
    
    yfs_inode_t* table = (yfs_inode_t*)buf;
    
    if (write) {
        memcpy(&table[offset], data, sizeof(yfs_inode_t));
        bcache_write(lba, buf);
    } else {
        memcpy(data, &table[offset], sizeof(yfs_inode_t));
    }
    
    kfree(buf);
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
    yfs_ino_t cached = dcache_get(dir->id, name);
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
                dcache_put(dir->id, name, entries[j].inode);
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
    dcache_invalidate(dir_ino);

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
    dcache_invalidate(dir_ino);

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
    dcache_invalidate(dir_ino);

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
        if(zero) kfree(zero); if(map) kfree(map); if(dots) kfree(dots);
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

    yulafs_mkdir("/bin");
    yulafs_mkdir("/home");
    yulafs_mkdir("/dev");
    bcache_sync();
    
    kfree(zero); kfree(map); kfree(dots);
}

void yulafs_init(void) {
    bcache_init();
    spinlock_init(&dcache_lock);
    bmap_cache_lba = 0;
    bmap_cache_dirty = 0;
    
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
    if (!fs_mounted) return -1;
    rwlock_t* lock = get_inode_lock(ino);
    rwlock_acquire_read(lock);

    yfs_inode_t node;
    if(!sync_inode(ino, &node, 0)) { rwlock_release_read(lock); return -1; }

    if (offset >= node.size) { rwlock_release_read(lock); return 0; }
    if (offset + size > node.size) size = node.size - offset;

    uint32_t read_count = 0;
    
    uint8_t* scratch = kmalloc(YFS_BLOCK_SIZE);
    if (!scratch) { rwlock_release_read(lock); return -1; }

    while (read_count < size) {
        uint32_t log_blk = (offset + read_count) / YFS_BLOCK_SIZE;
        uint32_t blk_off = (offset + read_count) % YFS_BLOCK_SIZE;
        uint32_t phys_blk = resolve_block(&node, log_blk, 0);

        uint32_t copy_len = YFS_BLOCK_SIZE - blk_off;
        if (copy_len > size - read_count) copy_len = size - read_count;

        if (phys_blk) {
            if(!bcache_read(phys_blk, scratch)) { 
                kfree(scratch);
                rwlock_release_read(lock); 
                return -1; 
            }
            memcpy((uint8_t*)buf + read_count, scratch + blk_off, copy_len);
        } else {
            memset((uint8_t*)buf + read_count, 0, copy_len);
        }
        read_count += copy_len;
    }
    
    kfree(scratch);
    rwlock_release_read(lock);
    return read_count;
}

int yulafs_write(yfs_ino_t ino, const void* buf, yfs_off_t offset, uint32_t size) {
    if (!fs_mounted) return -1;
    rwlock_t* lock = get_inode_lock(ino);
    rwlock_acquire_write(lock);

    yfs_inode_t node; 
    sync_inode(ino, &node, 0);

    uint32_t written = 0;
    int dirty = 0;
    int blocks_allocated = 0;

    uint8_t* scratch = kmalloc(YFS_BLOCK_SIZE);
    if (!scratch) { rwlock_release_write(lock); return -1; }

    while (written < size) {
        uint32_t log_blk = (offset + written) / YFS_BLOCK_SIZE;
        uint32_t blk_off = (offset + written) % YFS_BLOCK_SIZE;
        
        yfs_blk_t phys_blk = resolve_block(&node, log_blk, 1);
        if (!phys_blk) break; 
        blocks_allocated = 1;

        uint32_t copy_len = YFS_BLOCK_SIZE - blk_off;
        if (copy_len > size - written) copy_len = size - written;

        if (copy_len < YFS_BLOCK_SIZE) {
            bcache_read(phys_blk, scratch);
        }
        memcpy(scratch + blk_off, (uint8_t*)buf + written, copy_len);
        bcache_write(phys_blk, scratch);
        
        written += copy_len;
        dirty = 1;
    }

    if (offset + written > node.size) {
        node.size = offset + written;
        dirty = 1;
    }

    if (dirty) sync_inode(ino, &node, 1);
    if (blocks_allocated) { flush_bitmap_cache(); flush_sb(); }
    
    kfree(scratch);
    rwlock_release_write(lock);
    return written;
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