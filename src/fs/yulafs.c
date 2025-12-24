#include "yulafs.h"
#include "../drivers/ahci.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../kernel/proc.h"
#include "../drivers/vga.h"

/* --- Globals & Helpers --- */

static yfs_superblock_t sb;
static int fs_mounted = 0;

static inline void set_bit(uint8_t* map, int i) { map[i/8] |= (1 << (i%8)); }
static inline void clr_bit(uint8_t* map, int i) { map[i/8] &= ~(1 << (i%8)); }
static inline int  chk_bit(uint8_t* map, int i) { return (map[i/8] & (1 << (i%8))); }

/* --- Low Level Storage Access --- */

static void flush_sb(void) {
    ahci_write_sector(10, (uint8_t*)&sb);
}

static void zero_block(yfs_blk_t lba) {
    uint8_t zeroes[YFS_BLOCK_SIZE];
    memset(zeroes, 0, YFS_BLOCK_SIZE);
    ahci_write_sector(lba, zeroes);
}

/* --- Bitmap Allocator --- */

static yfs_blk_t alloc_block(void) {
    if (sb.free_blocks == 0) return 0;

    uint8_t buf[YFS_BLOCK_SIZE];
    uint32_t sectors = (sb.total_blocks + 4095) / 4096;

    for (uint32_t i = 0; i < sectors; i++) {
        ahci_read_sector(sb.map_block_start + i, buf);
        for (int j = 0; j < YFS_BLOCK_SIZE * 8; j++) {
            if (!chk_bit(buf, j)) {
                set_bit(buf, j);
                ahci_write_sector(sb.map_block_start + i, buf);
                
                sb.free_blocks--;
                flush_sb();

                yfs_blk_t lba = sb.data_start + (i * YFS_BLOCK_SIZE * 8) + j;
                zero_block(lba);
                return lba;
            }
        }
    }
    return 0;
}

static void free_block(yfs_blk_t lba) {
    if (lba < sb.data_start) return;
    
    uint32_t idx = lba - sb.data_start;
    uint32_t sector = idx / (YFS_BLOCK_SIZE * 8);
    uint32_t bit    = idx % (YFS_BLOCK_SIZE * 8);

    uint8_t buf[YFS_BLOCK_SIZE];
    ahci_read_sector(sb.map_block_start + sector, buf);
    
    if (chk_bit(buf, bit)) {
        clr_bit(buf, bit);
        ahci_write_sector(sb.map_block_start + sector, buf);
        sb.free_blocks++;
        flush_sb();
    }
}

static yfs_ino_t alloc_inode(void) {
    if (sb.free_inodes == 0) return 0;

    uint8_t buf[YFS_BLOCK_SIZE];
    ahci_read_sector(sb.map_inode_start, buf);

    for (yfs_ino_t i = 1; i < sb.total_inodes; i++) {
        if (!chk_bit(buf, i)) {
            set_bit(buf, i);
            ahci_write_sector(sb.map_inode_start, buf);
            sb.free_inodes--;
            flush_sb();
            return i;
        }
    }
    return 0;
}

static void free_inode(yfs_ino_t ino) {
    uint8_t buf[YFS_BLOCK_SIZE];
    ahci_read_sector(sb.map_inode_start, buf);
    clr_bit(buf, ino);
    ahci_write_sector(sb.map_inode_start, buf);
    sb.free_inodes++;
    flush_sb();
}

/* --- Inode Management --- */

static void sync_inode(yfs_ino_t ino, yfs_inode_t* data, int write) {
    uint32_t per_block = YFS_BLOCK_SIZE / sizeof(yfs_inode_t);
    uint32_t block_idx = ino / per_block;
    uint32_t offset    = ino % per_block;
    
    yfs_blk_t lba = sb.inode_table_start + block_idx;
    uint8_t buf[YFS_BLOCK_SIZE];
    
    ahci_read_sector(lba, buf);
    yfs_inode_t* table = (yfs_inode_t*)buf;
    
    if (write) {
        memcpy(&table[offset], data, sizeof(yfs_inode_t));
        ahci_write_sector(lba, buf);
    } else {
        memcpy(data, &table[offset], sizeof(yfs_inode_t));
    }
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

    if (file_block < YFS_PTRS_PER_BLOCK) {
        if (node->indirect == 0) {
            if (!alloc) return 0;
            node->indirect = alloc_block();
        }
        
        yfs_blk_t table[YFS_PTRS_PER_BLOCK];
        ahci_read_sector(node->indirect, (uint8_t*)table);
        
        if (table[file_block] == 0) {
            if (!alloc) return 0;
            table[file_block] = alloc_block();
            ahci_write_sector(node->indirect, (uint8_t*)table);
        }
        return table[file_block];
    }
    file_block -= YFS_PTRS_PER_BLOCK;

    if (file_block < (YFS_PTRS_PER_BLOCK * YFS_PTRS_PER_BLOCK)) {
        if (node->doubly_indirect == 0) {
            if (!alloc) return 0;
            node->doubly_indirect = alloc_block();
        }

        yfs_blk_t l1_table[YFS_PTRS_PER_BLOCK];
        ahci_read_sector(node->doubly_indirect, (uint8_t*)l1_table);

        uint32_t l1_idx = file_block / YFS_PTRS_PER_BLOCK;
        uint32_t l2_idx = file_block % YFS_PTRS_PER_BLOCK;

        if (l1_table[l1_idx] == 0) {
            if (!alloc) return 0;
            l1_table[l1_idx] = alloc_block();
            ahci_write_sector(node->doubly_indirect, (uint8_t*)l1_table);
        }

        yfs_blk_t l2_table[YFS_PTRS_PER_BLOCK];
        ahci_read_sector(l1_table[l1_idx], (uint8_t*)l2_table);

        if (l2_table[l2_idx] == 0) {
            if (!alloc) return 0;
            l2_table[l2_idx] = alloc_block();
            ahci_write_sector(l1_table[l1_idx], (uint8_t*)l2_table);
        }
        return l2_table[l2_idx];
    }

    return 0;
}

