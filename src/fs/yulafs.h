// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef FS_YULAFS_H
#define FS_YULAFS_H

#include <stdint.h>

#define YFS_MAGIC       0x59554C41  // 'YULA'
#define YFS_VERSION     2
#define YFS_BLOCK_SIZE  4096    
#define YFS_NAME_MAX    60

#define YFS_DIRECT_PTRS     12
#define YFS_PTRS_PER_BLOCK  (YFS_BLOCK_SIZE / sizeof(uint32_t))

typedef uint32_t yfs_blk_t; 
typedef uint32_t yfs_ino_t; 
typedef uint32_t yfs_off_t; 

typedef enum {
    YFS_TYPE_FREE = 0,
    YFS_TYPE_FILE = 1,
    YFS_TYPE_DIR  = 2
} yfs_file_type_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    
    uint32_t total_blocks;
    uint32_t total_inodes;
    uint32_t free_blocks;
    uint32_t free_inodes;

    yfs_blk_t map_inode_start;
    yfs_blk_t map_block_start;
    yfs_blk_t inode_table_start;
    yfs_blk_t data_start;

    uint8_t  padding[4052];
} __attribute__((packed)) yfs_superblock_t;

typedef struct {
    yfs_ino_t id;          
    uint32_t  type;        
    uint32_t  size;        
    uint32_t  flags;       
    uint32_t  created_at;  
    uint32_t  modified_at; 

    yfs_blk_t direct[YFS_DIRECT_PTRS];
    yfs_blk_t indirect;               
    yfs_blk_t doubly_indirect;        
    yfs_blk_t triply_indirect;        
    uint8_t   padding[44];
} __attribute__((packed)) yfs_inode_t;

typedef struct {
    yfs_ino_t inode; 
    char      name[YFS_NAME_MAX];
} __attribute__((packed)) yfs_dirent_t;

typedef struct {
    yfs_ino_t inode;
    uint32_t type;
    uint32_t size;
    char name[YFS_NAME_MAX];
} __attribute__((packed)) yfs_dirent_info_t;

void yulafs_init(void);
void yulafs_format(uint32_t disk_size_sectors);

int yulafs_read(yfs_ino_t ino, void* buf, yfs_off_t offset, uint32_t size);
int yulafs_write(yfs_ino_t ino, const void* buf, yfs_off_t offset, uint32_t size);

int yulafs_open(const char* path, int flags);
int yulafs_mkdir(const char* path);
int yulafs_create(const char* path);
int yulafs_unlink(const char* path);
int yulafs_lookup(const char* path);

int yulafs_stat(yfs_ino_t ino, yfs_inode_t* out);
void yulafs_resize(yfs_ino_t ino, uint32_t new_size);

int yulafs_lookup_in_dir(yfs_ino_t dir_ino, const char* name);
int yulafs_getdents(yfs_ino_t dir_ino, uint32_t* inout_offset, yfs_dirent_info_t* out, uint32_t out_size);

void yulafs_get_filesystem_info(uint32_t* total_blocks, uint32_t* free_blocks, uint32_t* block_size);
int yulafs_rename(const char* old_path, const char* new_path);

#endif