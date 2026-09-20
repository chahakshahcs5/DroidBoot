#include "memory.h"
#include "../core/printf.h"

#define HEAP_START 0x00800000       // 8 MiB mark
#define HEAP_END   0x02000000       // 32 MiB mark (24 MiB total heap)

static uintptr_t heap_curr = HEAP_START;
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
    heap_curr = HEAP_START;

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
    log_info("MEMORY", "Dynamic Heap initialized: 0x%08X - 0x%08X (24 MiB)", HEAP_START, HEAP_END);
}

void *kmalloc_aligned(size_t size, size_t alignment) {
    if (alignment == 0) alignment = 16;
    uintptr_t aligned = (heap_curr + (alignment - 1)) & ~(alignment - 1);
    if (aligned + size > HEAP_END) {
        log_error("MEMORY", "Out of heap memory! (requested %u bytes, available %u bytes)",
                  (uint32_t)size, (uint32_t)(HEAP_END - heap_curr));
        return NULL;
    }
    heap_curr = aligned + size;
    return (void *)aligned;
}

void *kmalloc(size_t size) {
    return kmalloc_aligned(size, 16);
}

void kfree(void *ptr) {
    (void)ptr; // Bump allocator does not free individual chunks during early boot
}

uint64_t memory_get_total_usable(void) {
    return total_usable_ram;
}
