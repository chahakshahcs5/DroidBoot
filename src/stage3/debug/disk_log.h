#ifndef DISK_LOG_H
#define DISK_LOG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../include/boot.h"

#define DISK_LOG_BUFFER_SIZE    65536   // 64 KiB
#define RAW_LOG_MAGIC_1         0x544F4F42  // "BOOT"
#define RAW_LOG_MAGIC_2         0x21474F4C  // "LOG!"
#define RAW_LOG_LBA             256
#define RAW_LOG_SECTORS         128     // 64 KiB

typedef struct disk_log_header {
    uint32_t magic1;        // "BOOT" (0x544F4F42)
    uint32_t magic2;        // "LOG!" (0x21474F4C)
    uint32_t log_length;    // Bytes in log buffer
    uint32_t flush_count;   // Monotonic flush counter
    uint32_t boot_drive;    // BIOS boot drive
    uint32_t error_count;   // Total flush errors encountered
    uint8_t  reserved[488]; // Pad to 512-byte sector
} __attribute__((packed)) disk_log_header_t;

// Initialize disk logging subsystem and resolve FAT32 BOOTLOG.TXT
void disk_log_init(boot_info_t *boot_info);

// Character sink called by printf
void disk_log_putc(char c);

// Flush in-memory log buffer to SD card (both FAT32 BOOTLOG.TXT and raw LBA 256)
void disk_log_flush(void);

// Flush with PC speaker audio feedback (beep on success, buzz on failure)
// Use this for critical checkpoints when screen is broken
void disk_log_flush_with_feedback(void);

// Register native USB Mass Storage device for persistent logging after xHCI takeover
void disk_log_register_usb_msc(void *usb_dev);

// Disable BIOS INT 13h disk access (called before xHCI reset to prevent hangs)
void disk_log_disable_bios(void);

// Get current log buffer stats
uint32_t disk_log_get_length(void);
const char *disk_log_get_buffer(void);

// Diagnostic stats
uint32_t disk_log_get_flush_count(void);
uint32_t disk_log_get_error_count(void);
bool     disk_log_last_flush_ok(void);

#endif // DISK_LOG_H
