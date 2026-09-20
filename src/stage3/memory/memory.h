#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>
#include <stddef.h>
#include "../../include/boot.h"

void memory_init(boot_info_t *boot_info);

// General purpose heap allocator
void *kmalloc(size_t size);
void *kmalloc_aligned(size_t size, size_t alignment);
void kfree(void *ptr);

// Statistics
uint64_t memory_get_total_usable(void);

#endif // MEMORY_H
