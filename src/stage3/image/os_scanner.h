#ifndef OS_SCANNER_H
#define OS_SCANNER_H

#include <stdint.h>
#include <stdbool.h>
#include "../usb/usb.h"
#include "../mtp/mtp.h"
#include "../adb/adb.h"
#include "../filesystem/iso_reader.h"
#include "../../include/boot.h"
#include "../xhci/xhci.h"

#define MAX_OS_ENTRIES 8
#define MAX_PERSISTENCE_PROFILES 8

typedef struct {
    char     profile_name[80]; // e.g. "[alpine-standard-3.24.2-x86_64] work (2048 MB)"
    char     filename[96];     // e.g. "alpine-standard-3.24.2-x86_64_work_2048MB.img"
    uint64_t file_size;        // Size in bytes
    uint32_t mtp_handle;       // PTP handle if MTP
    bool     is_clean_session; // True for non-persistent disposable session
} persistence_profile_t;

typedef enum {
    OS_STORAGE_BLOCK_USB,    // USB Mass Storage / Rooted Phone (Approach 1: Direct Block)
    OS_STORAGE_BLOCK_SD,     // SD Card FAT32 Partition (Approach 1: Direct Block)
    OS_STORAGE_MTP_ANDROID   // Android Phone over MTP (Approach 2: In-RAM + SD Persistence)
} os_storage_type_t;

typedef enum {
    BOOT_APPROACH_BLOCK_ON_DEMAND, // Extract only vmlinuz + initrd (0 MB of 6GB in RAM)
    BOOT_APPROACH_MTP_IN_RAM,      // Stream complete ISO into RAM + SD card persistence
    BOOT_APPROACH_CHAINLOAD        // Real-mode VBR chainloading (Windows, BSD)
} boot_approach_t;

typedef struct os_entry {
    char              title[64];           // Human-readable title (e.g. "Ubuntu 26.04 Live Desktop")
    char              filename[64];        // Filename (e.g. "ubuntu-26.04.1-desktop-amd64.iso")
    char              storage_desc[48];    // Storage description (e.g. "USB Block (Rooted Phone)")
    uint64_t          file_size;           // Image size in bytes
    os_storage_type_t storage_type;
    boot_approach_t   approach;
    bool              is_windows;

    // Approach 1 (Block on-demand):
    iso_boot_files_t  iso_files;
    usb_device_t     *usb_dev;
    uint32_t          partition_lba;

    // Approach 2 (MTP in-RAM):
    uint32_t          mtp_handle;
    mtp_session_t    *mtp_session;

    // Multi-profile persistence (/BootManager/persistence/):
    persistence_profile_t profiles[MAX_PERSISTENCE_PROFILES];
    uint32_t               profile_count;
    uint32_t               selected_profile;
} os_entry_t;

typedef struct os_registry {
    os_entry_t entries[MAX_OS_ENTRIES];
    uint32_t   count;
} os_registry_t;

void os_registry_init(os_registry_t *reg);

int  os_add_custom_profile(os_entry_t *entry, const char *name, const char *filename, uint64_t size_bytes);

int  os_scan_all_storages(boot_info_t *boot_info,
                          xhci_controller_t *xhci,
                          usb_device_t *msc_dev,
                          mtp_session_t *mtp_session,
                          adb_session_t *adb_session,
                          os_registry_t *out_registry);

#endif // OS_SCANNER_H
