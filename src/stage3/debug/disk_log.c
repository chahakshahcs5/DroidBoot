#include "disk_log.h"
#include "../bios/bios_disk.h"
#include "../usb/usb_msc.h"
#include "../core/printf.h"
#include "sound.h"

#pragma pack(push, 1)

typedef struct fat32_vbr {
    uint8_t  jmp_boot[3];
    char     oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t  num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t  media;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;

    // Extended FAT32 fields
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];
} fat32_vbr_t;

typedef struct fat_dir_entry {
    char     name[11];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t first_cluster_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t first_cluster_lo;
    uint32_t file_size;
} fat_dir_entry_t;

#pragma pack(pop)

static char             log_buffer[DISK_LOG_BUFFER_SIZE];
static uint32_t         log_pos = 0;
static uint32_t         log_start = 0;           // Start of valid data in circular buffer
static uint32_t         total_written = 0;       // Monotonic total bytes ever written
static bool             log_wrapped = false;     // True if buffer has wrapped around
static bool             log_initialized = false;
static bool             is_flushing = false;
static uint8_t          disk_boot_drive = 0x80;
static disk_log_state_t log_state = LOG_STATE_BIOS;

// Pointer to active USB MSC device (maintained in main.c, never copied to avoid ring desync)
static usb_device_t    *msc_log_dev = NULL;

// FAT32 File Targets
static bool             bootlog_fat_resolved = false;
static uint32_t         bootlog_file_lba = 0;
static uint32_t         bootlog_dir_lba = 0;
static uint32_t         bootlog_dir_offset = 0;

static bool             session_fat_resolved = false;
static uint32_t         session_file_lba = 0;
static uint32_t         session_dir_lba = 0;
static uint32_t         session_dir_offset = 0;
static uint32_t         boot_session_id = 1;
static char             session_filename[16] = "BOOT0001.LOG";

static uint32_t         flush_counter = 0;
static uint32_t         error_counter = 0;
static bool             last_flush_ok = false;
static uint32_t         bytes_at_last_flush = 0;

static int k_memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) return p1[i] - p2[i];
    }
    return 0;
}

static void k_memset(void *dst, uint8_t val, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = val;
}

static void k_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static void disk_log_putc_raw(char c) {
    log_buffer[log_pos] = c;
    log_pos = (log_pos + 1) % DISK_LOG_BUFFER_SIZE;
    total_written++;
    if (log_wrapped || log_pos == 0) {
        log_wrapped = true;
        log_start = log_pos;
    }
}

void disk_log_putc(char c) {
    if (c == '\r') return; // Handled with \n

    if (c == '\n') {
        disk_log_putc_raw('\r');
        disk_log_putc_raw('\n');
        return;
    }

    disk_log_putc_raw(c);
}

uint32_t disk_log_get_length(void) {
    return log_wrapped ? DISK_LOG_BUFFER_SIZE : log_pos;
}

uint32_t disk_log_get_total_written(void) {
    return total_written;
}

const char *disk_log_get_buffer(void) {
    return log_buffer;
}

static disk_log_phone_sync_fn phone_sync_callback = NULL;
static bool in_phone_sync = false;

void disk_log_set_phone_sync_hook(disk_log_phone_sync_fn fn) {
    phone_sync_callback = fn;
}

void disk_log_trigger_phone_sync(void) {
    if (!IS_DEBUG_BUILD) return;
    if (phone_sync_callback && !in_phone_sync) {
        in_phone_sync = true;
        phone_sync_callback();
        in_phone_sync = false;
    }
}

