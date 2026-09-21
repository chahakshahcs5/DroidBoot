/*----------------------------------------------------------------------------/
/  FatFs - Generic FAT Filesystem Module  R0.15                               /
/-----------------------------------------------------------------------------/
/
/ Copyright (C) 2022, ChaN, all right reserved.
/
/ FatFs module is an open source software. Redistribution and use of FatFs in
/ source and binary forms, with or without modification, are permitted provided
/ that the following condition is met:
/
/ 1. Redistributions of source code must retain the above copyright notice,
/    this condition and the following disclaimer.
/
/ This software is provided by the copyright holders and contributors "AS IS"
/ and any express or implied warranties, including, but not limited to, the
/ implied warranties of merchantability and fitness for a particular purpose
/ are disclaimed.
/----------------------------------------------------------------------------*/

#include "ff.h"
#include "diskio.h"
#include "../memory/memory.h"
#include "../core/printf.h"
#include "../core/rtc.h"

/* Filesystem object pointer for each logical drive */
static FATFS *FatFs[FF_VOLUMES] = {0};

/* Volume mount ID counter */
static uint16_t FsId = 0;

/* Character conversion helpers */
static inline int ff_wtoupper(int c) {
    if (c >= 'a' && c <= 'z') return c - ('a' - 'A');
    return c;
}

#if FF_USE_LFN == 3
void* ff_memalloc(uint32_t msize) {
    return kmalloc(msize);
}
void ff_memfree(void* mblock) {
    kfree(mblock);
}
#endif

uint32_t get_fattime(void) {
    uint8_t sec  = cmos_read(0x00);
    uint8_t min  = cmos_read(0x02);
    uint8_t hour = cmos_read(0x04);
    uint8_t day  = cmos_read(0x07);
    uint8_t mon  = cmos_read(0x08);
    uint8_t yr   = cmos_read(0x09);
    uint8_t regb = cmos_read(0x0B);

    if (!(regb & 0x04)) {
        sec  = bcd_to_bin(sec);
        min  = bcd_to_bin(min);
        hour = bcd_to_bin(hour & 0x7F);
        day  = bcd_to_bin(day);
        mon  = bcd_to_bin(mon);
        yr   = bcd_to_bin(yr);
    }
    uint32_t year = 2000 + yr;
    if (year < 1980 || year > 2107 || mon < 1 || mon > 12 || day < 1 || day > 31) {
        return ((uint32_t)(2026 - 1980) << 25) | (1U << 21) | (1U << 16);
    }

    return ((uint32_t)(year - 1980) << 25)
         | ((uint32_t)mon << 21)
         | ((uint32_t)day << 16)
         | ((uint32_t)hour << 11)
         | ((uint32_t)min << 5)
         | ((uint32_t)(sec >> 1));
}

/* Load 16/32-bit little endian values */
static inline uint16_t ld_word(const uint8_t *ptr) {
    return (uint16_t)ptr[0] | ((uint16_t)ptr[1] << 8);
}

static inline uint32_t ld_dword(const uint8_t *ptr) {
    return (uint32_t)ptr[0] | ((uint32_t)ptr[1] << 8) | ((uint32_t)ptr[2] << 16) | ((uint32_t)ptr[3] << 24);
}

static inline void st_word(uint8_t *ptr, uint16_t val) {
    ptr[0] = (uint8_t)val;
    ptr[1] = (uint8_t)(val >> 8);
}

static inline void st_dword(uint8_t *ptr, uint32_t val) {
    ptr[0] = (uint8_t)val;
    ptr[1] = (uint8_t)(val >> 8);
    ptr[2] = (uint8_t)(val >> 16);
    ptr[3] = (uint8_t)(val >> 24);
}

/* Flush disk access window cache */
static FRESULT sync_window(FATFS *fs) {
    if (fs->wflag) {
        if (disk_write(fs->pdrv, fs->win, fs->winsect, 1) != RES_OK) {
            return FR_DISK_ERR;
        }
        fs->wflag = 0;
    }
    return FR_OK;
}

