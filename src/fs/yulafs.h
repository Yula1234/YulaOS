#ifndef FS_YULAFS_H
#define FS_YULAFS_H

#include <stdint.h>
#include <stddef.h>

#define YFS_MAGIC       0x59554C41
#define BLOCK_SIZE      512
#define MAX_INODES      128

#define INODE_BLOCKS_COUNT 62 

#define INODE_DIRECT_COUNT 60

#define INDIRECT_BLOCK_IDX 60 

#define SB_LBA          10   
#define INODE_MAP_LBA   11   
#define BLOCK_MAP_LBA   12   
#define INODE_TABLE_LBA 13   
#define DATA_START_LBA  100  

typedef enum {
    YFS_TYPE_FREE = 0,
    YFS_TYPE_FILE = 1,
    YFS_TYPE_DIR  = 2
} yfs_type_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t blocks[INODE_BLOCKS_COUNT];
} __attribute__((packed)) yfs_inode_t;

typedef struct {
    char name[28];
    uint32_t inode_idx;
} __attribute__((packed)) yfs_dir_entry_t;

typedef struct {
    uint32_t magic;
    uint32_t total_inodes;
    uint32_t total_blocks;
} __attribute__((packed)) yfs_superblock_t;

void yulafs_init(void);
void yulafs_format(void);
int  yulafs_lookup(const char* path);
int  yulafs_mkdir(const char* path);
int  yulafs_create(const char* path);
int  yulafs_read(uint32_t inode_idx, void* buf, uint32_t offset, uint32_t size);
int  yulafs_write(uint32_t inode_idx, const void* buf, uint32_t offset, uint32_t size);
void yulafs_ls(const char* path);
int  yulafs_unlink(const char* path);
int  yulafs_get_inode(uint32_t idx, yfs_inode_t* out);
void yulafs_update_size(uint32_t inode_idx, uint32_t new_size);

#endif