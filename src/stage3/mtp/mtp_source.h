#ifndef MTP_SOURCE_H
#define MTP_SOURCE_H

#include <stdint.h>
#include <stdbool.h>
#include "mtp.h"
#include "../../include/boot_source.h"

boot_source_t *boot_source_mtp_create(mtp_session_t *session);
int            mtp_source_set_target(boot_source_t *src, uint32_t handle, uint64_t file_size);
int            mtp_find_boot_file(boot_source_t *src, char *out_name, uint32_t max_len);

#endif // MTP_SOURCE_H