void disk_log_record_early_boot(boot_info_t *boot_info) {
    if (!boot_info) return;

#if IS_DEBUG_BUILD
    log_info("BOOT", "=== BootManager DEBUG BUILD: Verbose Hardware Diagnostics & Phone Sync Active ===");
#else
    log_info("BOOT", "=== BootManager RELEASE BUILD ===");
#endif
    log_info("BOOT", "  Stage 1 MBR Boot Sector (0x7C00): OK");
    log_info("BOOT", "  BIOS Boot Drive: 0x%02X (%s)",
             boot_info->boot_drive,
             (boot_info->boot_drive >= 0x80) ? "Hard Disk / USB / SD" : "Floppy");
    log_info("BOOT", "  Stage 2 Bootstrap (0x8000): A20 Gate Enabled, GDT Loaded");
    log_info("BOOT", "  CPU Mode: 32-bit Flat Protected Mode (CR0.PE = 1, CS=0x08, DS=0x10)");
    log_info("BOOT", "  E820 System Memory Map Probed: %u entries preserved at 0x%08X",
             boot_info->e820_count, boot_info->e820_map_addr);

    if (boot_info->e820_map_addr && boot_info->e820_count > 0) {
        e820_entry_t *entries = (e820_entry_t *)(uintptr_t)boot_info->e820_map_addr;
        uint64_t usable_bytes = 0;
        for (uint32_t i = 0; i < boot_info->e820_count && i < 128; i++) {
            const char *type_str = "UNKNOWN";
            switch (entries[i].type) {
                case 1: type_str = "USABLE RAM"; usable_bytes += entries[i].length; break;
                case 2: type_str = "RESERVED"; break;
                case 3: type_str = "ACPI RECLAIM"; break;
                case 4: type_str = "ACPI NVS"; break;
                case 5: type_str = "BAD RAM"; break;
                default: type_str = "RESERVED"; break;
            }
            (void)type_str;
#if IS_DEBUG_BUILD
            uint32_t base_hi = (uint32_t)(entries[i].base >> 32);
            uint32_t base_lo = (uint32_t)(entries[i].base & 0xFFFFFFFF);
            uint64_t end_addr = entries[i].base + entries[i].length;
            uint32_t end_hi = (uint32_t)(end_addr >> 32);
            uint32_t end_lo = (uint32_t)(end_addr & 0xFFFFFFFF);
            log_debug("E820", "[%02u] Base 0x%08X%08X - 0x%08X%08X (%u MB, Type %u: %s)",
                      i, base_hi, base_lo, end_hi, end_lo,
                      (uint32_t)(entries[i].length / 1024 / 1024),
                      entries[i].type, type_str);
#endif
        }
        uint32_t usable_mb = (uint32_t)(usable_bytes / 1024 / 1024);
        log_info("BOOT", "  Total Usable Physical RAM: %u MB (%u entries probed)", usable_mb, boot_info->e820_count);
    }
    log_info("BOOT", "  Stage 3 C Runtime Relocated to 0x00100000 (1 MiB boundary)");
    log_info("BOOT", "================================================");
}

void disk_log_copy_linear(char *dst, uint32_t max_len, uint32_t *actual_len) {
    if (!dst || max_len == 0) {
        if (actual_len) *actual_len = 0;
        return;
    }

    uint32_t len = log_wrapped ? DISK_LOG_BUFFER_SIZE : log_pos;
    if (len > max_len - 1) len = max_len - 1;

    if (log_wrapped) {
        uint32_t first_part = DISK_LOG_BUFFER_SIZE - log_start;
        if (first_part > len) first_part = len;
        k_memcpy(dst, log_buffer + log_start, first_part);
        uint32_t second_part = len - first_part;
        if (second_part > 0) {
            k_memcpy(dst + first_part, log_buffer, second_part);
        }
    } else {
        k_memcpy(dst, log_buffer, len);
    }
    dst[len] = '\0';
    if (actual_len) *actual_len = len;
}

uint32_t disk_log_get_flush_count(void) {
    return flush_counter;
}

uint32_t disk_log_get_error_count(void) {
    return error_counter;
}

bool disk_log_last_flush_ok(void) {
    return last_flush_ok;
}

disk_log_state_t disk_log_get_state(void) {
    return log_state;
}

uint32_t disk_log_get_session_id(void) {
    return boot_session_id;
}

const char *disk_log_get_session_filename(void) {
    return session_filename;
}

static int block_io_read(bool use_msc, uint8_t drive, uint32_t lba, uint16_t count, void *buf) {
    if (use_msc) {
        if (!msc_log_dev || !msc_log_dev->has_msc) return -1;
        return usb_msc_read_sectors(msc_log_dev, lba, count, buf);
    } else {
        return bios_disk_read(drive, lba, count, buf);
    }
}

