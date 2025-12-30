// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef MM_HEAP_H
#define MM_HEAP_H

#include <stddef.h>
#include <stdint.h>

void heap_init(void);

void* kmalloc(size_t size);
void* kzalloc(size_t size);
void* krealloc(void* ptr, size_t new_size);
void  kfree(void* ptr);

void* kmalloc_aligned(size_t size, uint32_t align); 

void* kmalloc_a(size_t size);

typedef struct kmem_cache kmem_cache_t;

kmem_cache_t* kmem_cache_create(const char* name, size_t size, uint32_t align, uint32_t flags);
void* kmem_cache_alloc(kmem_cache_t* cache);
void  kmem_cache_free(kmem_cache_t* cache, void* obj);

#endif