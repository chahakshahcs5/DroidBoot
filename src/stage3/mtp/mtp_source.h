#ifndef MTP_SOURCE_H
#define MTP_SOURCE_H

#include <stdint.h>
#include <stdbool.h>
#include "mtp.h"
#include "../../include/boot_source.h"

boot_source_t *boot_source_mtp_create(mtp_session_t *session);

#endif // MTP_SOURCE_H
