#include "fat_source.h"
#include "../core/printf.h"
#include "../memory/memory.h"
#include <stddef.h>

#pragma pack(push, 1)

typedef struct fat32_bpb {
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
} fat32_bpb_t;

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

typedef struct fat_fs {
    block_read_fn read_fn;
    void         *priv;
    uint32_t      part_lba;

    uint16_t      bytes_per_sector;
    uint8_t       sectors_per_cluster;
    uint32_t      bytes_per_cluster;
    uint32_t      fat_start_lba;
    uint32_t      data_start_lba;
    uint32_t      root_cluster;

    // Open file state
    bool          file_open;
    uint32_t      file_start_cluster;
    uint32_t      file_cur_cluster;
    uint32_t      file_size;
    uint32_t      file_pos;

    // Cache buffers
    uint8_t       sector_buf[512];
    uint8_t       cluster_buf[4096];
} fat_fs_t;

static uint32_t get_next_cluster(fat_fs_t *fs, uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fs->fat_start_lba + (fat_offset / fs->bytes_per_sector);
    uint32_t ent_offset = fat_offset % fs->bytes_per_sector;

    if (fs->read_fn(fs->priv, fat_sector, 1, fs->sector_buf) != 0) {
        return 0x0FFFFFFF; // Error / End of chain
    }

    uint32_t next = *(uint32_t *)(&fs->sector_buf[ent_offset]) & 0x0FFFFFFF;
    return next;
}

static uint32_t cluster_to_lba(fat_fs_t *fs, uint32_t cluster) {
    return fs->data_start_lba + (cluster - 2) * fs->sectors_per_cluster;
}

static void to_83_name(const char *src, char *dst) {
    for (int i = 0; i < 11; i++) dst[i] = ' ';

    int i = 0;
    while (*src && *src != '.' && i < 8) {
        char c = *src++;
        if (c >= 'a' && c <= 'z') c -= 32;
        dst[i++] = c;
    }
    while (*src && *src != '.') src++;
    if (*src == '.') src++;

    i = 8;
    while (*src && i < 11) {
        char c = *src++;
        if (c >= 'a' && c <= 'z') c -= 32;
        dst[i++] = c;
    }
}

static int fat_open(boot_source_t *src, const char *path) {
    fat_fs_t *fs = (fat_fs_t *)src->priv;
    if (!fs || !path) return -1;

    // Skip leading slash
    if (*path == '/') path++;

    char search_83[11];
    to_83_name(path, search_83);

    uint32_t cur_cluster = fs->root_cluster;

    while (cur_cluster < 0x0FFFFFF8 && cur_cluster >= 2) {
        uint32_t lba = cluster_to_lba(fs, cur_cluster);
        for (uint8_t sec = 0; sec < fs->sectors_per_cluster; sec++) {
            if (fs->read_fn(fs->priv, lba + sec, 1, fs->sector_buf) != 0) {
                return -2;
            }

            fat_dir_entry_t *entries = (fat_dir_entry_t *)fs->sector_buf;
            for (int e = 0; e < 16; e++) {
                if ((uint8_t)entries[e].name[0] == 0x00) {
                    return -3; // No more entries
                }
                if ((uint8_t)entries[e].name[0] == 0xE5) {
                    continue; // Deleted entry
                }
                if (entries[e].attr == 0x0F) {
                    continue; // LFN entry
                }

                bool match = true;
                for (int c = 0; c < 11; c++) {
                    if (entries[e].name[c] != search_83[c]) {
                        match = false;
                        break;
                    }
                }

                if (match) {
                    fs->file_open = true;
                    fs->file_start_cluster = ((uint32_t)entries[e].first_cluster_hi << 16) | entries[e].first_cluster_lo;
                    fs->file_cur_cluster = fs->file_start_cluster;
                    fs->file_size = entries[e].file_size;
                    fs->file_pos = 0;
                    return 0; // File opened
                }
            }
        }
        cur_cluster = get_next_cluster(fs, cur_cluster);
    }
    return -4; // Not found
}

