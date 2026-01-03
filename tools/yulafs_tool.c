// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <stdarg.h>

#define YFS_MAGIC       0x59554C41
#define YFS_VERSION     2
#define BLOCK_SIZE      4096    
#define NAME_MAX        60

#define DIRECT_PTRS     12
#define PTRS_PER_BLOCK  (BLOCK_SIZE / 4)

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    
    uint32_t total_blocks;
    uint32_t total_inodes;
    uint32_t free_blocks;
    uint32_t free_inodes;
    
    uint32_t map_inode_start;
    uint32_t map_block_start;
    uint32_t inode_table_start;
    uint32_t data_start;
    
    uint8_t  padding[4052];
} __attribute__((packed)) yfs_superblock_t;

typedef struct {
    uint32_t id;
    uint32_t type;      // 1=FILE, 2=DIR
    uint32_t size;
    uint32_t flags;
    uint32_t created;
    uint32_t modified;
    
    uint32_t direct[DIRECT_PTRS];
    uint32_t indirect;           
    uint32_t doubly_indirect;    
    uint32_t triply_indirect;    
    
    uint8_t  padding[44];
} __attribute__((packed)) yfs_inode_t;

typedef struct {
    uint32_t inode;
    char     name[NAME_MAX];
} __attribute__((packed)) yfs_dirent_t;


typedef struct {
    FILE* fp;
    yfs_superblock_t sb;
    char* img_path;
} YulaCtx;

static void panic(YulaCtx* ctx, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[ERROR] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
    if (ctx && ctx->fp) fclose(ctx->fp);
    exit(EXIT_FAILURE);
}

static void log_info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stdout, "[INFO] ");
    vfprintf(stdout, fmt, args);
    fprintf(stdout, "\n");
    va_end(args);
}

static void disk_read(YulaCtx* ctx, uint32_t block_idx, void* buf) {
    if (fseek(ctx->fp, (long)block_idx * BLOCK_SIZE, SEEK_SET) != 0)
        panic(ctx, "Seek error at Block %u", block_idx);
    if (fread(buf, 1, BLOCK_SIZE, ctx->fp) != BLOCK_SIZE)
        panic(ctx, "Read error at Block %u", block_idx);
}

static void disk_write(YulaCtx* ctx, uint32_t block_idx, const void* buf) {
    if (fseek(ctx->fp, (long)block_idx * BLOCK_SIZE, SEEK_SET) != 0)
        panic(ctx, "Seek error at Block %u", block_idx);
    if (fwrite(buf, 1, BLOCK_SIZE, ctx->fp) != BLOCK_SIZE)
        panic(ctx, "Write error at Block %u", block_idx);
}

static void disk_zero_block(YulaCtx* ctx, uint32_t block_idx) {
    uint8_t zero[BLOCK_SIZE] = {0};
    disk_write(ctx, block_idx, zero);
}

static void sb_sync(YulaCtx* ctx) {
    disk_write(ctx, 1, &ctx->sb);
}

static int bitmap_get(const uint8_t* map, int i) {
    return (map[i / 8] & (1 << (i % 8)));
}

static void bitmap_set(uint8_t* map, int i) {
    map[i / 8] |= (1 << (i % 8));
}

static uint32_t alloc_block(YulaCtx* ctx) {
    if (ctx->sb.free_blocks == 0) panic(ctx, "No free blocks");

    uint8_t buf[BLOCK_SIZE];
    uint32_t map_blocks = (ctx->sb.total_blocks + (BLOCK_SIZE*8) - 1) / (BLOCK_SIZE*8);

    for (uint32_t i = 0; i < map_blocks; i++) {
        uint32_t map_lba = ctx->sb.map_block_start + i;
        disk_read(ctx, map_lba, buf);

        for (int j = 0; j < BLOCK_SIZE * 8; j++) {
            if (!bitmap_get(buf, j)) {
                bitmap_set(buf, j);
                disk_write(ctx, map_lba, buf);

                ctx->sb.free_blocks--;
                sb_sync(ctx);

                uint32_t lba = ctx->sb.data_start + (i * BLOCK_SIZE * 8) + j;
                disk_zero_block(ctx, lba);
                return lba;
            }
        }
    }
    panic(ctx, "Bitmap inconsistency");
    return 0;
}

