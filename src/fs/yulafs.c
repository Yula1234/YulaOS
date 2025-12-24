#include "yulafs.h"
#include "../drivers/ata.h"
#include "../lib/string.h"
#include "../drivers/vga.h"
#include "../mm/heap.h"
#include "../kernel/proc.h"

static uint8_t inode_map[BLOCK_SIZE];
static uint8_t block_map[BLOCK_SIZE];

static int bitmap_get(uint8_t* map, int bit) { return (map[bit / 8] & (1 << (bit % 8))) != 0; }
static void bitmap_set(uint8_t* map, int bit) { map[bit / 8] |= (1 << (bit % 8)); }
static void bitmap_clear(uint8_t* map, int bit) { map[bit / 8] &= ~(1 << (bit % 8)); }
static void sync_metadata() { ata_write_sector(INODE_MAP_LBA, inode_map); ata_write_sector(BLOCK_MAP_LBA, block_map); }

static int bitmap_alloc(uint8_t* map) {
    for (int i = 0; i < BLOCK_SIZE * 8; i++) {
        if (!bitmap_get(map, i)) { bitmap_set(map, i); return i; }
    }
    return -1;
}

static void read_inode(uint32_t idx, yfs_inode_t* inode) {
    uint8_t buf[BLOCK_SIZE];
    uint32_t lba = INODE_TABLE_LBA + (idx * 256) / BLOCK_SIZE;
    uint32_t off = (idx * 256) % BLOCK_SIZE;
    ata_read_sector(lba, buf);
    memcpy(inode, buf + off, sizeof(yfs_inode_t));
}

static void write_inode(uint32_t idx, yfs_inode_t* inode) {
    uint8_t buf[BLOCK_SIZE];
    uint32_t lba = INODE_TABLE_LBA + (idx * 256) / BLOCK_SIZE;
    uint32_t off = (idx * 256) % BLOCK_SIZE;
    ata_read_sector(lba, buf);
    memcpy(buf + off, inode, sizeof(yfs_inode_t));
    ata_write_sector(lba, buf);
}

static uint32_t get_real_block(yfs_inode_t* inode, uint32_t logical_blk, int allocate) {
    if (logical_blk < INODE_DIRECT_COUNT) {
        if (inode->blocks[logical_blk] == 0 && allocate) {
            int b = bitmap_alloc(block_map);
            if (b == -1) return 0;
            inode->blocks[logical_blk] = DATA_START_LBA + b;
        }
        return inode->blocks[logical_blk];
    }
    
    uint32_t indirect_idx = logical_blk - INODE_DIRECT_COUNT;
    
    if (indirect_idx >= 128) return 0;

    if (inode->blocks[INDIRECT_BLOCK_IDX] == 0) {
        if (!allocate) return 0;
        int b = bitmap_alloc(block_map);
        if (b == -1) return 0;
        inode->blocks[INDIRECT_BLOCK_IDX] = DATA_START_LBA + b;
        
        uint8_t zeros[512]; memset(zeros, 0, 512);
        ata_write_sector(inode->blocks[INDIRECT_BLOCK_IDX], zeros);
    }

    uint32_t indices[128];
    ata_read_sector(inode->blocks[INDIRECT_BLOCK_IDX], (uint8_t*)indices);

    if (indices[indirect_idx] == 0 && allocate) {
        int b = bitmap_alloc(block_map);
        if (b == -1) return 0;
        indices[indirect_idx] = DATA_START_LBA + b;
        ata_write_sector(inode->blocks[INDIRECT_BLOCK_IDX], (uint8_t*)indices);
    }

    return indices[indirect_idx];
}

void yulafs_format() {
    memset(inode_map, 0, BLOCK_SIZE);
    memset(block_map, 0, BLOCK_SIZE);
    bitmap_set(inode_map, 0); bitmap_set(inode_map, 1);

    int b = bitmap_alloc(block_map);
    yfs_inode_t root = { .type = YFS_TYPE_DIR, .size = 0 };
    root.blocks[0] = DATA_START_LBA + b;
    write_inode(1, &root);

    uint8_t buffer[BLOCK_SIZE]; memset(buffer, 0, BLOCK_SIZE);
    yfs_dir_entry_t* ent = (yfs_dir_entry_t*)buffer;
    ent[0].inode_idx = 1; strlcpy(ent[0].name, ".", 28);
    ent[1].inode_idx = 1; strlcpy(ent[1].name, "..", 28);
    ata_write_sector(root.blocks[0], buffer);

    yfs_superblock_t sb = { YFS_MAGIC, MAX_INODES, 4096 };
    ata_write_sector(SB_LBA, (uint8_t*)&sb);
    sync_metadata();
}

void yulafs_init() {
    uint8_t buf[BLOCK_SIZE];
    ata_read_sector(SB_LBA, buf);
    yfs_superblock_t* sb = (yfs_superblock_t*)buf;
    if (sb->magic != YFS_MAGIC) yulafs_format();
    else {
        ata_read_sector(INODE_MAP_LBA, inode_map);
        ata_read_sector(BLOCK_MAP_LBA, block_map);
    }
}

