/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

#ifndef FS_BCACHE_H
#define FS_BCACHE_H

#include <stdint.h>

/*
 * Simple block cache.
 *
 * This module provides an in-memory cache for fixed-size 4KiB disk blocks.
 * The cache is shared globally and is used as the lowest layer under the file
 * system code.
 *
 * The cache uses a CLOCK-style eviction policy. Metadata is sharded internally
 * to reduce lock contention.
 *
 * The API is synchronous and does not provide buffer pinning: once a call
 * returns, the cached block may be evicted at any time.
 */

#ifdef __cplusplus
extern "C" {
#endif

struct block_device;

/*
 * Initialize cache state and size the cache based on total RAM.
 *
 * The current policy targets ~2% of RAM, split evenly across internal shards.
 */
void bcache_init(void);

void bcache_attach_device(struct block_device* dev);

/*
 * Read a 4KiB block into `buf`.
 *
 * Returns non-zero on success. If the cache is under pressure and cannot
 * allocate a new entry, the implementation may fall back to direct disk I/O.
 */
int bcache_read(uint32_t block_idx, uint8_t* buf);

/*
 * Update a cached 4KiB block from `buf` and mark it dirty.
 *
 * Returns non-zero on success. Under cache pressure this call may fail if it
 * cannot allocate (or make room for) a cache entry.
 */
int bcache_write(uint32_t block_idx, const uint8_t* buf);

/*
 * Write back all dirty cached blocks.
 *
 * This is expected to be called periodically by a background sync task.
 */
void bcache_sync(void);

/* Write back one dirty block if it is currently cached. */
void bcache_flush_block(uint32_t block_idx);

/*
 * Best-effort sequential readahead.
 *
 * Prefetches up to a small bounded number of blocks after `start_block`.
 * The operation is opportunistic: it can silently drop work on allocation
 * failure or cache pressure.
 */
void bcache_readahead(uint32_t start_block, uint32_t count);

#ifdef __cplusplus
}
#endif

#endif