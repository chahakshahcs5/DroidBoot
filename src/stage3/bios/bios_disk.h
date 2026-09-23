#ifndef BIOS_DISK_H
#define BIOS_DISK_H

#include <stdint.h>
#include <stddef.h>

#define BIOS_DAP_ADDR           0x00007E00
#define BIOS_BOUNCE_ADDR        0x00010000
#define BIOS_BOUNCE_SEG         0x1000
#define BIOS_BOUNCE_OFF         0x0000
#define BIOS_MAX_CHUNK_SECTORS  16
#define BIOS_SECTOR_SIZE        512

#define BIOS_CMD_READ_EXT       0x42
#define BIOS_CMD_WRITE_EXT      0x43

// BIOS Disk Address Packet (16 bytes)
typedef struct bios_dap {
    uint8_t  size;           // 0x10
    uint8_t  reserved;       // 0x00
    uint16_t sector_count;   // number of sectors (max 64)
    uint16_t buffer_offset;  // 0x0000
    uint16_t buffer_segment; // 0x1000
    uint64_t lba;            // starting LBA (64-bit)
} __attribute__((packed)) bios_dap_t;

// Thunk assembly interface
int bios_int13_call(uint8_t drive, uint32_t dap_phys, uint8_t cmd);
void bios_chainload(uint8_t drive_num, uint32_t boot_sector_phys) __attribute__((noreturn));

// High-level disk read/write functions
int bios_disk_read(uint8_t drive, uint64_t lba, uint16_t count, void *dst);
int bios_disk_write(uint8_t drive, uint64_t lba, uint16_t count, const void *src);

#endif // BIOS_DISK_H
