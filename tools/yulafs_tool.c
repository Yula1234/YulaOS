#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define BLOCK_SIZE      512
#define INODE_BLOCKS    62
#define INODE_DIRECT    60
#define INODE_INDIRECT  60

#define SB_LBA          10   
#define INODE_MAP_LBA   11   
#define BLOCK_MAP_LBA   12   
#define INODE_TABLE_LBA 13   
#define DATA_START_LBA  100  

typedef struct {
    uint32_t type; // 0=FREE, 1=FILE, 2=DIR
    uint32_t size;
    uint32_t blocks[INODE_BLOCKS];
} __attribute__((packed)) yfs_inode_t;

typedef struct {
    char name[28];
    uint32_t inode_idx;
} __attribute__((packed)) yfs_dir_entry_t;

typedef struct {
    uint32_t magic;
    uint32_t inodes;
    uint32_t blocks;
} __attribute__((packed)) yfs_superblock_t;

FILE* disk;
uint8_t inode_map[BLOCK_SIZE];
uint8_t block_map[BLOCK_SIZE];

void read_sector(uint32_t lba, void* buf) { fseek(disk, lba*512, SEEK_SET); fread(buf, 1, 512, disk); }
void write_sector(uint32_t lba, void* buf) { fseek(disk, lba*512, SEEK_SET); fwrite(buf, 1, 512, disk); }

int bitmap_get(uint8_t* map, int i) { return (map[i/8] & (1<<(i%8))) != 0; }
void bitmap_set(uint8_t* map, int i) { map[i/8] |= (1<<(i%8)); }
void bitmap_clr(uint8_t* map, int i) { map[i/8] &= ~(1<<(i%8)); }

int alloc_block() {
    for(int i=0; i<512*8; i++) if(!bitmap_get(block_map, i)) { bitmap_set(block_map, i); return i; }
    return -1;
}
int alloc_inode() {
    for(int i=0; i<512*8; i++) if(!bitmap_get(inode_map, i)) { bitmap_set(inode_map, i); return i; }
    return -1;
}

void read_inode(int idx, yfs_inode_t* out) {
    uint8_t buf[512];
    read_sector(INODE_TABLE_LBA + (idx*256)/512, buf);
    memcpy(out, buf + (idx*256)%512, sizeof(yfs_inode_t));
}
void write_inode(int idx, yfs_inode_t* in) {
    uint8_t buf[512]; uint32_t lba = INODE_TABLE_LBA + (idx*256)/512;
    read_sector(lba, buf);
    memcpy(buf + (idx*256)%512, in, sizeof(yfs_inode_t));
    write_sector(lba, buf);
}

uint32_t get_real_block(yfs_inode_t* inode, uint32_t logical, int alloc) {
    if (logical < INODE_DIRECT) {
        if (inode->blocks[logical] == 0 && alloc) {
            int b = alloc_block(); if(b==-1) return 0;
            inode->blocks[logical] = DATA_START_LBA + b;
        }
        return inode->blocks[logical];
    }
    
    uint32_t idx = logical - INODE_DIRECT;
    if (idx >= 128) return 0;

    if (inode->blocks[INODE_INDIRECT] == 0) {
        if (!alloc) return 0;
        int b = alloc_block(); if(b==-1) return 0;
        inode->blocks[INODE_INDIRECT] = DATA_START_LBA + b;
        uint8_t z[512] = {0}; write_sector(inode->blocks[INODE_INDIRECT], z);
    }

    uint32_t tbl[128];
    read_sector(inode->blocks[INODE_INDIRECT], tbl);
    
    if (tbl[idx] == 0 && alloc) {
        int b = alloc_block(); if(b==-1) return 0;
        tbl[idx] = DATA_START_LBA + b;
        write_sector(inode->blocks[INODE_INDIRECT], tbl);
    }
    return tbl[idx];
}

int ensure_dir(const char* name, int parent_inode) {
    yfs_inode_t parent; read_inode(parent_inode, &parent);
    yfs_dir_entry_t entries[16]; read_sector(parent.blocks[0], entries);
    
    for(int i=0; i<16; i++) {
        if (entries[i].inode_idx && strcmp(entries[i].name, name)==0) return entries[i].inode_idx;
    }

    int new_idx = alloc_inode();
    if(new_idx == -1) return -1;
    
    yfs_inode_t dir = { .type = 2, .size = 0 };
    int b = alloc_block();
    dir.blocks[0] = DATA_START_LBA + b;
    write_inode(new_idx, &dir);
    
    uint8_t buf[512] = {0};
    yfs_dir_entry_t* e = (yfs_dir_entry_t*)buf;
    e[0].inode_idx = new_idx; strcpy(e[0].name, ".");
    e[1].inode_idx = parent_inode; strcpy(e[1].name, "..");
    write_sector(dir.blocks[0], buf);
    
    for(int i=0; i<16; i++) if(!entries[i].inode_idx) {
        entries[i].inode_idx = new_idx; 
        strncpy(entries[i].name, name, 27);
        write_sector(parent.blocks[0], entries);
        break;
    }
    
    return new_idx;
}