/* Move disk access window to specified sector */
static FRESULT move_window(FATFS *fs, LBA_t sect) {
    if (sect != fs->winsect) {
        FRESULT res = sync_window(fs);
        if (res != FR_OK) return res;
        if (disk_read(fs->pdrv, fs->win, sect, 1) != RES_OK) {
            return FR_DISK_ERR;
        }
        fs->winsect = sect;
    }
    return FR_OK;
}

/* Get next cluster in cluster chain */
static uint32_t get_fat(FATFS *fs, uint32_t clst) {
    if (clst < 2 || clst >= fs->n_fatent) return 1; // Invalid

    LBA_t fat_sect;
    uint32_t fat_offset;

    if (fs->fs_type == 3) { // FAT32
        fat_sect = fs->fatbase + (clst / (FF_MAX_SS / 4));
        fat_offset = (clst % (FF_MAX_SS / 4)) * 4;
        if (move_window(fs, fat_sect) != FR_OK) return 1;
        return ld_dword(&fs->win[fat_offset]) & 0x0FFFFFFF;
    } else if (fs->fs_type == 2) { // FAT16
        fat_sect = fs->fatbase + (clst / (FF_MAX_SS / 2));
        fat_offset = (clst % (FF_MAX_SS / 2)) * 2;
        if (move_window(fs, fat_sect) != FR_OK) return 1;
        return ld_word(&fs->win[fat_offset]);
    } else { // FAT12
        fat_offset = clst + (clst / 2);
        fat_sect = fs->fatbase + (fat_offset / FF_MAX_SS);
        fat_offset %= FF_MAX_SS;
        if (move_window(fs, fat_sect) != FR_OK) return 1;
        uint16_t val = fs->win[fat_offset];
        if (fat_offset == FF_MAX_SS - 1) {
            if (move_window(fs, fat_sect + 1) != FR_OK) return 1;
            val |= ((uint16_t)fs->win[0] << 8);
        } else {
            val |= ((uint16_t)fs->win[fat_offset + 1] << 8);
        }
        return (clst & 1) ? (val >> 4) : (val & 0x0FFF);
    }
}

/* Put FAT entry */
static FRESULT put_fat(FATFS *fs, uint32_t clst, uint32_t val) {
    if (clst < 2 || clst >= fs->n_fatent) return FR_INT_ERR;

    LBA_t fat_sect;
    uint32_t fat_offset;

    if (fs->fs_type == 3) { // FAT32
        fat_sect = fs->fatbase + (clst / (FF_MAX_SS / 4));
        fat_offset = (clst % (FF_MAX_SS / 4)) * 4;
        FRESULT res = move_window(fs, fat_sect);
        if (res != FR_OK) return res;
        val = (val & 0x0FFFFFFF) | (ld_dword(&fs->win[fat_offset]) & 0xF0000000);
        st_dword(&fs->win[fat_offset], val);
        fs->wflag = 1;
        for (uint8_t f = 1; f < fs->n_fats; f++) {
            disk_write(fs->pdrv, fs->win, fat_sect + (f * fs->fsize), 1);
        }
    }
    return FR_OK;
}

/* Convert cluster number to starting LBA */
static inline LBA_t clst2sect(FATFS *fs, uint32_t clst) {
    clst -= 2;
    if (clst >= fs->n_fatent - 2) return 0;
    return fs->database + (LBA_t)clst * fs->csize;
}

/* Check BPB boot record and determine filesystem type */
static uint8_t check_fs(FATFS *fs, LBA_t sect) {
    fs->wflag = 0;
    fs->winsect = 0xFFFFFFFF;
    if (move_window(fs, sect) != FR_OK) return 0;

    // Check boot signature 0xAA55
    if (ld_word(&fs->win[510]) != 0xAA55) return 0;

    // Check FAT signatures
    if (ld_word(&fs->win[11]) == 512 || ld_word(&fs->win[11]) == 1024 ||
        ld_word(&fs->win[11]) == 2048 || ld_word(&fs->win[11]) == 4096) {
        if (fs->win[38] == 0x29 || fs->win[66] == 0x29) {
            return 1; // Valid VBR
        }
    }
    return 0;
}

