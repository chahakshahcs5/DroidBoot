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

#ifndef FF_DEFINED
#define FF_DEFINED 80286

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include "ffconf.h"

/* Type of path name strings on FatFs API */
#ifndef TCHAR
typedef char TCHAR;
#define _T(x) x
#define _TEXT(x) x
#endif

/* Type of file size variables */
typedef uint32_t FSIZE_t;

/* Type of logical block address variables */
typedef uint32_t LBA_t;

/* Filesystem object structure (FATFS) */
typedef struct {
    uint8_t   fs_type;      /* Filesystem type (0:not mounted) */
    uint8_t   pdrv;         /* Associated physical drive */
    uint8_t   n_fats;       /* Number of FATs (1 or 2) */
    uint8_t   wflag;        /* win[] flag (b0:dirty) */
    uint8_t   fsi_flag;     /* FSINFO flags (b7:disabled, b0:dirty) */
    uint16_t  id;           /* Volume mount ID */
    uint16_t  n_rootdir;    /* Number of root directory entries (FAT12/16) */
    uint16_t  csize;        /* Cluster size [sectors] */
#if FF_MAX_SS != FF_MIN_SS
    uint16_t  ssize;        /* Sector size (512, 1024, 2048 or 4096) */
#endif
#if FF_USE_LFN
    uint8_t*  lfnbuf;       /* LFN working buffer */
#endif
    uint32_t  last_clst;    /* Last allocated cluster */
    uint32_t  free_clst;    /* Number of free clusters */
    uint32_t  cdir;         /* Current directory cluster (0:root) */
    uint32_t  n_fatent;     /* Number of FAT entries (number of clusters + 2) */
    uint32_t  fsize;        /* Size of an FAT [sectors] */
    LBA_t     volbase;      /* Volume base sector */
    LBA_t     fatbase;      /* FAT base sector */
    LBA_t     dirbase;      /* Root directory base sector/cluster */
    LBA_t     database;     /* Data base sector */
    LBA_t     winsect;      /* Current sector appearing in the win[] */
    uint8_t   win[FF_MAX_SS]; /* Disk access window for Directory, FAT, etc. */
} FATFS;

/* Object ID and allocation information (FFOBJID) */
typedef struct {
    FATFS*    fs;           /* Pointer to the hosting volume of this object */
    uint16_t  id;           /* Hosting volume mount ID */
    uint8_t   attr;         /* Object attribute */
    uint8_t   stat;         /* Object chain status */
    uint32_t  sclust;       /* Object start cluster (0:no cluster or root dir) */
    FSIZE_t   objsize;      /* Object size (valid when sclust != 0) */
} FFOBJID;

/* File object structure (FIL) */
typedef struct {
    FFOBJID   obj;          /* Object identifier */
    uint8_t   flag;         /* File status flags */
    uint8_t   err;          /* Abort flag (error code) */
    FSIZE_t   fptr;         /* File read/write pointer */
    uint32_t  clust;        /* Current cluster of fptr */
    LBA_t     sect;         /* Sector number appearing in buf[] */
    LBA_t     dir_sect;     /* Sector number containing the directory entry */
    uint8_t*  dir_ptr;      /* Pointer to the directory entry in the win[] */
    uint8_t   buf[FF_MAX_SS]; /* File private data read/write window */
} FIL;

/* Directory object structure (DIR) */
typedef struct {
    FFOBJID   obj;          /* Object identifier */
    uint32_t  dptr;         /* Current read/write offset */
    uint32_t  clust;        /* Current cluster */
    LBA_t     sect;         /* Current sector */
    uint8_t*  dir;          /* Pointer to the current SFN entry in win[] */
    uint8_t   fn[12];       /* SFN (in/out) {body[8],ext[3],status[1]} */
#if FF_USE_LFN
    uint32_t  blk_ofs;      /* Offset of current LFN block */
#endif
} DIR;

/* File information structure (FILINFO) */
typedef struct {
    FSIZE_t   fsize;        /* File size */
    uint16_t  fdate;        /* Modified date */
    uint16_t  ftime;        /* Modified time */
    uint8_t   fattrib;      /* File attribute */
#if FF_USE_LFN
    TCHAR     altname[FF_SFN_BUF + 1];/* Alternative file name */
    TCHAR     fname[FF_MAX_LFN + 1];  /* Primary file name */
#else
    TCHAR     fname[12 + 1];/* File name */
#endif
} FILINFO;

