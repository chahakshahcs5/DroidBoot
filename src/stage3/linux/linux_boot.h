#ifndef LINUX_BOOT_H
#define LINUX_BOOT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../include/boot.h"
#include "../bios/vbe.h"

// Linux 32-bit Boot Protocol definitions (Protocol 2.00+)
#define LINUX_HDRS_MAGIC        0x53726448  // "HdrS"
#define LINUX_BOOT_FLAG_MAGIC   0xAA55
#define LINUX_BOOT_LOADER_TYPE  0xFF        // Undefined / Custom Bootloader

#define LINUX_LOADFLAGS_LOADED_HIGH     (1 << 0)
#define LINUX_LOADFLAGS_QUIET           (1 << 5)
#define LINUX_LOADFLAGS_KEEP_SEGMENTS   (1 << 6)
#define LINUX_LOADFLAGS_CAN_USE_HEAP    (1 << 7)

#pragma pack(push, 1)

typedef struct linux_setup_header {
    uint8_t  setup_sects;
    uint16_t root_flags;
    uint32_t syssize;
    uint16_t ram_size;
    uint16_t vid_mode;
    uint16_t root_dev;
    uint16_t boot_flag;
    uint16_t jump;
    uint32_t header;            // "HdrS" (0x53726448)
    uint16_t version;
    uint32_t realmode_swtch;
    uint16_t start_sys_seg;
    uint16_t kernel_version;
    uint8_t  type_of_loader;
    uint8_t  loadflags;
    uint16_t setup_move_size;
    uint32_t code32_start;
    uint32_t ramdisk_image;
    uint32_t ramdisk_size;
    uint32_t bootsect_kludge;
    uint16_t heap_end_ptr;
    uint8_t  ext_loader_ver;
    uint8_t  ext_loader_type;
    uint32_t cmd_line_ptr;
    uint32_t initrd_addr_max;
    uint32_t kernel_alignment;
    uint8_t  relocatable_kernel;
    uint8_t  min_alignment;
    uint16_t xloadflags;
    uint32_t cmdline_size;
    uint32_t hardware_subarch;
    uint64_t hardware_subarch_data;
    uint32_t payload_offset;
    uint32_t payload_length;
    uint64_t setup_data;
    uint64_t pref_address;
    uint32_t init_size;
    uint32_t handover_offset;
} linux_setup_header_t;

typedef struct linux_e820_entry {
    uint64_t addr;
    uint64_t size;
    uint32_t type;
} linux_e820_entry_t;

typedef struct linux_screen_info {
    uint8_t  orig_x;                // 0x00
    uint8_t  orig_y;                // 0x01
    uint16_t ext_mem_k;             // 0x02
    uint16_t orig_video_page;       // 0x04
    uint8_t  orig_video_mode;       // 0x06
    uint8_t  orig_video_cols;       // 0x07
    uint8_t  flags;                 // 0x08
    uint8_t  unused2;               // 0x09
    uint16_t orig_video_ega_bx;     // 0x0A
    uint16_t unused3;               // 0x0C
    uint8_t  orig_video_lines;      // 0x0E
    uint8_t  orig_video_isVGA;      // 0x0F: 0x22 = VIDEO_TYPE_VGAC, 0x23 = VIDEO_TYPE_VLFB
    uint16_t orig_video_points;     // 0x10: font height (16)
    uint16_t lfb_width;             // 0x12
    uint16_t lfb_height;            // 0x14
    uint16_t lfb_depth;             // 0x16
    uint32_t lfb_base;              // 0x18: 32-bit physical address of framebuffer
    uint32_t lfb_size;              // 0x1C: framebuffer size in 64KB units
    uint16_t cl_magic, cl_offset;   // 0x20
    uint16_t lfb_linelength;        // 0x24: pitch in bytes
    uint8_t  red_size;              // 0x26
    uint8_t  red_pos;               // 0x27
    uint8_t  green_size;            // 0x28
    uint8_t  green_pos;             // 0x29
    uint8_t  blue_size;             // 0x2A
    uint8_t  blue_pos;              // 0x2B
    uint8_t  rsvd_size;             // 0x2C
    uint8_t  rsvd_pos;              // 0x2D
    uint16_t vesapm_seg;            // 0x2E
    uint16_t vesapm_off;            // 0x30
    uint16_t pages;                 // 0x32
    uint16_t vesa_attributes;       // 0x34
    uint32_t capabilities;          // 0x36
    uint32_t ext_lfb_base;          // 0x3A
    uint8_t  _reserved[2];          // 0x3E
} linux_screen_info_t;

