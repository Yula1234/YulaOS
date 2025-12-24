#ifndef MM_HEAP_H
#define MM_HEAP_H

#include <stddef.h>
#include <stdint.h>

void  heap_init(void);
void* kmalloc(size_t n);
void* kmalloc_a(size_t n); // kmalloc aligned
void kfree(void* ptr);

#endif