static uint32_t fat_read(boot_source_t *src, void *buf, uint32_t size) {
    fat_fs_t *fs = (fat_fs_t *)src->priv;
    if (!fs || !fs->file_open || !buf) return 0;

    if (fs->file_pos + size > fs->file_size) {
        size = fs->file_size - fs->file_pos;
    }
    if (size == 0) return 0;

    uint8_t *out = (uint8_t *)buf;
    uint32_t bytes_read = 0;

    while (bytes_read < size && fs->file_cur_cluster < 0x0FFFFFF8) {
        uint32_t cluster_offset = fs->file_pos % fs->bytes_per_cluster;
        uint32_t to_copy = fs->bytes_per_cluster - cluster_offset;
        if (to_copy > (size - bytes_read)) to_copy = size - bytes_read;

        uint32_t lba = cluster_to_lba(fs, fs->file_cur_cluster);
        if (fs->read_fn(fs->priv, lba, fs->sectors_per_cluster, fs->cluster_buf) != 0) {
            break;
        }

        for (uint32_t i = 0; i < to_copy; i++) {
            out[bytes_read + i] = fs->cluster_buf[cluster_offset + i];
        }

        bytes_read += to_copy;
        fs->file_pos += to_copy;

        if ((fs->file_pos % fs->bytes_per_cluster) == 0) {
            fs->file_cur_cluster = get_next_cluster(fs, fs->file_cur_cluster);
        }
    }
    return bytes_read;
}

static int fat_seek(boot_source_t *src, uint64_t offset) {
    fat_fs_t *fs = (fat_fs_t *)src->priv;
    if (!fs || !fs->file_open) return -1;
    if (offset > fs->file_size) offset = fs->file_size;

    fs->file_cur_cluster = fs->file_start_cluster;
    fs->file_pos = 0;

    uint32_t clusters_to_skip = ((uint32_t)offset) / fs->bytes_per_cluster;
    for (uint32_t c = 0; c < clusters_to_skip; c++) {
        fs->file_cur_cluster = get_next_cluster(fs, fs->file_cur_cluster);
        if (fs->file_cur_cluster >= 0x0FFFFFF8) break;
    }
    fs->file_pos = (uint32_t)offset;
    return 0;
}

static uint64_t fat_tell(boot_source_t *src) {
    fat_fs_t *fs = (fat_fs_t *)src->priv;
    return fs ? fs->file_pos : 0;
}

static uint64_t fat_size(boot_source_t *src) {
    fat_fs_t *fs = (fat_fs_t *)src->priv;
    return fs ? fs->file_size : 0;
}

static void fat_close(boot_source_t *src) {
    fat_fs_t *fs = (fat_fs_t *)src->priv;
    if (fs) {
        fs->file_open = false;
    }
}

boot_source_t *boot_source_fat_create(block_read_fn read_fn, void *priv, uint32_t partition_start_lba) {
    if (!read_fn) return NULL;

    fat_fs_t *fs = (fat_fs_t *)kmalloc(sizeof(fat_fs_t));
    if (!fs) return NULL;

    fs->read_fn = read_fn;
    fs->priv = priv;
    fs->part_lba = partition_start_lba;
    fs->file_open = false;

    // Read VBR (Volume Boot Record / BPB)
    if (read_fn(priv, partition_start_lba, 1, fs->sector_buf) != 0) {
        log_error("FAT", "Failed to read VBR at LBA %u", partition_start_lba);
        return NULL;
    }

    fat32_bpb_t *bpb = (fat32_bpb_t *)fs->sector_buf;
    fs->bytes_per_sector = bpb->bytes_per_sector ? bpb->bytes_per_sector : 512;
    fs->sectors_per_cluster = bpb->sectors_per_cluster ? bpb->sectors_per_cluster : 8;
    fs->bytes_per_cluster = fs->bytes_per_sector * fs->sectors_per_cluster;
    fs->fat_start_lba = partition_start_lba + bpb->reserved_sector_count;
    fs->data_start_lba = fs->fat_start_lba + (bpb->num_fats * bpb->fat_size_32);
    fs->root_cluster = bpb->root_cluster ? bpb->root_cluster : 2;

    log_info("FAT", "FAT32 Volume Mounted at LBA %u (Cluster Size: %u B, Root Cluster: %u)",
             partition_start_lba, fs->bytes_per_cluster, fs->root_cluster);

    boot_source_t *src = (boot_source_t *)kmalloc(sizeof(boot_source_t));
    if (!src) return NULL;

    src->name = "FAT32/SD";
    src->open = fat_open;
    src->read = fat_read;
    src->seek = fat_seek;
    src->tell = fat_tell;
    src->size = fat_size;
    src->close = fat_close;
    src->priv = fs;

    return src;
}