/* Mount a volume */
FRESULT f_mount(FATFS *fs, const TCHAR *path, uint8_t opt) {
    uint8_t pdrv = 0;
    if (path && path[0] >= '0' && path[0] <= '9') {
        pdrv = (uint8_t)(path[0] - '0');
    }
    if (pdrv >= FF_VOLUMES) return FR_INVALID_DRIVE;

    if (FatFs[pdrv]) {
        FatFs[pdrv]->fs_type = 0;
    }

    if (!fs) {
        FatFs[pdrv] = NULL;
        return FR_OK;
    }

    fs->fs_type = 0;
    fs->pdrv = pdrv;
    fs->wflag = 0;
    fs->winsect = 0xFFFFFFFF;
    fs->id = ++FsId;
    FatFs[pdrv] = fs;

    if (opt == 0) return FR_OK; // Force mount later

    if (disk_initialize(pdrv) & STA_NOINIT) {
        return FR_NOT_READY;
    }

    // Determine partition base: check sector 0 for MBR
    LBA_t bsect = 0;
    if (check_fs(fs, 0) == 0) {
        // Sector 0 is MBR, read partition 1 entry at offset 0x1BE (446)
        if (move_window(fs, 0) != FR_OK) return FR_DISK_ERR;
        uint8_t ptype = fs->win[446 + 4];
        if (ptype != 0) {
            bsect = ld_dword(&fs->win[446 + 8]);
            if (check_fs(fs, bsect) == 0) {
                // Try partition 2 if partition 1 not FAT
                bsect = ld_dword(&fs->win[446 + 16 + 8]);
                if (check_fs(fs, bsect) == 0) {
                    bsect = 2048; // Standard 1MB offset fallback
                    if (check_fs(fs, bsect) == 0) return FR_NO_FILESYSTEM;
                }
            }
        }
    }

    // Parse BPB
    if (move_window(fs, bsect) != FR_OK) return FR_DISK_ERR;

    uint16_t bytes_per_sec = ld_word(&fs->win[11]);
    if (bytes_per_sec == 0) bytes_per_sec = 512;
#if FF_MAX_SS != FF_MIN_SS
    fs->ssize = bytes_per_sec;
#endif
    fs->csize = fs->win[13];
    if (fs->csize == 0 || (fs->csize & (fs->csize - 1)) != 0) return FR_NO_FILESYSTEM;

    uint16_t rsvd_sec = ld_word(&fs->win[14]);
    fs->n_fats = fs->win[16];
    if (fs->n_fats < 1 || fs->n_fats > 2) return FR_NO_FILESYSTEM;

    fs->n_rootdir = ld_word(&fs->win[17]);
    uint32_t totsec = ld_word(&fs->win[19]);
    if (totsec == 0) totsec = ld_dword(&fs->win[32]);

    fs->fsize = ld_word(&fs->win[22]);
    if (fs->fsize == 0) fs->fsize = ld_dword(&fs->win[36]); // FAT32

    fs->volbase = bsect;
    fs->fatbase = bsect + rsvd_sec;

    uint32_t rootdir_sec = ((uint32_t)fs->n_rootdir * 32 + (bytes_per_sec - 1)) / bytes_per_sec;
    fs->database = fs->fatbase + (fs->n_fats * fs->fsize) + rootdir_sec;

    uint32_t data_sec = totsec - (rsvd_sec + (fs->n_fats * fs->fsize) + rootdir_sec);
    fs->n_fatent = (data_sec / fs->csize) + 2;

    if (fs->n_fatent > 65525) {
        fs->fs_type = 3; // FAT32
        fs->dirbase = ld_dword(&fs->win[44]); // Root cluster
    } else if (fs->n_fatent > 4085) {
        fs->fs_type = 2; // FAT16
        fs->dirbase = fs->fatbase + (fs->n_fats * fs->fsize);
    } else {
        fs->fs_type = 1; // FAT12
        fs->dirbase = fs->fatbase + (fs->n_fats * fs->fsize);
    }

    log_info("FATFS", "Volume %u: mounted FAT%u at LBA %u (Cluster: %u KB, FAT: LBA %u, Data: LBA %u)",
             pdrv, fs->fs_type == 3 ? 32 : (fs->fs_type == 2 ? 16 : 12),
             (uint32_t)fs->volbase, (uint32_t)(fs->csize * bytes_per_sec / 1024),
             (uint32_t)fs->fatbase, (uint32_t)fs->database);

    return FR_OK;
}