static int block_io_write(bool use_msc, uint8_t drive, uint32_t lba, uint16_t count, const void *buf) {
    if (use_msc) {
        if (!msc_log_dev || !msc_log_dev->has_msc) return -1;
        return usb_msc_write_sectors(msc_log_dev, lba, count, buf);
    } else {
        return bios_disk_write(drive, lba, count, buf);
    }
}

static bool resolve_fat_targets(bool use_msc, uint8_t drive) {
    static uint8_t sec_buf[512];
    uint32_t part1_lba = 2048;

    // 1. Read MBR
    if (block_io_read(use_msc, drive, 0, 1, sec_buf) != 0) return false;
    if (sec_buf[510] == 0x55 && sec_buf[511] == 0xAA) {
        uint32_t mbr_lba = *(uint32_t *)(&sec_buf[0x1BE + 8]);
        if (mbr_lba > 0 && mbr_lba < 10000000) part1_lba = mbr_lba;
    }

    // 2. Read VBR
    if (block_io_read(use_msc, drive, part1_lba, 1, sec_buf) != 0) return false;
    fat32_vbr_t *vbr = (fat32_vbr_t *)sec_buf;
    if (vbr->bytes_per_sector != 512 || vbr->sectors_per_cluster == 0 ||
        vbr->fat_size_32 == 0 || vbr->root_cluster < 2) {
        return false;
    }

    uint8_t  spc = vbr->sectors_per_cluster;
    uint16_t res_sec = vbr->reserved_sector_count;
    uint8_t  nfats = vbr->num_fats;
    uint32_t fat_sz = vbr->fat_size_32;
    uint32_t root_cl = vbr->root_cluster;

    uint32_t fat_start_lba = part1_lba + res_sec;
    uint32_t data_start_lba = fat_start_lba + (nfats * fat_sz);
    uint32_t root_lba = data_start_lba + (root_cl - 2) * spc;

    uint32_t bootcnt_file_lba = 0;

    // First scan: find BOOTLOG.TXT and BOOTCNT.DAT
    const char target_bootlog[11] = "BOOTLOG TXT";
    const char target_bootcnt[11] = "BOOTCNT DAT";

    for (uint8_t s = 0; s < spc; s++) {
        uint32_t cur_sec_lba = root_lba + s;
        if (block_io_read(use_msc, drive, cur_sec_lba, 1, sec_buf) != 0) break;

        fat_dir_entry_t *entries = (fat_dir_entry_t *)sec_buf;
        for (int e = 0; e < 16; e++) {
            if ((uint8_t)entries[e].name[0] == 0x00) break;
            if ((uint8_t)entries[e].name[0] == 0xE5 || entries[e].attr == 0x0F) continue;

            if (k_memcmp(entries[e].name, target_bootlog, 11) == 0) {
                uint32_t first_cluster = ((uint32_t)entries[e].first_cluster_hi << 16) | entries[e].first_cluster_lo;
                if (first_cluster >= 2) {
                    bootlog_file_lba = data_start_lba + (first_cluster - 2) * spc;
                    bootlog_dir_lba = cur_sec_lba;
                    bootlog_dir_offset = e * sizeof(fat_dir_entry_t);
                    bootlog_fat_resolved = true;
                }
            } else if (k_memcmp(entries[e].name, target_bootcnt, 11) == 0) {
                uint32_t first_cluster = ((uint32_t)entries[e].first_cluster_hi << 16) | entries[e].first_cluster_lo;
                if (first_cluster >= 2) {
                    bootcnt_file_lba = data_start_lba + (first_cluster - 2) * spc;
                }
            }
        }
    }

    // 3. Resolve Boot Counter and increment for this session
    if (bootcnt_file_lba > 0) {
        if (block_io_read(use_msc, drive, bootcnt_file_lba, 1, sec_buf) == 0) {
            uint32_t cnt = *(uint32_t *)sec_buf;
            if (cnt == 0 || cnt > 1000000) cnt = 1;
            boot_session_id = cnt;
            // Increment and persist counter
            *(uint32_t *)sec_buf = cnt + 1;
            block_io_write(use_msc, drive, bootcnt_file_lba, 1, sec_buf);
        }
    }

    // Format dedicated session target name (rotate across slots 1..10)
    uint32_t slot = ((boot_session_id - 1) % 10) + 1;
    char target_session_83[11];
    target_session_83[0] = 'B';
    target_session_83[1] = 'O';
    target_session_83[2] = 'O';
    target_session_83[3] = 'T';
    target_session_83[4] = '0' + (slot / 1000) % 10;
    target_session_83[5] = '0' + (slot / 100) % 10;
    target_session_83[6] = '0' + (slot / 10) % 10;
    target_session_83[7] = '0' + (slot % 10);
    target_session_83[8] = 'L';
    target_session_83[9] = 'O';
    target_session_83[10] = 'G';

    session_filename[0] = 'B';
    session_filename[1] = 'O';
    session_filename[2] = 'O';
    session_filename[3] = 'T';
    session_filename[4] = '0' + (boot_session_id / 1000) % 10;
    session_filename[5] = '0' + (boot_session_id / 100) % 10;
    session_filename[6] = '0' + (boot_session_id / 10) % 10;
    session_filename[7] = '0' + (boot_session_id % 10);
    session_filename[8] = '.';
    session_filename[9] = 'L';
    session_filename[10] = 'O';
    session_filename[11] = 'G';
    session_filename[12] = '\0';

    // Second scan: find dedicated session log entry
    for (uint8_t s = 0; s < spc; s++) {
        uint32_t cur_sec_lba = root_lba + s;
        if (block_io_read(use_msc, drive, cur_sec_lba, 1, sec_buf) != 0) break;

        fat_dir_entry_t *entries = (fat_dir_entry_t *)sec_buf;
        for (int e = 0; e < 16; e++) {
            if ((uint8_t)entries[e].name[0] == 0x00) break;
            if ((uint8_t)entries[e].name[0] == 0xE5 || entries[e].attr == 0x0F) continue;

            if (k_memcmp(entries[e].name, target_session_83, 11) == 0) {
                uint32_t first_cluster = ((uint32_t)entries[e].first_cluster_hi << 16) | entries[e].first_cluster_lo;
                if (first_cluster >= 2) {
                    session_file_lba = data_start_lba + (first_cluster - 2) * spc;
                    session_dir_lba = cur_sec_lba;
                    session_dir_offset = e * sizeof(fat_dir_entry_t);
                    session_fat_resolved = true;
                    return bootlog_fat_resolved;
                }
            }
        }
    }

    return bootlog_fat_resolved;
}