static uint32_t alloc_inode(YulaCtx* ctx) {
    if (ctx->sb.free_inodes == 0) panic(ctx, "No free inodes");

    uint8_t buf[BLOCK_SIZE];
    uint32_t map_blocks = (ctx->sb.total_inodes + (BLOCK_SIZE*8) - 1) / (BLOCK_SIZE*8);

    for (uint32_t i = 0; i < map_blocks; i++) {
        uint32_t map_lba = ctx->sb.map_inode_start + i;
        disk_read(ctx, map_lba, buf);

        for (int j = 0; j < BLOCK_SIZE * 8; j++) {
            uint32_t ino = (i * BLOCK_SIZE * 8) + j;
            if (ino == 0) continue; 
            if (ino >= ctx->sb.total_inodes) break;

            if (!bitmap_get(buf, j)) {
                bitmap_set(buf, j);
                disk_write(ctx, map_lba, buf);

                ctx->sb.free_inodes--;
                sb_sync(ctx);
                return ino;
            }
        }
    }
    panic(ctx, "Bitmap inconsistency");
    return 0;
}

static void inode_read(YulaCtx* ctx, uint32_t id, yfs_inode_t* out) {
    uint32_t per_block = BLOCK_SIZE / sizeof(yfs_inode_t);
    uint32_t lba = ctx->sb.inode_table_start + (id / per_block);
    uint32_t off = id % per_block;

    uint8_t buf[BLOCK_SIZE];
    disk_read(ctx, lba, buf);
    memcpy(out, (yfs_inode_t*)buf + off, sizeof(yfs_inode_t));
}

static void inode_write(YulaCtx* ctx, uint32_t id, const yfs_inode_t* in) {
    uint32_t per_block = BLOCK_SIZE / sizeof(yfs_inode_t);
    uint32_t lba = ctx->sb.inode_table_start + (id / per_block);
    uint32_t off = id % per_block;

    uint8_t buf[BLOCK_SIZE];
    disk_read(ctx, lba, buf);
    memcpy((yfs_inode_t*)buf + off, in, sizeof(yfs_inode_t));
    disk_write(ctx, lba, buf);
}

static uint32_t inode_resolve_block(YulaCtx* ctx, yfs_inode_t* node, uint32_t block_idx, int alloc) {
    if (block_idx < DIRECT_PTRS) {
        if (node->direct[block_idx] == 0) {
            if (!alloc) return 0;
            node->direct[block_idx] = alloc_block(ctx);
        }
        return node->direct[block_idx];
    }
    block_idx -= DIRECT_PTRS;

    if (block_idx < PTRS_PER_BLOCK) {
        if (node->indirect == 0) {
            if (!alloc) return 0;
            node->indirect = alloc_block(ctx);
        }
        uint32_t tbl[PTRS_PER_BLOCK];
        disk_read(ctx, node->indirect, tbl);
        if (tbl[block_idx] == 0) {
            if (!alloc) return 0;
            tbl[block_idx] = alloc_block(ctx);
            disk_write(ctx, node->indirect, tbl);
        }
        return tbl[block_idx];
    }
    block_idx -= PTRS_PER_BLOCK;

    if (block_idx < PTRS_PER_BLOCK * PTRS_PER_BLOCK) {
        if (node->doubly_indirect == 0) {
            if (!alloc) return 0;
            node->doubly_indirect = alloc_block(ctx);
        }
        
        uint32_t l1[PTRS_PER_BLOCK];
        disk_read(ctx, node->doubly_indirect, l1);
        uint32_t idx1 = block_idx / PTRS_PER_BLOCK;
        uint32_t idx2 = block_idx % PTRS_PER_BLOCK;

        if (l1[idx1] == 0) {
            if (!alloc) return 0;
            l1[idx1] = alloc_block(ctx);
            disk_write(ctx, node->doubly_indirect, l1);
        }

        uint32_t l2[PTRS_PER_BLOCK];
        disk_read(ctx, l1[idx1], l2);
        if (l2[idx2] == 0) {
            if (!alloc) return 0;
            l2[idx2] = alloc_block(ctx);
            disk_write(ctx, l1[idx1], l2);
        }
        return l2[idx2];
    }
    block_idx -= PTRS_PER_BLOCK * PTRS_PER_BLOCK;

    if (block_idx < PTRS_PER_BLOCK * PTRS_PER_BLOCK * PTRS_PER_BLOCK) {
        if (node->triply_indirect == 0) {
            if (!alloc) return 0;
            node->triply_indirect = alloc_block(ctx);
        }

        uint32_t l1[PTRS_PER_BLOCK];
        disk_read(ctx, node->triply_indirect, l1);

        uint32_t ptrs_sq = PTRS_PER_BLOCK * PTRS_PER_BLOCK;
        uint32_t i1 = block_idx / ptrs_sq;
        uint32_t rem = block_idx % ptrs_sq;
        uint32_t i2 = rem / PTRS_PER_BLOCK;
        uint32_t i3 = rem % PTRS_PER_BLOCK;

        if (l1[i1] == 0) {
            if (!alloc) return 0;
            l1[i1] = alloc_block(ctx);
            disk_write(ctx, node->triply_indirect, l1);
        }

        uint32_t l2[PTRS_PER_BLOCK];
        disk_read(ctx, l1[i1], l2);
        if (l2[i2] == 0) {
            if (!alloc) return 0;
            l2[i2] = alloc_block(ctx);
            disk_write(ctx, l1[i1], l2);
        }

        uint32_t l3[PTRS_PER_BLOCK];
        disk_read(ctx, l2[i2], l3);
        if (l3[i3] == 0) {
            if (!alloc) return 0;
            l3[i3] = alloc_block(ctx);
            disk_write(ctx, l2[i2], l3);
        }
        return l3[i3];
    }

    panic(ctx, "File too large");
    return 0;
}

