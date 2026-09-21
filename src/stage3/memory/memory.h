#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>
#include <stddef.h>
#include "../../include/boot.h"

void memory_init(boot_info_t *boot_info);

// General purpose heap allocator with functional free and reallocation
void *kmalloc(size_t size);
void *kmalloc_aligned(size_t size, size_t alignment);
void *kcalloc(size_t num, size_t size);
void *krealloc(void *ptr, size_t new_size);
void  kfree(void *ptr);

// Statistics
uint64_t memory_get_total_usable(void);
size_t   memory_get_free_heap(void);
size_t   memory_get_used_heap(void);

#endif // MEMORY_H
