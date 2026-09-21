#include "memory.h"
#include "heap.h"
#include "../core/printf.h"

#define HEAP_START 0x00800000       // 8 MiB mark
#define HEAP_SIZE  (24 * 1024 * 1024) // 24 MiB total heap up to 32 MiB

static uint64_t total_usable_ram = 0;

static const char *get_e820_type_name(uint32_t type) {
    switch (type) {
        case E820_TYPE_USABLE:       return "USABLE";
        case E820_TYPE_RESERVED:     return "RESERVED";
        case E820_TYPE_ACPI_RECLAIM: return "ACPI RECLAIM";
        case E820_TYPE_ACPI_NVS:     return "ACPI NVS";
        case E820_TYPE_BAD_MEMORY:   return "BAD MEMORY";
        default:                     return "UNKNOWN";
    }
}

void memory_init(boot_info_t *boot_info) {
    total_usable_ram = 0;

    if (!boot_info || boot_info->e820_count == 0) {
        log_error("MEMORY", "No E820 memory map entries provided by Stage 2!");
        return;
    }

    log_info("MEMORY", "E820 System Memory Map (%u entries):", boot_info->e820_count);

    e820_entry_t *entries = (e820_entry_t *)boot_info->e820_map_addr;
    for (uint32_t i = 0; i < boot_info->e820_count; i++) {
        uint64_t base = entries[i].base;
        uint64_t length = entries[i].length;
        uint32_t type = entries[i].type;

        uint32_t base_hi = (uint32_t)(base >> 32);
        uint32_t base_lo = (uint32_t)(base & 0xFFFFFFFF);

        if (type == E820_TYPE_USABLE) {
            total_usable_ram += length;
        }

        printk("  [E820 %2u] 0x%08X%08X - 0x%08X%08X (%u KB) [%s]\n",
               i,
               base_hi, base_lo,
               (uint32_t)((base + length) >> 32), (uint32_t)((base + length) & 0xFFFFFFFF),
               (uint32_t)(length / 1024),
               get_e820_type_name(type));
    }

    uint32_t ram_mb = (uint32_t)(total_usable_ram / (1024 * 1024));
    log_info("MEMORY", "Total Usable RAM: %u MiB", ram_mb);

    // Initialize the freestanding heap allocator
    heap_init(HEAP_START, HEAP_SIZE);
}

void *kmalloc_aligned(size_t size, size_t alignment) {
    return heap_malloc_aligned(size, alignment);
}

void *kmalloc(size_t size) {
    return heap_malloc(size);
}

void *kcalloc(size_t num, size_t size) {
    return heap_calloc(num, size);
}

void *krealloc(void *ptr, size_t new_size) {
    return heap_realloc(ptr, new_size);
}

void kfree(void *ptr) {
    heap_free(ptr);
}

uint64_t memory_get_total_usable(void) {
    return total_usable_ram;
}

size_t memory_get_free_heap(void) {
    return heap_get_free_bytes();
}

size_t memory_get_used_heap(void) {
    return heap_get_used_bytes();
}
