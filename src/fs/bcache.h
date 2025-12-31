// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef FS_BCACHE_H
#define FS_BCACHE_H

#include <stdint.h>

void bcache_init(void);

int bcache_read(uint32_t block_idx, uint8_t* buf);
int bcache_write(uint32_t block_idx, const uint8_t* buf);

void bcache_sync(void);
void bcache_flush_block(uint32_t block_idx);

void bcache_readahead(uint32_t start_block, uint32_t count);

#endif