void disk_log_init(boot_info_t *boot_info) {
    disk_boot_drive = boot_info ? boot_info->boot_drive : 0x80;
    bootlog_fat_resolved = false;
    session_fat_resolved = false;
    bootlog_file_lba = 0;
    session_file_lba = 0;
    msc_log_dev = NULL;
    log_state = LOG_STATE_BIOS;

    // 1. Try initial drive provided by BIOS
    if (!resolve_fat_targets(false, disk_boot_drive)) {
        // 2. Scan alternate BIOS drives (0x80..0x83) to locate BootManager partition
        uint8_t candidates[4] = {0x80, 0x81, 0x82, 0x83};
        for (int i = 0; i < 4; i++) {
            if (candidates[i] != disk_boot_drive) {
                if (resolve_fat_targets(false, candidates[i])) {
                    disk_boot_drive = candidates[i];
                    break;
                }
            }
        }
    }

    log_initialized = true;

    if (bootlog_fat_resolved) {
        log_info("LOG", "Persistent log target: BOOTLOG.TXT + %s (Boot #%u, Drive 0x%02X)",
                 session_filename, boot_session_id, disk_boot_drive);
    } else {
        log_info("LOG", "Persistent log target: Raw SD Sectors (LBA 1024..1151, Drive 0x%02X)", disk_boot_drive);
    }

    // Flush initial banner to disk immediately via BIOS INT 13h
    disk_log_flush();
}

