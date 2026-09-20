#ifndef FAT_SOURCE_H
#define FAT_SOURCE_H

#include <stdint.h>
#include <stdbool.h>
#include "../../include/boot_source.h"

// Block read callback function: int read_sectors(void *priv, uint32_t lba, uint32_t count, void *buf)
typedef int (*block_read_fn)(void *priv, uint32_t lba, uint32_t count, void *buf);

boot_source_t *boot_source_fat_create(block_read_fn read_fn, void *priv, uint32_t partition_start_lba);

#endif // FAT_SOURCE_H
