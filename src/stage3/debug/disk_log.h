#ifndef DISK_LOG_H
#define DISK_LOG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../include/boot.h"

#define DISK_LOG_BUFFER_SIZE    131072  // 128 KiB
#define RAW_LOG_MAGIC_1         0x544F4F42  // "BOOT"
#define RAW_LOG_MAGIC_2         0x21474F4C  // "LOG!"
#define RAW_LOG_LBA             1024
#define RAW_LOG_SECTORS         128     // 64 KiB

typedef enum {
    LOG_STATE_BIOS = 0,     // Early boot: safe to use BIOS INT 13h
    LOG_STATE_BUFFERED = 1, // Transitional: xHCI handover in progress, buffer in RAM only (NO BIOS calls!)
    LOG_STATE_USB_MSC = 2   // xHCI active: direct hardware USB Mass Storage (SCSI BOT)
} disk_log_state_t;

typedef struct disk_log_header {
    uint32_t magic1;        // "BOOT" (0x544F4F42)
    uint32_t magic2;        // "LOG!" (0x21474F4C)
    uint32_t log_length;    // Bytes in log buffer (up to DISK_LOG_BUFFER_SIZE)
    uint32_t flush_count;   // Monotonic flush counter
    uint32_t boot_drive;    // BIOS boot drive
    uint32_t error_count;   // Total flush errors encountered
    uint32_t log_start;     // Start offset in circular buffer (0 if not wrapped)
    uint32_t total_written; // Total bytes ever written (monotonic, for reader tools)
    uint32_t boot_count;    // Monotonic boot session counter (1, 2, 3...)
    uint8_t  session_name[16]; // e.g. "BOOT0001.LOG\0"
    uint8_t  reserved[460]; // Pad to 512-byte sector
} __attribute__((packed)) disk_log_header_t;

// Initialize disk logging subsystem and resolve FAT32 BOOTLOG.TXT & per-boot session file
void disk_log_init(boot_info_t *boot_info);

// Record Stage 1 and Stage 2 boot parameters into log buffer
void disk_log_record_early_boot(boot_info_t *boot_info);

// Character sink called by printf
void disk_log_putc(char c);

// Flush in-memory log buffer to disk (both latest BOOTLOG.TXT and dedicated BOOTxxxx.LOG)
void disk_log_flush(void);

// Flush with PC speaker audio feedback (beep on success, buzz on failure)
void disk_log_flush_with_feedback(void);

// Auto-flush if enough new data has accumulated since last flush (≥4KB)
void disk_log_auto_flush_if_needed(void);

// Register native USB Mass Storage device for persistent logging after xHCI takeover
void disk_log_register_usb_msc(void *usb_dev);

// Disable BIOS INT 13h disk access (called before xHCI reset to prevent hangs)
void disk_log_disable_bios(void);

// Re-enable BIOS fallback if boot drive was not on xHCI USB (e.g. IDE in QEMU)
void disk_log_enable_bios_fallback(void);

// Get current logging backend state
disk_log_state_t disk_log_get_state(void);

// Get current boot session id and file name (e.g. 1, "BOOT0001.LOG")
uint32_t disk_log_get_session_id(void);
const char *disk_log_get_session_filename(void);

// Get current log buffer stats
uint32_t disk_log_get_length(void);
uint32_t disk_log_get_total_written(void);
const char *disk_log_get_buffer(void);
void     disk_log_copy_linear(char *dst, uint32_t max_len, uint32_t *actual_len);

// Phone sync hook for immediate log delivery upon errors and checkpoints
typedef void (*disk_log_phone_sync_fn)(void);
void disk_log_set_phone_sync_hook(disk_log_phone_sync_fn fn);
void disk_log_trigger_phone_sync(void);

// Diagnostic stats
uint32_t disk_log_get_flush_count(void);
uint32_t disk_log_get_error_count(void);
bool     disk_log_last_flush_ok(void);

#endif // DISK_LOG_H
