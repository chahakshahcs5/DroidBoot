#ifndef BOOT_H
#define BOOT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define E820_TYPE_USABLE        1
#define E820_TYPE_RESERVED      2
#define E820_TYPE_ACPI_RECLAIM  3
#define E820_TYPE_ACPI_NVS      4
#define E820_TYPE_BAD_MEMORY    5

#pragma pack(push, 1)

typedef struct e820_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi_attrs;
} e820_entry_t;

typedef struct boot_info {
    uint8_t  boot_drive;
    uint8_t  reserved[3];
    uint32_t e820_count;
    uint32_t e820_map_addr;
} boot_info_t;

#pragma pack(pop)

#endif // BOOT_H