static int find_in_dir(uint32_t dir_idx, const char* name) {
    yfs_inode_t dir; read_inode(dir_idx, &dir);
    if (dir.type != YFS_TYPE_DIR) return -1;
    yfs_dir_entry_t entries[16]; 
    ata_read_sector(dir.blocks[0], (uint8_t*)entries);
    for (int i = 0; i < 16; i++) {
        if (entries[i].inode_idx > 0 && strcmp(entries[i].name, name) == 0) return (int)entries[i].inode_idx;
    }
    return -1;
}

int yulafs_lookup(const char* path) {
    if (!path || path[0] == '\0') return (int)proc_current()->cwd_inode;
    uint32_t curr = (path[0] == '/') ? 1 : proc_current()->cwd_inode;
    const char* p = (path[0] == '/') ? path + 1 : path;
    if (*p == '\0') return (int)curr;
    char buf[256]; strlcpy(buf, p, 256);
    char* token = buf; char* next;
    while (token && *token) {
        next = token; while (*next && *next != '/') next++;
        if (*next == '/') { *next = '\0'; next++; } else next = NULL;
        int found = find_in_dir(curr, token);
        if (found == -1) return -1;
        curr = (uint32_t)found;
        token = next;
    }
    return (int)curr;
}

static int add_entry_to_dir(uint32_t dir_idx, const char* name, uint32_t inode_idx) {
    yfs_inode_t dir; read_inode(dir_idx, &dir);
    yfs_dir_entry_t entries[16];
    ata_read_sector(dir.blocks[0], (uint8_t*)entries);
    for (int i = 0; i < 16; i++) {
        if (entries[i].inode_idx == 0) {
            memset(&entries[i], 0, sizeof(yfs_dir_entry_t));
            entries[i].inode_idx = inode_idx;
            strlcpy(entries[i].name, name, 28);
            ata_write_sector(dir.blocks[0], (uint8_t*)entries);
            return 0;
        }
    }
    return -1;
}

static int yfs_create_internal(const char* path, yfs_type_t type) {
    char name[32]; uint32_t parent_idx = proc_current()->cwd_inode;
    const char* last_s = 0;
    for(const char* p = path; *p; p++) if(*p == '/') last_s = p;
    if (last_s) {
        if (last_s == path) parent_idx = 1; 
        else {
            char parent_p[256]; int len = last_s - path;
            memcpy(parent_p, path, len); parent_p[len] = '\0';
            int pi = yulafs_lookup(parent_p);
            if (pi == -1) return -1;
            parent_idx = pi;
        }
        strlcpy(name, last_s + 1, 32);
    } else strlcpy(name, path, 32);

    int new_idx = bitmap_alloc(inode_map);
    if (new_idx == -1) return -1;

    yfs_inode_t ni; memset(&ni, 0, sizeof(yfs_inode_t));
    ni.type = (uint32_t)type; ni.size = 0;
    if (type == YFS_TYPE_DIR) {
        int b = bitmap_alloc(block_map);
        ni.blocks[0] = DATA_START_LBA + b;
        uint8_t buf[512] = {0};
        yfs_dir_entry_t* de = (yfs_dir_entry_t*)buf;
        de[0].inode_idx = new_idx; strlcpy(de[0].name, ".", 28);
        de[1].inode_idx = parent_idx; strlcpy(de[1].name, "..", 28);
        ata_write_sector(ni.blocks[0], buf);
    }
    write_inode(new_idx, &ni);
    if (add_entry_to_dir(parent_idx, name, new_idx) == -1) return -1;
    sync_metadata();
    return new_idx;
}

int yulafs_mkdir(const char* path) { return yfs_create_internal(path, YFS_TYPE_DIR); }
int yulafs_create(const char* path) { return yfs_create_internal(path, YFS_TYPE_FILE); }

void yulafs_update_size(uint32_t inode_idx, uint32_t new_size) {
    uint8_t buf[BLOCK_SIZE];
    uint32_t lba = INODE_TABLE_LBA + (inode_idx * 256) / BLOCK_SIZE;
    uint32_t off = (inode_idx * 256) % BLOCK_SIZE;
    ata_read_sector(lba, buf);
    
    yfs_inode_t* pinode = (yfs_inode_t*)(buf + off);
    
    pinode->size = new_size;
    
    ata_write_sector(lba, buf);
}