/* Parse path and resolve drive */
static FRESULT find_volume(const TCHAR **path, FATFS **rfs) {
    uint8_t pdrv = 0;
    const TCHAR *p = *path;
    if (p && p[0] >= '0' && p[0] <= '9' && p[1] == ':') {
        pdrv = (uint8_t)(p[0] - '0');
        p += 2;
    }
    if (pdrv >= FF_VOLUMES) return FR_INVALID_DRIVE;
    FATFS *fs = FatFs[pdrv];
    if (!fs || fs->fs_type == 0) return FR_NOT_ENABLED;

    while (*p == '/' || *p == '\\') p++;
    *path = p;
    *rfs = fs;
    return FR_OK;
}

/* Convert ASCII string to 8.3 formatted name */
static void make_sfn(const char *src, uint8_t *dst) {
    for (int i = 0; i < 11; i++) dst[i] = ' ';
    int i = 0;
    while (*src && *src != '.' && i < 8) {
        dst[i++] = (uint8_t)ff_wtoupper(*src++);
    }
    while (*src && *src != '.') src++;
    if (*src == '.') src++;
    i = 8;
    while (*src && i < 11) {
        dst[i++] = (uint8_t)ff_wtoupper(*src++);
    }
}

/* Check match between directory entry and filename (SFN or LFN) */
static bool match_entry(const uint8_t *sfn_in_dir, const char *search_name, const char *lfn_buf) {
    if (lfn_buf && lfn_buf[0]) {
        // Compare case-insensitively with LFN
        const char *a = lfn_buf;
        const char *b = search_name;
        while (*a && *b) {
            if (ff_wtoupper(*a) != ff_wtoupper(*b)) break;
            a++; b++;
        }
        if (*a == '\0' && *b == '\0') return true;
    }

    // Fallback to SFN compare
    uint8_t target_83[11];
    make_sfn(search_name, target_83);
    for (int i = 0; i < 11; i++) {
        if (sfn_in_dir[i] != target_83[i]) return false;
    }
    return true;
}

/* Open directory */
FRESULT f_opendir(DIR *dp, const TCHAR *path) {
    if (!dp) return FR_INVALID_OBJECT;
    FATFS *fs = NULL;
    FRESULT res = find_volume(&path, &fs);
    if (res != FR_OK) return res;

    dp->obj.fs = fs;
    dp->obj.id = fs->id;
    dp->dptr = 0;

    if (*path == '\0') {
        // Root directory
        dp->clust = (fs->fs_type == 3) ? fs->dirbase : 0;
        dp->sect = (fs->fs_type == 3) ? clst2sect(fs, dp->clust) : fs->dirbase;
        return FR_OK;
    }

    // Subdirectory navigation
    dp->clust = (fs->fs_type == 3) ? fs->dirbase : 0;
    dp->sect = (fs->fs_type == 3) ? clst2sect(fs, dp->clust) : fs->dirbase;

    // Simple single-level or multi-level subfolder traversal
    char part[64];
    while (*path) {
        int l = 0;
        while (*path && *path != '/' && *path != '\\' && l < 63) {
            part[l++] = *path++;
        }
        part[l] = '\0';
        while (*path == '/' || *path == '\\') path++;

        FILINFO fno;
        bool found = false;
        while (f_readdir(dp, &fno) == FR_OK && fno.fname[0]) {
            if ((fno.fattrib & AM_DIR) && match_entry((const uint8_t *)dp->fn, part, fno.fname)) {
                found = true;
                dp->clust = dp->obj.sclust;
                dp->sect = clst2sect(fs, dp->clust);
                dp->dptr = 0;
                break;
            }
        }
        if (!found) return FR_NO_PATH;
    }

    return FR_OK;
}

FRESULT f_closedir(DIR *dp) {
    if (dp) dp->obj.fs = NULL;
    return FR_OK;
}