static uint32_t dir_find(YulaCtx* ctx, uint32_t dir_ino, const char* name) {
    yfs_inode_t dir;
    inode_read(ctx, dir_ino, &dir);

    if (dir.type != 2) panic(ctx, "Inode %u is not a directory", dir_ino);

    uint32_t entries_per_block = BLOCK_SIZE / sizeof(yfs_dirent_t);
    yfs_dirent_t* entries = malloc(BLOCK_SIZE);
    
    uint32_t blocks = (dir.size + BLOCK_SIZE - 1) / BLOCK_SIZE;

    for (uint32_t i = 0; i < blocks; i++) {
        uint32_t lba = inode_resolve_block(ctx, &dir, i, 0);
        if (!lba) continue;

        disk_read(ctx, lba, entries);
        for (uint32_t j = 0; j < entries_per_block; j++) {
            if (entries[j].inode != 0 && strcmp(entries[j].name, name) == 0) {
                uint32_t found = entries[j].inode;
                free(entries);
                return found;
            }
        }
    }
    free(entries);
    return 0;
}

static uint32_t path_resolve(YulaCtx* ctx, const char* os_path) {
    if (!os_path || !*os_path) return 1;

    const char* p = os_path;
    if (*p == '/') p++;

    uint32_t curr = 1;

    while (1) {
        while (*p == '/') p++;
        if (*p == 0) break;

        const char* start = p;
        while (*p && *p != '/') p++;

        int len = (int)(p - start);
        if (len <= 0) break;
        if (len >= NAME_MAX) panic(ctx, "Path component too long: %.*s", len, start);

        char name[NAME_MAX];
        memcpy(name, start, (size_t)len);
        name[len] = 0;

        uint32_t next = dir_find(ctx, curr, name);
        if (!next) return 0;

        if (*p) {
            yfs_inode_t inode;
            inode_read(ctx, next, &inode);
            if (inode.type != 2) return 0;
        }

        curr = next;
    }

    return curr;
}

static void dir_add(YulaCtx* ctx, uint32_t dir_ino, uint32_t child_ino, const char* name) {
    yfs_inode_t dir;
    inode_read(ctx, dir_ino, &dir);

    uint32_t entries_per_block = BLOCK_SIZE / sizeof(yfs_dirent_t);
    yfs_dirent_t* entries = calloc(1, BLOCK_SIZE);
    uint32_t blk_idx = 0;

    while (1) {
        uint32_t lba = inode_resolve_block(ctx, &dir, blk_idx, 1);
        
        disk_read(ctx, lba, entries);

        for (uint32_t i = 0; i < entries_per_block; i++) {
            if (entries[i].inode == 0) {
                entries[i].inode = child_ino;
                strncpy(entries[i].name, name, NAME_MAX);
                
                disk_write(ctx, lba, entries);

                uint32_t min_size = (blk_idx + 1) * BLOCK_SIZE;
                if (dir.size < min_size) {
                    dir.size = min_size;
                    inode_write(ctx, dir_ino, &dir);
                }
                free(entries);
                return;
            }
        }
        blk_idx++;
    }
}

static void dir_init_dots(YulaCtx* ctx, uint32_t self_ino, uint32_t parent_ino) {
    yfs_inode_t dir;
    inode_read(ctx, self_ino, &dir);

    dir.size = BLOCK_SIZE;
    dir.direct[0] = alloc_block(ctx);
    
    yfs_dirent_t* block = calloc(1, BLOCK_SIZE);
    
    block[0].inode = self_ino;
    strcpy(block[0].name, ".");
    
    block[1].inode = parent_ino;
    strcpy(block[1].name, "..");
    
    disk_write(ctx, dir.direct[0], block);
    inode_write(ctx, self_ino, &dir);
    free(block);
}