/* File function return code (FRESULT) */
typedef enum {
    FR_OK = 0,              /* (0) Succeeded */
    FR_DISK_ERR,            /* (1) A hard error occurred in the low level disk I/O layer */
    FR_INT_ERR,             /* (2) Assertion failed */
    FR_NOT_READY,           /* (3) The physical drive cannot work */
    FR_NO_FILE,             /* (4) Could not find the file */
    FR_NO_PATH,             /* (5) Could not find the path */
    FR_INVALID_NAME,        /* (6) The path name format is invalid */
    FR_DENIED,              /* (7) Access denied due to prohibited access or directory full */
    FR_EXIST,               /* (8) Access denied due to prohibited access */
    FR_INVALID_OBJECT,      /* (9) The file/directory object is invalid */
    FR_WRITE_PROTECTED,     /* (10) The physical drive is write protected */
    FR_INVALID_DRIVE,       /* (11) The logical drive number is invalid */
    FR_NOT_ENABLED,         /* (12) The volume has no work area */
    FR_NO_FILESYSTEM,       /* (13) There is no valid FAT volume */
    FR_MKFS_ABORTED,        /* (14) The f_mkfs() aborted due to any problem */
    FR_TIMEOUT,             /* (15) Could not get a grant to access the volume within the time */
    FR_LOCKED,              /* (16) The operation is rejected according to the file sharing policy */
    FR_NOT_ENOUGH_CORE,     /* (17) LFN working buffer could not be allocated */
    FR_TOO_MANY_OPEN_FILES, /* (18) Number of open files > FF_FS_LOCK */
    FR_INVALID_PARAMETER    /* (19) Given parameter is invalid */
} FRESULT;

/* FatFs module API functions */
FRESULT f_open (FIL* fp, const TCHAR* path, uint8_t mode);
FRESULT f_close (FIL* fp);
FRESULT f_read (FIL* fp, void* buff, uint32_t btr, uint32_t* br);
FRESULT f_write (FIL* fp, const void* buff, uint32_t btw, uint32_t* bw);
FRESULT f_lseek (FIL* fp, FSIZE_t ofs);
FRESULT f_truncate (FIL* fp);
FRESULT f_sync (FIL* fp);
FRESULT f_opendir (DIR* dp, const TCHAR* path);
FRESULT f_closedir (DIR* dp);
FRESULT f_readdir (DIR* dp, FILINFO* fno);
FRESULT f_findfirst (DIR* dp, FILINFO* fno, const TCHAR* path, const TCHAR* pattern);
FRESULT f_findnext (DIR* dp, FILINFO* fno);
FRESULT f_stat (const TCHAR* path, FILINFO* fno);
FRESULT f_unlink (const TCHAR* path);
FRESULT f_rename (const TCHAR* path_old, const TCHAR* path_new);
FRESULT f_mkdir (const TCHAR* path);
FRESULT f_mount (FATFS* fs, const TCHAR* path, uint8_t opt);

#define f_eof(fp) ((int)((fp)->fptr == (fp)->obj.objsize))
#define f_error(fp) ((fp)->err)
#define f_tell(fp) ((fp)->fptr)
#define f_size(fp) ((fp)->obj.objsize)
#define f_rewind(fp) f_lseek((fp), 0)
#define f_rewinddir(dp) f_readdir((dp), 0)

/* File access mode and open method flags (3rd argument of f_open) */
#define FA_READ             0x01
#define FA_OPEN_EXISTING    0x00
#define FA_WRITE            0x02
#define FA_CREATE_NEW       0x04
#define FA_CREATE_ALWAYS    0x08
#define FA_OPEN_ALWAYS      0x10
#define FA_OPEN_APPEND      0x30

/* Fast seek controls (2nd argument of f_lseek) */
#define CREATE_LINKMAP      ((FSIZE_t)0 - 1)

/* Format options (2nd argument of f_mkfs) */
#define FM_FAT              0x01
#define FM_FAT32            0x02
#define FM_EXFAT            0x04
#define FM_ANY              0x07
#define FM_SFD              0x08

/* File attribute bits for directory entry */
#define AM_RDO  0x01    /* Read only */
#define AM_HID  0x02    /* Hidden */
#define AM_SYS  0x04    /* System */
#define AM_DIR  0x10    /* Directory */
#define AM_ARC  0x20    /* Archive */

uint32_t get_fattime (void);

#if FF_USE_LFN == 3
void* ff_memalloc (uint32_t msize);
void  ff_memfree (void* mblock);
#endif

#ifdef __cplusplus
}
#endif

#endif /* FF_DEFINED */
