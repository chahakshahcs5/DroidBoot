#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

void  heap_init(uintptr_t base, size_t size);
void *heap_malloc(size_t size);
void *heap_malloc_aligned(size_t size, size_t alignment);
void *heap_calloc(size_t num, size_t size);
void *heap_realloc(void *ptr, size_t new_size);
void  heap_free(void *ptr);

size_t heap_get_free_bytes(void);
size_t heap_get_used_bytes(void);

#endif // HEAP_H