static uint32_t dir_ensure(YulaCtx* ctx, uint32_t parent_ino, const char* name) {
    uint32_t existing = dir_find(ctx, parent_ino, name);
    if (existing) return existing;

    uint32_t new_ino = alloc_inode(ctx);
    yfs_inode_t node = {0};
    node.id = new_ino;
    node.type = 2;
    node.created = time(NULL);
    
    inode_write(ctx, new_ino, &node);
    dir_init_dots(ctx, new_ino, parent_ino);
    dir_add(ctx, parent_ino, new_ino, name);
    
    log_info("Created directory: %s (inode %u)", name, new_ino);
    return new_ino;
}

static void op_format(YulaCtx* ctx) {
    fseek(ctx->fp, 0, SEEK_END);
    long size_bytes = ftell(ctx->fp);
    if (size_bytes < 4 * 1024 * 1024) panic(ctx, "Image too small (<4MB)");
    
    uint32_t total_blocks = size_bytes / BLOCK_SIZE;

    memset(&ctx->sb, 0, sizeof(ctx->sb));
    ctx->sb.magic = YFS_MAGIC;
    ctx->sb.version = YFS_VERSION;
    ctx->sb.block_size = BLOCK_SIZE;
    ctx->sb.total_blocks = total_blocks;
    
    ctx->sb.total_inodes = total_blocks / 8;
    if (ctx->sb.total_inodes < 128) ctx->sb.total_inodes = 128;

    uint32_t imap_sz = (ctx->sb.total_inodes + (BLOCK_SIZE*8) - 1) / (BLOCK_SIZE*8);
    uint32_t bmap_sz = (ctx->sb.total_blocks + (BLOCK_SIZE*8) - 1) / (BLOCK_SIZE*8);
    uint32_t itbl_sz = (ctx->sb.total_inodes * sizeof(yfs_inode_t) + BLOCK_SIZE - 1) / BLOCK_SIZE;

    ctx->sb.map_inode_start = 2;
    ctx->sb.map_block_start = ctx->sb.map_inode_start + imap_sz;
    ctx->sb.inode_table_start = ctx->sb.map_block_start + bmap_sz;
    ctx->sb.data_start = ctx->sb.inode_table_start + itbl_sz;

    ctx->sb.free_inodes = ctx->sb.total_inodes;
    ctx->sb.free_blocks = ctx->sb.total_blocks - ctx->sb.data_start;

    uint8_t zero[BLOCK_SIZE] = {0};
    for (uint32_t i = 2; i < ctx->sb.data_start; i++) {
        disk_write(ctx, i, zero);
    }

    uint8_t imap[BLOCK_SIZE] = {0};
    imap[0] |= 3; 
    disk_write(ctx, ctx->sb.map_inode_start, imap);
    ctx->sb.free_inodes -= 2;

    yfs_inode_t root = {0};
    root.id = 1;
    root.type = 2; // DIR
    root.created = time(NULL);
    inode_write(ctx, 1, &root);
    
    dir_init_dots(ctx, 1, 1);

    sb_sync(ctx);
    
    log_info("Formatted. %u blocks (4KB), %u inodes.", ctx->sb.total_blocks, ctx->sb.total_inodes);
    
    dir_ensure(ctx, 1, "bin");
    dir_ensure(ctx, 1, "home");
    dir_ensure(ctx, 1, "dev");
}