/* Read next directory item */
FRESULT f_readdir(DIR *dp, FILINFO *fno) {
    if (!dp || !dp->obj.fs) return FR_INVALID_OBJECT;
    FATFS *fs = dp->obj.fs;
    if (fs->id != dp->obj.id) return FR_INVALID_OBJECT;

    if (!fno) {
        // Rewind directory
        dp->dptr = 0;
        return FR_OK;
    }

    fno->fname[0] = '\0';
    char lfn_acc[FF_MAX_LFN + 1] = {0};

    while (1) {
        LBA_t sect = 0;
        if (fs->fs_type == 3 || dp->clust != 0) {
            uint32_t cidx = dp->dptr / (fs->csize * FF_MAX_SS);
            uint32_t clst = dp->clust;
            for (uint32_t i = 0; i < cidx; i++) {
                clst = get_fat(fs, clst);
                if (clst < 2 || clst >= 0x0FFFFFF8) return FR_OK; // End of dir
            }
            uint32_t c_offset = (dp->dptr % (fs->csize * FF_MAX_SS)) / FF_MAX_SS;
            sect = clst2sect(fs, clst) + c_offset;
        } else {
            // FAT12/16 root directory
            uint32_t s_idx = dp->dptr / FF_MAX_SS;
            uint32_t max_root_sec = ((uint32_t)fs->n_rootdir * 32) / FF_MAX_SS;
            if (s_idx >= max_root_sec) return FR_OK;
            sect = fs->dirbase + s_idx;
        }

        if (move_window(fs, sect) != FR_OK) return FR_DISK_ERR;

        uint32_t entry_offset = dp->dptr % FF_MAX_SS;
        uint8_t *dir_entry = &fs->win[entry_offset];
        dp->dptr += 32;

        uint8_t marker = dir_entry[0];
        if (marker == 0x00) return FR_OK; // End of directory
        if (marker == 0xE5) {
            lfn_acc[0] = '\0';
            continue; // Deleted entry
        }

        uint8_t attr = dir_entry[11];
        if (attr == 0x0F) {
            // LFN Entry: extract Unicode characters
            uint8_t seq = dir_entry[0] & 0x1F;
            if (seq >= 1 && seq <= 20) {
                uint32_t lfn_idx = (seq - 1) * 13;
                const uint8_t name_offsets[] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
                for (int c = 0; c < 13 && (lfn_idx + c) < FF_MAX_LFN; c++) {
                    uint16_t uc = ld_word(&dir_entry[name_offsets[c]]);
                    if (uc == 0x0000 || uc == 0xFFFF) break;
                    lfn_acc[lfn_idx + c] = (uc < 128) ? (char)uc : '?';
                }
            }
            continue;
        }

        // Standard SFN entry found
        for (int i = 0; i < 11; i++) dp->fn[i] = dir_entry[i];

        fno->fattrib = attr;
        fno->fsize = ld_dword(&dir_entry[28]);
        fno->fdate = ld_word(&dir_entry[26]);
        fno->ftime = ld_word(&dir_entry[24]);
        dp->obj.sclust = ((uint32_t)ld_word(&dir_entry[20]) << 16) | ld_word(&dir_entry[26]);

        if (lfn_acc[0] != '\0') {
            for (int i = 0; i <= FF_MAX_LFN; i++) {
                fno->fname[i] = lfn_acc[i];
                if (lfn_acc[i] == '\0') break;
            }
        } else {
            // Format SFN into 8.3 readable string
            int out = 0;
            for (int i = 0; i < 8 && dir_entry[i] != ' '; i++) {
                fno->fname[out++] = (char)dir_entry[i];
            }
            if (dir_entry[8] != ' ') {
                fno->fname[out++] = '.';
                for (int i = 8; i < 11 && dir_entry[i] != ' '; i++) {
                    fno->fname[out++] = (char)dir_entry[i];
                }
            }
            fno->fname[out] = '\0';
        }

        return FR_OK;
    }
}

