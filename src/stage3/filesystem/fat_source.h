#ifndef FAT_SOURCE_H
#define FAT_SOURCE_H

#include "../../include/boot_source.h"
#include <stdint.h>
#include <stddef.h>

typedef int (*block_read_fn)(void *priv, uint32_t lba, uint32_t count, void *buf);

boot_source_t *boot_source_fat_create(block_read_fn read_fn, void *priv, uint32_t partition_start_lba);
void           boot_source_fat_destroy(boot_source_t *src);

// Dynamic directory enumeration callback
typedef void (*fat_file_callback_fn)(const char *filename, uint64_t size, bool is_dir, void *user_data);
int            boot_source_fat_scan_dir(boot_source_t *src, const char *dir_path, fat_file_callback_fn cb, void *user_data);

#endif // FAT_SOURCE_H