typedef struct linux_boot_params {
    linux_screen_info_t  screen_info;       // Offset 0x000 (64 bytes)
    uint8_t              apm_bios_info[20];
    uint8_t              _pad2[4];
    uint64_t             tboot_addr;
    uint8_t              ist_info[16];
    uint8_t              _pad3[16];
    uint8_t              hd0_info[16];
    uint8_t              hd1_info[16];
    uint8_t              sys_desc_table[16];
    uint8_t              olpc_ofw_header[16];
    uint32_t             ext_ramdisk_image;
    uint32_t             ext_ramdisk_size;
    uint32_t             ext_cmd_line_ptr;
    uint8_t              _pad4[116];
    uint8_t              edid_info[128];
    uint8_t              efi_info[32];
    uint32_t             alt_mem_k;
    uint32_t             scratch;
    uint8_t              e820_entries;
    uint8_t              eddbuf_entries;
    uint8_t              edd_mbr_sig_buf_entries;
    uint8_t              kbd_status;
    uint8_t              _pad5[3];
    uint8_t              sentinel;
    uint8_t              _pad6[1];
    linux_setup_header_t hdr;               // Offset 0x1F1
    uint8_t              _pad7[0x290 - 0x1F1 - sizeof(linux_setup_header_t)];
    uint32_t             edd_mbr_sig_buffer[16];
    linux_e820_entry_t   e820_table[128];   // Offset 0x2D0
    uint8_t              _pad8[4096 - 0x2D0 - (128 * sizeof(linux_e820_entry_t))];
} linux_boot_params_t;

// MEMDISK Boot Firmware Table (mBFT) for Alpine memdiskfind
typedef struct linux_mbft {
    struct {
        char     signature[4];    // "mBFT"
        uint32_t length;          // sizeof(struct linux_mbft) = 66
        uint8_t  revision;        // 1
        uint8_t  checksum;        // byte sum of entire struct == 0
        char     oem_id[6];       // "SYSLNX"
        char     oem_table_id[8]; // "MEMDISK "
        uint32_t oem_revision;    // 1
        uint32_t creator_id;      // 0
        uint32_t creator_rev;     // 0
    } acpi;
    uint32_t safe_hook;           // 0
    struct {
        uint16_t bytes;           // 26
        uint8_t  version_minor;   // 0
        uint8_t  version_major;   // 1
        uint32_t diskbuf;         // Physical start address of ISO
        uint32_t disksize;        // Total sectors
        uint32_t cmdline;         // 0
        uint32_t oldint13;        // 0
        uint32_t oldint15;        // 0
        uint16_t olddosmem;       // 640
        uint8_t  bootloaderid;    // 0x30
        uint8_t  sector_shift;    // 11 (2048 bytes/sector for ISO)
        uint16_t dpt_ptr;         // 0
    } mdi;
} linux_mbft_t;

#pragma pack(pop)

// Standard physical addresses for 32-bit Linux boot
#define LINUX_BOOT_PARAMS_PHYS   0x00090000
#define LINUX_CMDLINE_PHYS      0x0009A000
#define LINUX_KERNEL_LOAD_PHYS  0x00100000
#define LINUX_TRAMPOLINE_PHYS   0x00006000
#define LINUX_RAM_ISO_PHYS      0x10000000  // 256 MiB physical address for in-RAM ISO

#define VIDEO_TYPE_VGAC         0x22
#define VIDEO_TYPE_VLFB         0x23

typedef struct linux_kernel_info {
    uint32_t realmode_sectors;
    uint32_t realmode_size;
    uint32_t protected_mode_offset;
    uint32_t protected_mode_size;
    uint32_t code32_start;
    uint16_t protocol_version;
} linux_kernel_info_t;

int  linux_check_kernel_image(const void *image_buf, uint32_t size, linux_kernel_info_t *out_info);
int  linux_prepare_boot_params(const void *kernel_image, uint32_t kernel_size,
                               const void *initrd_buf, uint32_t initrd_size,
                               const char *cmdline, boot_info_t *boot_info,
                               const vbe_mode_info_t *vbe_mode,
                               uint32_t ram_iso_addr, uint32_t ram_iso_size,
                               linux_boot_params_t *out_params);
void linux_setup_mbft(uint32_t iso_phys_addr, uint32_t iso_size);
void linux_boot_jump(uint32_t kernel_source_addr, uint32_t kernel_target_addr,
                    uint32_t kernel_size, uint32_t boot_params_addr,
                    uint32_t entry_point);

#endif // LINUX_BOOT_H
