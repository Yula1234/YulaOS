#ifndef FS_BCACHE_H
#define FS_BCACHE_H

#include <stdint.h>

void bcache_init(void);
int bcache_read(uint32_t lba, uint8_t* buf);
int bcache_write(uint32_t lba, const uint8_t* buf);
void bcache_sync(void);
void bcache_flush_block(uint32_t lba);

#endif