/* Open file */
FRESULT f_open(FIL *fp, const TCHAR *path, uint8_t mode) {
    if (!fp || !path) return FR_INVALID_PARAMETER;

    FATFS *fs = NULL;
    FRESULT res = find_volume(&path, &fs);
    if (res != FR_OK) return res;

    // Extract directory path and filename
    char dir_path[256] = {0};
    const char *fname = path;
    const char *last_slash = NULL;
    for (const char *s = path; *s; s++) {
        if (*s == '/' || *s == '\\') last_slash = s;
    }

    if (last_slash) {
        size_t dlen = last_slash - path;
        if (dlen >= sizeof(dir_path)) dlen = sizeof(dir_path) - 1;
        for (size_t i = 0; i < dlen; i++) dir_path[i] = path[i];
        dir_path[dlen] = '\0';
        fname = last_slash + 1;
    }

    DIR dp;
    res = f_opendir(&dp, dir_path);
    if (res != FR_OK) return res;

    FILINFO fno;
    bool found = false;
    while (f_readdir(&dp, &fno) == FR_OK && fno.fname[0]) {
        if (!(fno.fattrib & AM_DIR) && match_entry(dp.fn, fname, fno.fname)) {
            found = true;
            fp->obj.fs = fs;
            fp->obj.id = fs->id;
            fp->obj.attr = fno.fattrib;
            fp->obj.objsize = fno.fsize;
            fp->obj.sclust = dp.obj.sclust;
            fp->fptr = 0;
            fp->clust = fp->obj.sclust;
            fp->sect = 0;
            fp->flag = mode;
            fp->err = 0;
            break;
        }
    }
    f_closedir(&dp);

    if (!found) {
        if (mode & FA_CREATE_ALWAYS) {
            // File creation stub for new profiles
            log_info("FATFS", "File '%s' not found, creation not permitted in this directory.", fname);
            return FR_NO_FILE;
        }
        return FR_NO_FILE;
    }

    return FR_OK;
}

/* Read from file */
FRESULT f_read(FIL *fp, void *buff, uint32_t btr, uint32_t *br) {
    if (br) *br = 0;
    if (!fp || !fp->obj.fs || !buff) return FR_INVALID_OBJECT;
    FATFS *fs = fp->obj.fs;
    if (fs->id != fp->obj.id) return FR_INVALID_OBJECT;

    if (fp->fptr >= fp->obj.objsize) return FR_OK; // EOF
    if (fp->fptr + btr > fp->obj.objsize) {
        btr = (uint32_t)(fp->obj.objsize - fp->fptr);
    }
    if (btr == 0) return FR_OK;

    uint8_t *dst = (uint8_t *)buff;
    uint32_t total_read = 0;
    uint32_t bytes_per_sec = 512;
#if FF_MAX_SS != FF_MIN_SS
    bytes_per_sec = fs->ssize ? fs->ssize : 512;
#endif
    uint32_t bytes_per_cluster = fs->csize * bytes_per_sec;

    while (total_read < btr) {
        if (fp->clust < 2 || fp->clust >= 0x0FFFFFF8) break;

        uint32_t clst_offset = fp->fptr % bytes_per_cluster;
        uint32_t to_read = bytes_per_cluster - clst_offset;
        if (to_read > (btr - total_read)) to_read = btr - total_read;

        uint32_t sec_offset = clst_offset / bytes_per_sec;
        uint32_t byte_in_sec = clst_offset % bytes_per_sec;
        LBA_t lba = clst2sect(fs, fp->clust) + sec_offset;

        if (byte_in_sec == 0 && to_read >= bytes_per_sec) {
            // Direct aligned sector read into target buffer
            uint32_t whole_sec = to_read / bytes_per_sec;
            if (disk_read(fs->pdrv, dst + total_read, lba, whole_sec) != RES_OK) {
                return FR_DISK_ERR;
            }
            uint32_t direct_bytes = whole_sec * bytes_per_sec;
            total_read += direct_bytes;
            fp->fptr += direct_bytes;
            if ((fp->fptr % bytes_per_cluster) == 0) {
                fp->clust = get_fat(fs, fp->clust);
            }
            continue;
        }

        // Window/bounce buffer read
        if (move_window(fs, lba) != FR_OK) return FR_DISK_ERR;

        uint32_t chunk = bytes_per_sec - byte_in_sec;
        if (chunk > to_read) chunk = to_read;
        for (uint32_t i = 0; i < chunk; i++) {
            dst[total_read + i] = fs->win[byte_in_sec + i];
        }

        total_read += chunk;
        fp->fptr += chunk;
        if ((fp->fptr % bytes_per_cluster) == 0) {
            fp->clust = get_fat(fs, fp->clust);
        }
    }

    if (br) *br = total_read;
    return FR_OK;
}

