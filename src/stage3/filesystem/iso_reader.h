#ifndef ISO_READER_H
#define ISO_READER_H

#include <stdint.h>
#include <stdbool.h>
#include "../../include/boot_source.h"

typedef struct iso_boot_files {
    uint32_t kernel_lba;
    uint32_t kernel_size;
    uint32_t initrd_lba;
    uint32_t initrd_size;
    bool     found_kernel;
    bool     found_initrd;
    bool     is_casper;
    bool     is_windows;
    bool     is_kali;
    uint32_t alt_initrd_lba;
    uint32_t alt_initrd_size;
    bool     has_alt_initrd;
    uint32_t bootmgr_lba;
    uint32_t bootmgr_size;
    uint32_t boot_sector_lba;
    uint32_t boot_sector_size;
    char     volume_id[33];
    char     title[96];
    char     cmdline[384];
} iso_boot_files_t;

// Find arbitrary file by path (case-insensitive, Rock Ridge aware)
int iso_find_file(boot_source_t *iso_src, const char *path, uint32_t *out_lba, uint32_t *out_size);

// Universal boot discovery: tries grub.cfg / isolinux.cfg first, then heuristic fallback
int iso_find_boot_files(boot_source_t *iso_src, iso_boot_files_t *out_files);

#endif // ISO_READER_H
