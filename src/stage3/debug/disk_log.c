#include "disk_log.h"
#include "../bios/bios_disk.h"
#include "../core/printf.h"

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

static char     log_buffer[DISK_LOG_BUFFER_SIZE];
static uint32_t log_pos = 0;
static bool     log_initialized = false;
static bool     is_flushing = false;
static uint8_t  disk_boot_drive = 0x80;

static bool     bootlog_fat_resolved = false;
static uint32_t bootlog_file_lba = 0;
static uint32_t bootlog_dir_lba = 0;
static uint32_t bootlog_dir_offset = 0;
static uint32_t flush_counter = 0;

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

void disk_log_putc(char c) {
    if (c == '\r') return; // Handled with \n

    if (c == '\n') {
        if (log_pos + 2 < DISK_LOG_BUFFER_SIZE) {
            log_buffer[log_pos++] = '\r';
            log_buffer[log_pos++] = '\n';
        }
        return;
    }

    if (log_pos + 1 < DISK_LOG_BUFFER_SIZE) {
        log_buffer[log_pos++] = c;
    }
}

uint32_t disk_log_get_length(void) {
    return log_pos;
}

const char *disk_log_get_buffer(void) {
    return log_buffer;
}

void disk_log_init(boot_info_t *boot_info) {
    disk_boot_drive = boot_info ? boot_info->boot_drive : 0x80;
    bootlog_fat_resolved = false;
    bootlog_file_lba = 0;
    bootlog_dir_lba = 0;
    bootlog_dir_offset = 0;

    static uint8_t sec_buf[512];

    // 1. Read MBR at LBA 0 to find Partition 1
    uint32_t part1_lba = 2048; // Default 1 MiB alignment
    if (bios_disk_read(disk_boot_drive, 0, 1, sec_buf) == 0) {
        if (sec_buf[510] == 0x55 && sec_buf[511] == 0xAA) {
            uint32_t mbr_lba = *(uint32_t *)(&sec_buf[0x1BE + 8]);
            if (mbr_lba > 0 && mbr_lba < 10000000) {
                part1_lba = mbr_lba;
            }
        }
    }

    // 2. Read VBR at Partition 1 LBA
    if (bios_disk_read(disk_boot_drive, part1_lba, 1, sec_buf) == 0) {
        fat32_vbr_t *vbr = (fat32_vbr_t *)sec_buf;
        if (vbr->bytes_per_sector == 512 &&
            vbr->sectors_per_cluster > 0 &&
            vbr->fat_size_32 > 0 &&
            vbr->root_cluster >= 2) {

            uint8_t  spc = vbr->sectors_per_cluster;
            uint16_t res_sec = vbr->reserved_sector_count;
            uint8_t  nfats = vbr->num_fats;
            uint32_t fat_sz = vbr->fat_size_32;
            uint32_t root_cl = vbr->root_cluster;

            uint32_t fat_start_lba = part1_lba + res_sec;
            uint32_t data_start_lba = fat_start_lba + (nfats * fat_sz);
            uint32_t root_lba = data_start_lba + (root_cl - 2) * spc;

            // Search root directory for "BOOTLOG TXT"
            const char target_83[11] = "BOOTLOG TXT";
            bool found = false;

            for (uint8_t s = 0; s < spc && !found; s++) {
                uint32_t cur_sec_lba = root_lba + s;
                if (bios_disk_read(disk_boot_drive, cur_sec_lba, 1, sec_buf) != 0) {
                    break;
                }

                fat_dir_entry_t *entries = (fat_dir_entry_t *)sec_buf;
                for (int e = 0; e < 16; e++) {
                    if ((uint8_t)entries[e].name[0] == 0x00) {
                        found = false;
                        s = spc; // Stop scanning
                        break;
                    }
                    if ((uint8_t)entries[e].name[0] == 0xE5 || entries[e].attr == 0x0F) {
                        continue;
                    }

                    if (k_memcmp(entries[e].name, target_83, 11) == 0) {
                        uint32_t first_cluster = ((uint32_t)entries[e].first_cluster_hi << 16) | entries[e].first_cluster_lo;
                        bootlog_file_lba = data_start_lba + (first_cluster - 2) * spc;
                        bootlog_dir_lba = cur_sec_lba;
                        bootlog_dir_offset = e * sizeof(fat_dir_entry_t);
                        bootlog_fat_resolved = true;
                        found = true;
                        break;
                    }
                }
            }
        }
    }

    log_initialized = true;

    if (bootlog_fat_resolved) {
        log_info("LOG", "Persistent log target: FAT32 BOOTLOG.TXT (LBA %u, Drive 0x%02X) + Raw LBA 256",
                 bootlog_file_lba, disk_boot_drive);
    } else {
        log_info("LOG", "Persistent log target: Raw SD Sectors (LBA 256..383, Drive 0x%02X)", disk_boot_drive);
    }

    // Flush initial banner to disk immediately
    disk_log_flush();
}

void disk_log_flush(void) {
    if (!log_initialized || is_flushing) return;
    is_flushing = true;

    flush_counter++;

    // 1. Write Raw Log Header & Text to LBA 256..383
    static uint8_t hdr_buf[512];
    k_memset(hdr_buf, 0, sizeof(hdr_buf));
    disk_log_header_t *hdr = (disk_log_header_t *)hdr_buf;
    hdr->magic1 = RAW_LOG_MAGIC_1;
    hdr->magic2 = RAW_LOG_MAGIC_2;
    hdr->log_length = log_pos;
    hdr->flush_count = flush_counter;
    hdr->boot_drive = disk_boot_drive;

    bios_disk_write(disk_boot_drive, RAW_LOG_LBA, 1, hdr_buf);

    uint16_t text_sectors = (log_pos + 511) / 512;
    if (text_sectors > (RAW_LOG_SECTORS - 1)) {
        text_sectors = RAW_LOG_SECTORS - 1;
    }
    if (text_sectors > 0) {
        bios_disk_write(disk_boot_drive, RAW_LOG_LBA + 1, text_sectors, log_buffer);
    }

    // 2. Write to FAT32 BOOTLOG.TXT file sectors & update file size in directory
    if (bootlog_fat_resolved && bootlog_file_lba > 0) {
        uint16_t fat_sectors = (log_pos + 511) / 512;
        if (fat_sectors > 128) fat_sectors = 128; // Max 64KB
        if (fat_sectors > 0) {
            bios_disk_write(disk_boot_drive, bootlog_file_lba, fat_sectors, log_buffer);
        }

        // Update directory entry file_size
        if (bootlog_dir_lba > 0) {
            static uint8_t dir_buf[512];
            if (bios_disk_read(disk_boot_drive, bootlog_dir_lba, 1, dir_buf) == 0) {
                fat_dir_entry_t *ent = (fat_dir_entry_t *)&dir_buf[bootlog_dir_offset];
                ent->file_size = log_pos;
                bios_disk_write(disk_boot_drive, bootlog_dir_lba, 1, dir_buf);
            }
        }
    }

    is_flushing = false;
}