/* Write to file */
FRESULT f_write(FIL *fp, const void *buff, uint32_t btw, uint32_t *bw) {
    if (bw) *bw = 0;
    if (!fp || !fp->obj.fs || !buff) return FR_INVALID_OBJECT;
    FATFS *fs = fp->obj.fs;
    if (fs->id != fp->obj.id) return FR_INVALID_OBJECT;

    const uint8_t *src = (const uint8_t *)buff;
    uint32_t total_written = 0;
    uint32_t bytes_per_sec = 512;
#if FF_MAX_SS != FF_MIN_SS
    bytes_per_sec = fs->ssize ? fs->ssize : 512;
#endif
    uint32_t bytes_per_cluster = fs->csize * bytes_per_sec;

    while (total_written < btw) {
        if (fp->clust < 2 || fp->clust >= 0x0FFFFFF8) break;

        uint32_t clst_offset = fp->fptr % bytes_per_cluster;
        uint32_t to_write = bytes_per_cluster - clst_offset;
        if (to_write > (btw - total_written)) to_write = btw - total_written;

        uint32_t sec_offset = clst_offset / bytes_per_sec;
        uint32_t byte_in_sec = clst_offset % bytes_per_sec;
        LBA_t lba = clst2sect(fs, fp->clust) + sec_offset;

        if (byte_in_sec == 0 && to_write >= bytes_per_sec) {
            uint32_t whole_sec = to_write / bytes_per_sec;
            if (disk_write(fs->pdrv, src + total_written, lba, whole_sec) != RES_OK) {
                return FR_DISK_ERR;
            }
            uint32_t direct_bytes = whole_sec * bytes_per_sec;
            total_written += direct_bytes;
            fp->fptr += direct_bytes;
            if (fp->fptr > fp->obj.objsize) fp->obj.objsize = fp->fptr;
            if ((fp->fptr % bytes_per_cluster) == 0) {
                fp->clust = get_fat(fs, fp->clust);
            }
            continue;
        }

        if (move_window(fs, lba) != FR_OK) return FR_DISK_ERR;
        uint32_t chunk = bytes_per_sec - byte_in_sec;
        if (chunk > to_write) chunk = to_write;
        for (uint32_t i = 0; i < chunk; i++) {
            fs->win[byte_in_sec + i] = src[total_written + i];
        }
        fs->wflag = 1;

        total_written += chunk;
        fp->fptr += chunk;
        if (fp->fptr > fp->obj.objsize) fp->obj.objsize = fp->fptr;
        if ((fp->fptr % bytes_per_cluster) == 0) {
            fp->clust = get_fat(fs, fp->clust);
        }
    }

    if (bw) *bw = total_written;
    return sync_window(fs);
}

/* Seek in file */
FRESULT f_lseek(FIL *fp, FSIZE_t ofs) {
    if (!fp || !fp->obj.fs) return FR_INVALID_OBJECT;
    FATFS *fs = fp->obj.fs;
    if (ofs > fp->obj.objsize) ofs = fp->obj.objsize;

    fp->fptr = 0;
    fp->clust = fp->obj.sclust;

    uint32_t bytes_per_sec = 512;
#if FF_MAX_SS != FF_MIN_SS
    bytes_per_sec = fs->ssize ? fs->ssize : 512;
#endif
    uint32_t bytes_per_cluster = fs->csize * bytes_per_sec;
    uint32_t clusters_to_skip = ofs / bytes_per_cluster;

    for (uint32_t i = 0; i < clusters_to_skip; i++) {
        fp->clust = get_fat(fs, fp->clust);
        if (fp->clust < 2 || fp->clust >= 0x0FFFFFF8) break;
    }
    fp->fptr = ofs;
    return FR_OK;
}

/* Close file */
FRESULT f_close(FIL *fp) {
    if (!fp || !fp->obj.fs) return FR_INVALID_OBJECT;
    FRESULT res = sync_window(fp->obj.fs);
    fp->obj.fs = NULL;
    return res;
}

FRESULT f_sync(FIL *fp) {
    if (!fp || !fp->obj.fs) return FR_INVALID_OBJECT;
    return sync_window(fp->obj.fs);
}
