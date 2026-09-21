#include "fat_source.h"
#include "ff.h"
#include "diskio.h"
#include "../core/printf.h"
#include "../memory/memory.h"
#include <stddef.h>

typedef struct fat_fs {
    FATFS    fs;
    FIL      file;
    bool     file_open;
    uint8_t  pdrv;
    uint32_t part_lba;
} fat_fs_t;

static int fat_open(boot_source_t *src, const char *path) {
    fat_fs_t *fs = (fat_fs_t *)src->priv;
    if (!fs || !path) return -1;

    if (fs->file_open) {
        f_close(&fs->file);
        fs->file_open = false;
    }

    // Skip leading slash
    while (*path == '/' || *path == '\\') path++;

    char full_path[256];
    snprintf(full_path, sizeof(full_path), "%u:/%s", fs->pdrv, path);

    FRESULT res = f_open(&fs->file, full_path, FA_READ);
    if (res == FR_OK) {
        fs->file_open = true;
        return 0;
    }

    return (int)res;
}

static uint32_t fat_read(boot_source_t *src, void *buf, uint32_t size) {
    fat_fs_t *fs = (fat_fs_t *)src->priv;
    if (!fs || !fs->file_open || !buf || size == 0) return 0;

    uint32_t bytes_read = 0;
    FRESULT res = f_read(&fs->file, buf, size, &bytes_read);
    return (res == FR_OK) ? bytes_read : 0;
}

static int fat_seek(boot_source_t *src, uint64_t offset) {
    fat_fs_t *fs = (fat_fs_t *)src->priv;
    if (!fs || !fs->file_open) return -1;
    FRESULT res = f_lseek(&fs->file, (FSIZE_t)offset);
    return (res == FR_OK) ? 0 : -1;
}

static uint64_t fat_tell(boot_source_t *src) {
    fat_fs_t *fs = (fat_fs_t *)src->priv;
    return (fs && fs->file_open) ? (uint64_t)f_tell(&fs->file) : 0;
}

static uint64_t fat_size(boot_source_t *src) {
    fat_fs_t *fs = (fat_fs_t *)src->priv;
    return (fs && fs->file_open) ? (uint64_t)f_size(&fs->file) : 0;
}

static void fat_close(boot_source_t *src) {
    fat_fs_t *fs = (fat_fs_t *)src->priv;
    if (fs && fs->file_open) {
        f_close(&fs->file);
        fs->file_open = false;
    }
}

int boot_source_fat_scan_dir(boot_source_t *src, const char *dir_path, fat_file_callback_fn cb, void *user_data) {
    fat_fs_t *fs = (fat_fs_t *)src->priv;
    if (!fs || !cb) return -1;

    char full_dir[256];
    const char *p = dir_path ? dir_path : "";
    while (*p == '/' || *p == '\\') p++;

    if (*p) {
        snprintf(full_dir, sizeof(full_dir), "%u:/%s", fs->pdrv, p);
    } else {
        snprintf(full_dir, sizeof(full_dir), "%u:/", fs->pdrv);
    }

    DIR dp;
    FRESULT res = f_opendir(&dp, full_dir);
    if (res != FR_OK) return (int)res;

    FILINFO fno;
    while (f_readdir(&dp, &fno) == FR_OK && fno.fname[0]) {
        if (fno.fname[0] == '.') continue; // Skip . and ..
        bool is_dir = (fno.fattrib & AM_DIR) != 0;
        cb(fno.fname, fno.fsize, is_dir, user_data);
    }

    f_closedir(&dp);
    return 0;
}

boot_source_t *boot_source_fat_create(block_read_fn read_fn, void *priv, uint32_t partition_start_lba) {
    (void)read_fn;

    fat_fs_t *fs = (fat_fs_t *)kmalloc(sizeof(fat_fs_t));
    if (!fs) return NULL;

    fs->file_open = false;
    fs->part_lba = partition_start_lba;

    // Check drive: drive 0 is BIOS boot drive, drive 1 is USB MSC
    uint8_t boot_drive = (uint8_t)(uintptr_t)priv;
    if (boot_drive >= 0x80 && boot_drive <= 0x8F) {
        fs->pdrv = 0;
        diskio_set_bios_drive(boot_drive);
    } else {
        fs->pdrv = 1;
    }

    char vol_path[8];
    snprintf(vol_path, sizeof(vol_path), "%u:", fs->pdrv);

    FRESULT mres = f_mount(&fs->fs, vol_path, 1);
    if (mres != FR_OK) {
        log_error("FAT", "Failed to mount FAT filesystem on drive %u (result %d)", fs->pdrv, mres);
        kfree(fs);
        return NULL;
    }

    boot_source_t *src = (boot_source_t *)kmalloc(sizeof(boot_source_t));
    if (!src) {
        kfree(fs);
        return NULL;
    }

    src->name = "FAT32/FatFs";
    src->open = fat_open;
    src->read = fat_read;
    src->seek = fat_seek;
    src->tell = fat_tell;
    src->size = fat_size;
    src->close = fat_close;
    src->priv = fs;

    return src;
}

void boot_source_fat_destroy(boot_source_t *src) {
    if (!src) return;
    fat_fs_t *fs = (fat_fs_t *)src->priv;
    if (fs) {
        if (fs->file_open) {
            f_close(&fs->file);
        }
        char vol_path[8];
        snprintf(vol_path, sizeof(vol_path), "%u:", fs->pdrv);
        f_mount(NULL, vol_path, 0);
        kfree(fs);
    }
    kfree(src);
}