void disk_log_disable_bios(void) {
    // Disengage BIOS disk writes before xHCI controller reset & SMM handover
    log_state = LOG_STATE_BUFFERED;
}

void disk_log_enable_bios_fallback(void) {
    if (log_state == LOG_STATE_USB_MSC) return; // USB MSC is already active and preferred

    static uint8_t test_buf[512];
    int res = bios_disk_read(disk_boot_drive, 0, 1, test_buf);
    if (res == 0) {
        log_state = LOG_STATE_BIOS;
        log_info("LOG", "BIOS disk fallback active on Drive 0x%02X.", disk_boot_drive);
        disk_log_flush();
        return;
    }

    // Try alternate BIOS drives (0x80..0x83)
    uint8_t candidates[4] = {0x80, 0x81, 0x82, 0x83};
    for (int i = 0; i < 4; i++) {
        if (candidates[i] != disk_boot_drive && bios_disk_read(candidates[i], 0, 1, test_buf) == 0) {
            disk_boot_drive = candidates[i];
            log_state = LOG_STATE_BIOS;
            log_info("LOG", "BIOS disk fallback active on alternate Drive 0x%02X.", disk_boot_drive);
            disk_log_flush();
            return;
        }
    }

    log_info("LOG", "BIOS disk fallback unavailable after xHCI takeover (Drive 0x%02X, code %d). Log buffered in RAM.",
             disk_boot_drive, res);
}

void disk_log_register_usb_msc(void *usb_dev) {
    if (!usb_dev) return;
    usb_device_t *dev = (usb_device_t *)usb_dev;
    if (!dev->has_msc) return;

    // CRITICAL: Hold pointer directly, never copy by value, preventing transfer ring desync!
    msc_log_dev = dev;
    log_state = LOG_STATE_USB_MSC;

    // Resolve FAT32 filesystem structures directly over native xHCI USB Mass Storage
    resolve_fat_targets(true, 0);

    log_info("LOG", "Native USB Mass Storage registered for logging (Port %u, File LBA %u, Session: %s)",
             msc_log_dev->port_num, bootlog_file_lba, session_filename);

    // Flush all accumulated logs (including xHCI reset and USB enumeration logs) in one fast batch!
    disk_log_flush();
}

