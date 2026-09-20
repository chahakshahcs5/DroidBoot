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
} iso_boot_files_t;

int iso_find_boot_files(boot_source_t *iso_src, iso_boot_files_t *out_files);

#endif // ISO_READER_H
