#ifndef BOOT_CFG_PARSER_H
#define BOOT_CFG_PARSER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../include/boot_source.h"

#define MAX_PARSED_ENTRIES 8

typedef struct {
    char title[96];
    char kernel_path[128];
    char initrd_path[128];
    char cmdline[384];
    bool valid;
} parsed_boot_entry_t;

typedef struct {
    parsed_boot_entry_t entries[MAX_PARSED_ENTRIES];
    uint32_t            count;
    uint32_t            default_index;
    char                config_source[64];
} parsed_boot_config_t;

int boot_cfg_parse(const char *cfg_data, uint32_t len, const char *iso_filename, parsed_boot_config_t *out_cfg);
int boot_cfg_find_and_parse(boot_source_t *iso_src, const char *iso_filename, parsed_boot_config_t *out_cfg);

#endif // BOOT_CFG_PARSER_H