void cmd_format() {
    memset(inode_map, 0, 512); memset(block_map, 0, 512);
    bitmap_set(inode_map, 0); bitmap_set(inode_map, 1);
    
    int b = alloc_block();
    yfs_inode_t root = { .type = 2, .size = 0 };
    root.blocks[0] = DATA_START_LBA + b;
    write_inode(1, &root);
    
    uint8_t buf[512] = {0};
    yfs_dir_entry_t* e = (yfs_dir_entry_t*)buf;
    e[0].inode_idx = 1; strcpy(e[0].name, ".");
    e[1].inode_idx = 1; strcpy(e[1].name, "..");
    write_sector(root.blocks[0], buf);
    
    yfs_superblock_t sb = { 0x59554C41, 128, 4096 };
    write_sector(SB_LBA, &sb);
    
    write_sector(INODE_MAP_LBA, inode_map);
    write_sector(BLOCK_MAP_LBA, block_map);
    
    ensure_dir("bin", 1);
    ensure_dir("home", 1);
    
    write_sector(INODE_MAP_LBA, inode_map);
    write_sector(BLOCK_MAP_LBA, block_map);
    printf("Formatted (with /bin and /home).\n");
}

void cmd_import(const char* host_path, const char* os_path) {
    read_sector(INODE_MAP_LBA, inode_map);
    read_sector(BLOCK_MAP_LBA, block_map);
    
    FILE* f = fopen(host_path, "rb");
    if(!f) { printf("File not found: %s\n", host_path); return; }
    fseek(f, 0, SEEK_END); uint32_t size = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* data = malloc(size); fread(data, 1, size, f); fclose(f);
    
    int parent_inode = 1;
    char filename[32];
    
    if (strncmp(os_path, "/bin/", 5) == 0) {
        parent_inode = ensure_dir("bin", 1);
        strncpy(filename, os_path + 5, 31);
    } else if (strncmp(os_path, "/home/", 6) == 0) {
        parent_inode = ensure_dir("home", 1);
        strncpy(filename, os_path + 6, 31);
    } else {
        const char* p = (os_path[0] == '/') ? os_path + 1 : os_path;
        strncpy(filename, p, 31);
    }
    
    yfs_inode_t parent; read_inode(parent_inode, &parent);
    yfs_dir_entry_t entries[16]; read_sector(parent.blocks[0], entries);
    
    int inode_idx = -1;
    for(int i=0; i<16; i++) {
        if (entries[i].inode_idx && strcmp(entries[i].name, filename)==0) inode_idx = entries[i].inode_idx;
    }
    
    yfs_inode_t file;
    if (inode_idx != -1) {
        read_inode(inode_idx, &file);
    } else {
        inode_idx = alloc_inode();
        memset(&file, 0, sizeof(file)); file.type = 1;
        for(int i=0; i<16; i++) if(!entries[i].inode_idx) {
            entries[i].inode_idx = inode_idx; strncpy(entries[i].name, filename, 27);
            write_sector(parent.blocks[0], entries); break;
        }
        printf("Importing %s -> inode %d (dir %d)\n", filename, inode_idx, parent_inode);
    }
    
    file.size = size;
    uint32_t written = 0;
    while(written < size) {
        uint32_t log_blk = written / 512;
        uint32_t lba = get_real_block(&file, log_blk, 1);
        if(!lba) { printf("Disk full!\n"); break; }
        
        uint32_t chunk = size - written; if(chunk > 512) chunk = 512;
        uint8_t sect[512] = {0};
        memcpy(sect, data + written, chunk);
        write_sector(lba, sect);
        written += chunk;
    }
    
    write_inode(inode_idx, &file);
    write_sector(INODE_MAP_LBA, inode_map);
    write_sector(BLOCK_MAP_LBA, block_map);
    free(data);
}

int main(int argc, char** argv) {
    if(argc < 3) {
        printf("Usage: %s <disk.img> [format | import <host_file> <os_path>]\n", argv[0]);
        return 1;
    }
    disk = fopen(argv[1], "rb+");
    if(!disk && strcmp(argv[2], "format")==0) disk = fopen(argv[1], "wb+");
    if(!disk) { printf("Cannot open disk image\n"); return 1; }
    
    if(strcmp(argv[2], "format")==0) cmd_format();
    if(strcmp(argv[2], "import")==0) {
        if (argc < 5) printf("Usage: import <host> <os_path>\n");
        else cmd_import(argv[3], argv[4]);
    }
    
    fclose(disk);
    return 0;
}