static void free_indir_level(yfs_blk_t block, int level) {
    if (block == 0) return;
    
    if (level > 0) {
        yfs_blk_t table[YFS_PTRS_PER_BLOCK];
        ahci_read_sector(block, (uint8_t*)table);
        for (uint32_t i = 0; i < YFS_PTRS_PER_BLOCK; i++) {
            if (table[i]) free_indir_level(table[i], level - 1);
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
    node->size = 0;
}


static yfs_ino_t dir_find(yfs_inode_t* dir, const char* name) {
    yfs_dirent_t entries[YFS_BLOCK_SIZE / sizeof(yfs_dirent_t)];
    uint32_t count = sizeof(entries) / sizeof(yfs_dirent_t);
    uint32_t blocks = (dir->size + YFS_BLOCK_SIZE - 1) / YFS_BLOCK_SIZE;

    for (uint32_t i = 0; i < blocks; i++) {
        yfs_blk_t lba = resolve_block(dir, i, 0);
        if (!lba) continue;
        
        ahci_read_sector(lba, (uint8_t*)entries);
        for (uint32_t j = 0; j < count; j++) {
            if (entries[j].inode != 0 && strcmp(entries[j].name, name) == 0) {
                return entries[j].inode;
            }
        }
    }
    return 0;
}

static int dir_link(yfs_ino_t dir_ino, yfs_ino_t child_ino, const char* name) {
    yfs_inode_t dir;
    sync_inode(dir_ino, &dir, 0);

    yfs_dirent_t entries[YFS_BLOCK_SIZE / sizeof(yfs_dirent_t)];
    uint32_t count = sizeof(entries) / sizeof(yfs_dirent_t);
    uint32_t blk_idx = 0;

    while (1) {
        yfs_blk_t lba = resolve_block(&dir, blk_idx, 1);
        if (!lba) return -1;

        ahci_read_sector(lba, (uint8_t*)entries);
        
        for (uint32_t i = 0; i < count; i++) {
            if (entries[i].inode == 0) {
                entries[i].inode = child_ino;
                strlcpy(entries[i].name, name, YFS_NAME_MAX);
                ahci_write_sector(lba, (uint8_t*)entries);
                
                uint32_t min_size = (blk_idx + 1) * YFS_BLOCK_SIZE;
                if (dir.size < min_size) {
                    dir.size = min_size;
                    sync_inode(dir_ino, &dir, 1);
                }
                return 0;
            }
        }
        blk_idx++;
    }
}

static int dir_unlink(yfs_ino_t dir_ino, const char* name) {
    yfs_inode_t dir;
    sync_inode(dir_ino, &dir, 0);

    yfs_dirent_t entries[YFS_BLOCK_SIZE / sizeof(yfs_dirent_t)];
    uint32_t count = sizeof(entries) / sizeof(yfs_dirent_t);
    uint32_t blocks = (dir.size + YFS_BLOCK_SIZE - 1) / YFS_BLOCK_SIZE;

    for (uint32_t i = 0; i < blocks; i++) {
        yfs_blk_t lba = resolve_block(&dir, i, 0);
        if (!lba) continue;

        ahci_read_sector(lba, (uint8_t*)entries);
        for (uint32_t j = 0; j < count; j++) {
            if (entries[j].inode != 0 && strcmp(entries[j].name, name) == 0) {
                yfs_ino_t child_id = entries[j].inode;
                yfs_inode_t child;
                sync_inode(child_id, &child, 0);

                if (child.type == YFS_TYPE_DIR) {
                }

                truncate_inode(&child);
                free_inode(child_id);

                entries[j].inode = 0;
                ahci_write_sector(lba, (uint8_t*)entries);
                return 0;
            }
        }
    }
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


void yulafs_format(uint32_t disk_sectors) {
    memset(&sb, 0, sizeof(sb));
    sb.magic = YFS_MAGIC;
    sb.version = YFS_VERSION;
    sb.block_size = YFS_BLOCK_SIZE;
    sb.total_blocks = disk_sectors;
    sb.total_inodes = 4096;

    uint32_t sec_per_map = (sb.total_blocks + 4095) / 4096;
    uint32_t sec_per_imap = (sb.total_inodes + 4095) / 4096;
    uint32_t sec_inodes  = (sb.total_inodes * sizeof(yfs_inode_t) + 511) / 512;

    sb.map_inode_start   = 11;
    sb.map_block_start   = sb.map_inode_start + sec_per_imap;
    sb.inode_table_start = sb.map_block_start + sec_per_map;
    sb.data_start        = sb.inode_table_start + sec_inodes;

    sb.free_inodes = sb.total_inodes;
    sb.free_blocks = sb.total_blocks - sb.data_start;

    uint8_t zero[512] = {0};
    ahci_write_sector(sb.map_inode_start, zero);
    ahci_write_sector(sb.map_block_start, zero);

    uint8_t map[512] = {0};
    set_bit(map, 0);
    set_bit(map, 1);
    ahci_write_sector(sb.map_inode_start, map);
    sb.free_inodes -= 2;

    yfs_inode_t root; memset(&root, 0, sizeof(root));
    root.id = 1;
    root.type = YFS_TYPE_DIR;
    root.size = YFS_BLOCK_SIZE;
    root.direct[0] = alloc_block();
    
    yfs_dirent_t dots[2]; memset(dots, 0, sizeof(dots));
    dots[0].inode = 1; strlcpy(dots[0].name, ".", YFS_NAME_MAX);
    dots[1].inode = 1; strlcpy(dots[1].name, "..", YFS_NAME_MAX);
    ahci_write_sector(root.direct[0], (uint8_t*)dots);

    sync_inode(1, &root, 1);
    flush_sb();
    fs_mounted = 1;
}

void yulafs_init(void) {
    uint8_t buf[YFS_BLOCK_SIZE];
    ahci_read_sector(10, buf);
    memcpy(&sb, buf, sizeof(sb));

    if (sb.magic != YFS_MAGIC) {
        yulafs_format(20480); 
    } else {
        fs_mounted = 1;
    }
}

int yulafs_read(yfs_ino_t ino, void* buf, yfs_off_t offset, uint32_t size) {
    if (!fs_mounted) return -1;
    yfs_inode_t node; sync_inode(ino, &node, 0);

    if (offset >= node.size) return 0;
    if (offset + size > node.size) size = node.size - offset;

    uint32_t read = 0;
    uint8_t scratch[YFS_BLOCK_SIZE];

    while (read < size) {
        uint32_t log_blk = (offset + read) / YFS_BLOCK_SIZE;
        uint32_t blk_off = (offset + read) % YFS_BLOCK_SIZE;
        uint32_t phys_blk = resolve_block(&node, log_blk, 0);

        if (phys_blk) ahci_read_sector(phys_blk, scratch);
        else memset(scratch, 0, YFS_BLOCK_SIZE);

        uint32_t copy = YFS_BLOCK_SIZE - blk_off;
        if (copy > size - read) copy = size - read;

        memcpy((uint8_t*)buf + read, scratch + blk_off, copy);
        read += copy;
    }
    return read;
}

int yulafs_write(yfs_ino_t ino, const void* buf, yfs_off_t offset, uint32_t size) {
    if (!fs_mounted) return -1;
    yfs_inode_t node; sync_inode(ino, &node, 0);

    uint32_t written = 0;
    uint8_t scratch[YFS_BLOCK_SIZE];
    int dirty = 0;

    while (written < size) {
        uint32_t log_blk = (offset + written) / YFS_BLOCK_SIZE;
        uint32_t blk_off = (offset + written) % YFS_BLOCK_SIZE;
        
        yfs_blk_t phys_blk = resolve_block(&node, log_blk, 1);
        if (!phys_blk) break; 
        
        if (blk_off > 0 || (size - written) < YFS_BLOCK_SIZE) {
            ahci_read_sector(phys_blk, scratch);
        }

        uint32_t copy = YFS_BLOCK_SIZE - blk_off;
        if (copy > size - written) copy = size - written;

        memcpy(scratch + blk_off, (uint8_t*)buf + written, copy);
        ahci_write_sector(phys_blk, scratch);
        
        written += copy;
        dirty = 1;
    }

    if (offset + written > node.size) {
        node.size = offset + written;
        dirty = 1;
    }

    if (dirty) sync_inode(ino, &node, 1);
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
        
        uint8_t buf[YFS_BLOCK_SIZE];
        memset(buf, 0, YFS_BLOCK_SIZE);
        
        yfs_dirent_t* dots = (yfs_dirent_t*)buf;
        
        dots[0].inode = new_ino; 
        strlcpy(dots[0].name, ".", YFS_NAME_MAX);
        
        dots[1].inode = dir_ino; 
        strlcpy(dots[1].name, "..", YFS_NAME_MAX);
        
        ahci_write_sector(obj.direct[0], buf);
    }

    sync_inode(new_ino, &obj, 1);
    dir_link(dir_ino, new_ino, name);
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
    yfs_inode_t node; sync_inode(ino, &node, 0);
    node.size = new_size;
    sync_inode(ino, &node, 1);
}