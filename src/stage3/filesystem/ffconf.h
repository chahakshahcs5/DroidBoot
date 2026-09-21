#ifndef FFCONF_H
#define FFCONF_H

#define FFCONF_DEF 80286

// Read & write support
#define FF_FS_READONLY      0
#define FF_FS_MINIMIZE      0
#define FF_USE_FIND         1
#define FF_USE_MKFS         0
#define FF_USE_FASTSEEK     0

// Long File Name support (3: heap allocation via ff_memalloc/ff_memfree)
#define FF_USE_LFN          3
#define FF_MAX_LFN          255
#define FF_LFN_UNICODE      0
#define FF_LFN_BUF          255
#define FF_SFN_BUF          12

// Code page (437 - U.S.)
#define FF_CODE_PAGE        437

// Sector size range (512 to 4096 bytes)
#define FF_MAX_SS           4096
#define FF_MIN_SS           512

#define FF_FS_TINY          0
#define FF_FS_EXFAT         0
#define FF_VOLUMES          2
#define FF_STR_VOLUME_ID    0
#define FF_USE_STRFUNC      0
#define FF_PRINT_LLI        0
#define FF_PRINT_FLOAT      0
#define FF_STRF_ENCODE      3
#define FF_FS_NOFSINFO      0
#define FF_FS_LOCK          0
#define FF_FS_REENTRANT     0

#endif // FFCONF_H