void disk_log_flush(void) {
    if (!IS_DEBUG_BUILD) return;
    if (!log_initialized || is_flushing) return;

    // In transitional state (during xHCI reset and SMM handover):
    // DO NOT invoke BIOS INT 13h to prevent SMM hangs on dead BIOS USB stacks!
    if (log_state == LOG_STATE_BUFFERED) return;

    is_flushing = true;
    flush_counter++;
    bool any_error = false;

    uint32_t actual_len = log_wrapped ? DISK_LOG_BUFFER_SIZE : log_pos;
    static char linear_buf[DISK_LOG_BUFFER_SIZE];

    // Linearize circular buffer so the file on disk is in chronological order
    if (log_wrapped) {
        uint32_t first_part = DISK_LOG_BUFFER_SIZE - log_start;
        k_memcpy(linear_buf, log_buffer + log_start, first_part);
        k_memcpy(linear_buf + first_part, log_buffer, log_start);
    } else {
        k_memcpy(linear_buf, log_buffer, log_pos);
    }

    // 1. Prepare Raw Log Header
    static uint8_t hdr_buf[512];
    k_memset(hdr_buf, 0, sizeof(hdr_buf));
    disk_log_header_t *hdr = (disk_log_header_t *)hdr_buf;
    hdr->magic1 = RAW_LOG_MAGIC_1;
    hdr->magic2 = RAW_LOG_MAGIC_2;
    hdr->log_length = actual_len;
    hdr->flush_count = flush_counter;
    hdr->boot_drive = disk_boot_drive;
    hdr->error_count = error_counter;
    hdr->log_start = 0; // Linearized
    hdr->total_written = total_written;
    hdr->boot_count = boot_session_id;
    k_memcpy(hdr->session_name, session_filename, 16);

    const char *disk_linear_ptr = linear_buf;
    uint32_t disk_actual_len = actual_len;
    if (disk_actual_len > 65536) {
        disk_linear_ptr = linear_buf + (disk_actual_len - 65536);
        disk_actual_len = 65536;
    }

    uint16_t text_sectors = (disk_actual_len + 511) / 512;
    if (text_sectors > (RAW_LOG_SECTORS - 1)) {
        text_sectors = RAW_LOG_SECTORS - 1;
    }

    uint16_t fat_sectors = (disk_actual_len + 511) / 512;
    if (fat_sectors > 128) fat_sectors = 128; // Max 64KB

    bool use_msc = (log_state == LOG_STATE_USB_MSC);
    uint8_t drive = disk_boot_drive;

    // A. Write Raw Backup Sectors (Header + Text at LBA 1024)
    int rerr = block_io_write(use_msc, drive, RAW_LOG_LBA, 1, hdr_buf);
    if (rerr == 0 && text_sectors > 0) {
        block_io_write(use_msc, drive, RAW_LOG_LBA + 1, text_sectors, disk_linear_ptr);
    } else if (rerr != 0) {
        any_error = true;
    }

    // B. Write Primary FAT32 BOOTLOG.TXT (Always holds the latest log)
    if (bootlog_fat_resolved && bootlog_file_lba > 0 && fat_sectors > 0) {
        int ferr = block_io_write(use_msc, drive, bootlog_file_lba, fat_sectors, disk_linear_ptr);
        if (ferr == 0) {
            if (bootlog_dir_lba > 0) {
                static uint8_t dir_buf[512];
                if (block_io_read(use_msc, drive, bootlog_dir_lba, 1, dir_buf) == 0) {
                    fat_dir_entry_t *ent = (fat_dir_entry_t *)&dir_buf[bootlog_dir_offset];
                    ent->file_size = disk_actual_len;
                    block_io_write(use_msc, drive, bootlog_dir_lba, 1, dir_buf);
                }
            }
        } else {
            any_error = true;
        }
    }

    // C. Write Dedicated Per-Boot Historical Log (BOOTxxxx.LOG)
    if (session_fat_resolved && session_file_lba > 0 && fat_sectors > 0) {
        int serr = block_io_write(use_msc, drive, session_file_lba, fat_sectors, disk_linear_ptr);
        if (serr == 0) {
            if (session_dir_lba > 0) {
                static uint8_t sdir_buf[512];
                if (block_io_read(use_msc, drive, session_dir_lba, 1, sdir_buf) == 0) {
                    fat_dir_entry_t *sent = (fat_dir_entry_t *)&sdir_buf[session_dir_offset];
                    sent->file_size = disk_actual_len;
                    block_io_write(use_msc, drive, session_dir_lba, 1, sdir_buf);
                }
            }
        } else {
            any_error = true;
        }
    }

    static uint8_t consecutive_msc_errors = 0;
    if (any_error) {
        error_counter++;
        if (use_msc) {
            consecutive_msc_errors++;
            if (consecutive_msc_errors >= 2) {
                log_state = LOG_STATE_BUFFERED;
            }
        }
    } else {
        consecutive_msc_errors = 0;
    }
    last_flush_ok = !any_error;
    bytes_at_last_flush = total_written;

    is_flushing = false;
}

void disk_log_auto_flush_if_needed(void) {
    if (!IS_DEBUG_BUILD) return;
    if (!log_initialized || is_flushing) return;
    if (log_state == LOG_STATE_BUFFERED) return;
    // Flush if >= 4KB has been written since last flush
    if (total_written - bytes_at_last_flush >= 4096) {
        disk_log_flush();
    }
}

void disk_log_flush_with_feedback(void) {
    if (!IS_DEBUG_BUILD) return;
    disk_log_flush();
    if (last_flush_ok) {
        sound_beep(1760, 40);
        for (int i = 0; i < 15000; i++) __asm__ volatile("nop");
        sound_beep(1760, 40);
    } else {
        sound_beep(330, 150);
    }
}