int yulafs_read(uint32_t inode_idx, void* buf, uint32_t offset, uint32_t size) {
    yfs_inode_t inode; read_inode(inode_idx, &inode);
    uint32_t limit = (inode.type == YFS_TYPE_DIR) ? BLOCK_SIZE : inode.size;
    
    if (offset >= limit) return 0;
    if (offset + size > limit) size = limit - offset;
    
    uint32_t bytes_read = 0;
    uint8_t block_buf[BLOCK_SIZE];
    
    while (bytes_read < size) {
        uint32_t logical_blk = (offset + bytes_read) / BLOCK_SIZE;
        uint32_t block_off   = (offset + bytes_read) % BLOCK_SIZE;
        
        uint32_t phys_lba = get_real_block(&inode, logical_blk, 0);
        
        if (phys_lba == 0) {
            memset(block_buf, 0, BLOCK_SIZE);
        } else {
            ata_read_sector(phys_lba, block_buf);
        }
        
        uint32_t chunk = BLOCK_SIZE - block_off;
        if (chunk > (size - bytes_read)) chunk = size - bytes_read;
        memcpy((uint8_t*)buf + bytes_read, block_buf + block_off, chunk);
        bytes_read += chunk;
    }
    return bytes_read;
}

int yulafs_write(uint32_t inode_idx, const void* buf, uint32_t offset, uint32_t size) {
    yfs_inode_t inode; read_inode(inode_idx, &inode);
    
    uint32_t written = 0;
    uint8_t block_buf[BLOCK_SIZE];
    
    int inode_dirty = 0; 

    while (written < size) {
        uint32_t logical_blk = (offset + written) / BLOCK_SIZE;
        uint32_t block_off   = (offset + written) % BLOCK_SIZE;
        
        uint32_t phys_lba = get_real_block(&inode, logical_blk, 1);
        if (phys_lba == 0) break;
        
        inode_dirty = 1;

        uint32_t chunk = BLOCK_SIZE - block_off;
        if (chunk > (size - written)) chunk = size - written;
        
        if (chunk < BLOCK_SIZE) {
            ata_read_sector(phys_lba, block_buf);
        }
        
        memcpy(block_buf + block_off, (uint8_t*)buf + written, chunk);
        ata_write_sector(phys_lba, block_buf);
        
        written += chunk;
    }
    
    if (offset + written > inode.size) {
        inode.size = offset + written;
        inode_dirty = 1;
    }
    
    if (inode_dirty) {
        write_inode(inode_idx, &inode);
        sync_metadata();
    }
    
    return written;
}

int yulafs_unlink(const char* path) {
    char name[32]; 
    uint32_t parent_dir_inode = proc_current()->cwd_inode;

    const char* last_slash = 0;
    for(const char* p = path; *p; p++) if(*p == '/') last_slash = p;

    if (last_slash) {
        if (last_slash == path) {
            parent_dir_inode = 1; 
        } else {
            char parent_p[256];
            int len = last_slash - path;
            memcpy(parent_p, path, len); 
            parent_p[len] = '\0';
            
            int p_idx = yulafs_lookup(parent_p);
            if (p_idx == -1) return -1;
            parent_dir_inode = (uint32_t)p_idx;
        }
        strlcpy(name, last_slash + 1, 32);
    } else {
        strlcpy(name, path, 32);
    }

    yfs_inode_t parent; 
    read_inode(parent_dir_inode, &parent);
    
    if (parent.blocks[0] == 0) return -1;

    yfs_dir_entry_t entries[16];
    ata_read_sector(parent.blocks[0], (uint8_t*)entries);

    int entry_idx = -1;
    uint32_t target_inode_idx = 0;

    for (int i = 0; i < 16; i++) {
        if (entries[i].inode_idx > 0 && strcmp(entries[i].name, name) == 0) {
            target_inode_idx = entries[i].inode_idx;
            entry_idx = i;
            break;
        }
    }

    if (entry_idx == -1) return -1; 

    yfs_inode_t target; 
    read_inode(target_inode_idx, &target);
    
    if (target.type == YFS_TYPE_DIR) return -2;

    for (int i = 0; i < INODE_DIRECT_COUNT; i++) {
        if (target.blocks[i] != 0) {
            uint32_t off = target.blocks[i] - DATA_START_LBA;
            bitmap_clear(block_map, off);
        }
    }

    if (target.blocks[INDIRECT_BLOCK_IDX] != 0) {
        uint32_t indices[128];
        ata_read_sector(target.blocks[INDIRECT_BLOCK_IDX], (uint8_t*)indices);
        
        for(int i=0; i<128; i++) {
            if (indices[i] != 0) {
                uint32_t off = indices[i] - DATA_START_LBA;
                bitmap_clear(block_map, off);
            }
        }
        
        uint32_t off_indirect = target.blocks[INDIRECT_BLOCK_IDX] - DATA_START_LBA;
        bitmap_clear(block_map, off_indirect);
    }

    bitmap_clear(inode_map, target_inode_idx);
    
    memset(&entries[entry_idx], 0, sizeof(yfs_dir_entry_t));
    ata_write_sector(parent.blocks[0], (uint8_t*)entries);
    
    sync_metadata();
    
    return 0;
}

int yulafs_get_inode(uint32_t idx, yfs_inode_t* out) {
    if (idx == 0 || idx >= MAX_INODES) return -1;
    read_inode(idx, out);
    return 0;
}