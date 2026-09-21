#include "heap.h"
#include "../core/printf.h"

#define HEAP_BLOCK_MAGIC 0x48454150 // "HEAP"
#define HEAP_MIN_BLOCK   32

typedef struct heap_block {
    uint32_t           magic;
    size_t             size;       // Size of usable payload in bytes
    bool               is_free;
    uint32_t           align_pad;  // Extra offset applied for aligned allocations
    struct heap_block *next;
    struct heap_block *prev;
} heap_block_t;

static uintptr_t    heap_start = 0;
static uintptr_t    heap_limit = 0;
static heap_block_t *head_block = NULL;
static size_t       total_heap_bytes = 0;
static size_t       allocated_bytes = 0;

void heap_init(uintptr_t base, size_t size) {
    // 64-byte align the heap start
    heap_start = (base + 63) & ~63;
    heap_limit = base + size;
    if (heap_limit <= heap_start + sizeof(heap_block_t) + HEAP_MIN_BLOCK) {
        log_error("HEAP", "Heap initialization range invalid!");
        return;
    }

    total_heap_bytes = heap_limit - heap_start;
    allocated_bytes = 0;

    head_block = (heap_block_t *)heap_start;
    head_block->magic = HEAP_BLOCK_MAGIC;
    head_block->size = total_heap_bytes - sizeof(heap_block_t);
    head_block->is_free = true;
    head_block->align_pad = 0;
    head_block->next = NULL;
    head_block->prev = NULL;

    log_info("HEAP", "Freestanding Heap Active: 0x%08X - 0x%08X (%u MiB)",
             (uint32_t)heap_start, (uint32_t)heap_limit, (uint32_t)(total_heap_bytes / (1024 * 1024)));
}

void *heap_malloc(size_t size) {
    if (size == 0) return NULL;

    // 8-byte align requested size
    size_t aligned_req = (size + 7) & ~7;

    heap_block_t *curr = head_block;
    while (curr) {
        if (curr->magic != HEAP_BLOCK_MAGIC) {
            log_error("HEAP", "Heap corruption detected at block 0x%08X!", (uint32_t)curr);
            return NULL;
        }

        if (curr->is_free && curr->size >= aligned_req) {
            // Check if block can be split
            if (curr->size >= aligned_req + sizeof(heap_block_t) + HEAP_MIN_BLOCK) {
                heap_block_t *new_block = (heap_block_t *)((uintptr_t)curr + sizeof(heap_block_t) + aligned_req);
                new_block->magic = HEAP_BLOCK_MAGIC;
                new_block->size = curr->size - aligned_req - sizeof(heap_block_t);
                new_block->is_free = true;
                new_block->align_pad = 0;
                new_block->next = curr->next;
                new_block->prev = curr;

                if (curr->next) {
                    curr->next->prev = new_block;
                }
                curr->next = new_block;
                curr->size = aligned_req;
            }

            curr->is_free = false;
            curr->align_pad = 0;
            allocated_bytes += curr->size;
            return (void *)((uintptr_t)curr + sizeof(heap_block_t));
        }

        curr = curr->next;
    }

    log_error("HEAP", "Out of heap memory! (requested %u bytes, in-use %u / %u bytes)",
              (uint32_t)size, (uint32_t)allocated_bytes, (uint32_t)total_heap_bytes);
    return NULL;
}

void *heap_malloc_aligned(size_t size, size_t alignment) {
    if (alignment <= 8) return heap_malloc(size);

    // Over-allocate to guarantee alignment offset
    size_t extra = alignment + sizeof(heap_block_t);
    size_t total_needed = size + extra;

    void *raw = heap_malloc(total_needed);
    if (!raw) return NULL;

    uintptr_t raw_addr = (uintptr_t)raw;
    uintptr_t aligned_addr = (raw_addr + (alignment - 1)) & ~(alignment - 1);
    uint32_t pad = (uint32_t)(aligned_addr - raw_addr);

    heap_block_t *block = (heap_block_t *)(raw_addr - sizeof(heap_block_t));
    block->align_pad = pad;

    return (void *)aligned_addr;
}

void heap_free(void *ptr) {
    if (!ptr) return;

    uintptr_t p = (uintptr_t)ptr;
    if (p < heap_start || p >= heap_limit) {
        log_error("HEAP", "Attempted to free non-heap pointer: 0x%08X", (uint32_t)p);
        return;
    }

    // Recover block header (handle any aligned padding)
    heap_block_t *block = NULL;
    // Probe backwards if aligned
    for (uint32_t back = 0; back <= 4096; back += 4) {
        heap_block_t *cand = (heap_block_t *)(p - sizeof(heap_block_t) - back);
        if ((uintptr_t)cand >= heap_start && cand->magic == HEAP_BLOCK_MAGIC) {
            if (cand->align_pad == back && !cand->is_free) {
                block = cand;
                break;
            }
        }
    }

    if (!block || block->magic != HEAP_BLOCK_MAGIC) {
        log_error("HEAP", "heap_free: invalid or corrupt block header for 0x%08X", (uint32_t)p);
        return;
    }

    if (block->is_free) {
        log_error("HEAP", "heap_free: double free detected at 0x%08X", (uint32_t)p);
        return;
    }

    block->is_free = true;
    if (allocated_bytes >= block->size) {
        allocated_bytes -= block->size;
    } else {
        allocated_bytes = 0;
    }

    // Coalesce with next block if free
    if (block->next && block->next->is_free && block->next->magic == HEAP_BLOCK_MAGIC) {
        block->size += sizeof(heap_block_t) + block->next->size;
        block->next = block->next->next;
        if (block->next) {
            block->next->prev = block;
        }
    }

    // Coalesce with prev block if free
    if (block->prev && block->prev->is_free && block->prev->magic == HEAP_BLOCK_MAGIC) {
        block->prev->size += sizeof(heap_block_t) + block->size;
        block->prev->next = block->next;
        if (block->next) {
            block->next->prev = block->prev;
        }
    }
}

void *heap_calloc(size_t num, size_t size) {
    size_t total = num * size;
    void *ptr = heap_malloc(total);
    if (ptr) {
        uint8_t *b = (uint8_t *)ptr;
        for (size_t i = 0; i < total; i++) b[i] = 0;
    }
    return ptr;
}

void *heap_realloc(void *ptr, size_t new_size) {
    if (!ptr) return heap_malloc(new_size);
    if (new_size == 0) {
        heap_free(ptr);
        return NULL;
    }

    uintptr_t p = (uintptr_t)ptr;
    heap_block_t *block = (heap_block_t *)(p - sizeof(heap_block_t));
    if (block->magic != HEAP_BLOCK_MAGIC) {
        return NULL;
    }

    if (block->size >= new_size) {
        return ptr;
    }

    void *new_ptr = heap_malloc(new_size);
    if (!new_ptr) return NULL;

    uint8_t *dst = (uint8_t *)new_ptr;
    uint8_t *src = (uint8_t *)ptr;
    size_t to_copy = block->size < new_size ? block->size : new_size;
    for (size_t i = 0; i < to_copy; i++) dst[i] = src[i];

    heap_free(ptr);
    return new_ptr;
}

size_t heap_get_free_bytes(void) {
    return total_heap_bytes > allocated_bytes ? (total_heap_bytes - allocated_bytes) : 0;
}

size_t heap_get_used_bytes(void) {
    return allocated_bytes;
}