static void op_import(YulaCtx* ctx, const char* host_path, const char* os_path) {
    FILE* hf = fopen(host_path, "rb");
    if (!hf) panic(ctx, "Cannot open host file: %s", host_path);
    
    fseek(hf, 0, SEEK_END);
    long fsize = ftell(hf);
    fseek(hf, 0, SEEK_SET);
    
    uint8_t* data = malloc(fsize);
    if (fread(data, 1, fsize, hf) != (size_t)fsize) panic(ctx, "Host file read error");
    fclose(hf);

    char fname[NAME_MAX];
    uint32_t parent = 1;

    if (strncmp(os_path, "/bin/", 5) == 0) {
        parent = dir_find(ctx, 1, "bin");
        strncpy(fname, os_path + 5, NAME_MAX);
    } else if (strncmp(os_path, "/home/", 6) == 0) {
        parent = dir_find(ctx, 1, "home");
        strncpy(fname, os_path + 6, NAME_MAX);
    } else {
        strncpy(fname, (os_path[0] == '/') ? os_path + 1 : os_path, NAME_MAX);
    }

    if (parent == 0) panic(ctx, "Parent directory for %s not found", os_path);

    uint32_t ino = dir_find(ctx, parent, fname);
    int is_update = 0;

    if (ino) {
        log_info("File %s exists (inode %u), updating...", fname, ino);
        is_update = 1;
    } else {
        ino = alloc_inode(ctx);
    }

    yfs_inode_t node = {0};
    if (is_update) inode_read(ctx, ino, &node);
    else {
        node.id = ino;
        node.type = 1; // FILE
        node.created = time(NULL);
    }
    
    node.size = fsize;
    node.modified = time(NULL);

    uint32_t written = 0;
    while (written < (uint32_t)fsize) {
        uint32_t blk_idx = written / BLOCK_SIZE;
        uint32_t off = written % BLOCK_SIZE;
        uint32_t chunk = BLOCK_SIZE - off;
        if (chunk > fsize - written) chunk = fsize - written;

        uint32_t lba = inode_resolve_block(ctx, &node, blk_idx, 1);
        
        uint8_t sector[BLOCK_SIZE];
        if (off > 0 || chunk < BLOCK_SIZE) {
            disk_read(ctx, lba, sector);
        }
        
        memcpy(sector + off, data + written, chunk);
        disk_write(ctx, lba, sector);
        
        written += chunk;
    }

    inode_write(ctx, ino, &node);

    if (!is_update) {
        dir_add(ctx, parent, ino, fname);
        log_info("Imported %s -> %s (inode %u, size %ld)", host_path, os_path, ino, fsize);
    }

    free(data);
}

static void op_export(YulaCtx* ctx, const char* os_path, const char* host_path) {
    uint32_t ino = path_resolve(ctx, os_path);
    if (!ino) panic(ctx, "Path not found in image: %s", os_path);

    yfs_inode_t node;
    inode_read(ctx, ino, &node);
    if (node.type != 1) panic(ctx, "Not a file: %s", os_path);

    FILE* hf = fopen(host_path, "wb");
    if (!hf) panic(ctx, "Cannot open host output file: %s", host_path);

    uint8_t sector[BLOCK_SIZE];
    uint32_t offset = 0;

    while (offset < node.size) {
        uint32_t blk_idx = offset / BLOCK_SIZE;
        uint32_t off = offset % BLOCK_SIZE;
        uint32_t chunk = BLOCK_SIZE - off;
        if (chunk > node.size - offset) chunk = node.size - offset;

        uint32_t lba = inode_resolve_block(ctx, &node, blk_idx, 0);
        if (lba) disk_read(ctx, lba, sector);
        else memset(sector, 0, sizeof(sector));

        if (fwrite(sector + off, 1, chunk, hf) != chunk) {
            fclose(hf);
            panic(ctx, "Host file write error: %s", host_path);
        }

        offset += chunk;
    }

    fclose(hf);
    log_info("Exported %s -> %s (inode %u, size %u)", os_path, host_path, ino, node.size);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("Usage: %s <disk.img> [format | import <host> <os> | export <os> <host>]\n", argv[0]);
        return 1;
    }

    YulaCtx ctx = {0};
    ctx.img_path = argv[1];
    
    ctx.fp = fopen(ctx.img_path, "rb+");
    if (!ctx.fp) {
        ctx.fp = fopen(ctx.img_path, "wb+"); 
        if (!ctx.fp) panic(NULL, "Cannot open/create disk image: %s", ctx.img_path);
    }

    const char* cmd = argv[2];

    if (strcmp(cmd, "format") == 0) {
        op_format(&ctx);
    } 
    else if (strcmp(cmd, "import") == 0) {
        if (argc < 5) panic(&ctx, "Missing args for import: <host_path> <os_path>");
        
        disk_read(&ctx, 1, &ctx.sb);
        if (ctx.sb.magic != YFS_MAGIC) panic(&ctx, "Invalid YulaFS signature");
        
        op_import(&ctx, argv[3], argv[4]);
    } 
    else if (strcmp(cmd, "export") == 0) {
        if (argc < 5) panic(&ctx, "Missing args for export: <os_path> <host_path>");

        disk_read(&ctx, 1, &ctx.sb);
        if (ctx.sb.magic != YFS_MAGIC) panic(&ctx, "Invalid YulaFS signature");

        op_export(&ctx, argv[3], argv[4]);
    }
    else {
        panic(&ctx, "Unknown command: %s", cmd);
    }

    fclose(ctx.fp);
    return 0;
}