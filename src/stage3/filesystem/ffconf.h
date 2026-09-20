#ifndef FFCONF_H
#define FFCONF_H

#define FFCONF_DEF 80286

// Read-only mode to minimize code footprint
#define FF_FS_READONLY      1

// Minimization level (0: full features)
#define FF_FS_MINIMIZE      0

// Find functions
#define FF_USE_FIND         0

// Use mkfs
#define FF_USE_MKFS         0

// Fast seek
#define FF_USE_FASTSEEK     0

// Long File Name support (1: BSS buffer)
#define FF_USE_LFN          1
#define FF_MAX_LFN          255
#define FF_LFN_UNICODE      0
#define FF_LFN_BUF          255
#define FF_SFN_BUF          12

// Code page (437 - U.S.)
#define FF_CODE_PAGE        437

// Sector size
#define FF_MAX_SS           512
#define FF_MIN_SS           512

// Tiny buffer mode (buffer in FATFS object)
#define FF_FS_TINY          1

// ExFAT support (disabled for minimal footprint)
#define FF_FS_EXFAT         0

// Volume count
#define FF_VOLUMES          2

// String functions
#define FF_USE_STRFUNC      0

#endif // FFCONF_H
