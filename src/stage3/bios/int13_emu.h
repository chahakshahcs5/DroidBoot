// =============================================================================
// INT 13h Emulation Layer — Makes Phone UMS Visible to Windows VBR
//
// This module installs a custom INT 13h handler in the IVT that intercepts
// disk read requests for a virtual drive number and routes them through our
// xHCI USB Mass Storage driver to the phone's UMS LUN.
//
// Supported INT 13h functions:
//   AH=0x41 — Check Extensions Present (LBA extensions)
//   AH=0x42 — Extended Read Sectors (LBA)
//   AH=0x48 — Extended Get Drive Parameters
//   AH=0x08 — Legacy Get Drive Parameters (CHS)
//   All others — Chained to original BIOS INT 13h handler
// =============================================================================

#ifndef INT13_EMU_H
#define INT13_EMU_H

#include <stdint.h>
#include <stdbool.h>
#include "../usb/usb.h"

// Resident data block address in low memory (must be below 640KB)
#define INT13_EMU_DATA_ADDR       0x5000
#define INT13_HOOK_MAILBOX_ADDR   0x5080

// Disk Address Packet (DAP) — used by AH=42h Extended Read
#pragma pack(push, 1)
typedef struct {
    uint8_t  size;           // Size of DAP (16 bytes)
    uint8_t  reserved;       // Always 0
    uint16_t count;          // Number of sectors to read
    uint16_t buf_offset;     // Transfer buffer offset (real-mode segment:offset)
    uint16_t buf_segment;    // Transfer buffer segment
    uint64_t lba;            // Starting LBA (absolute sector number)
} int13_dap_t;

// Extended Drive Parameters — returned by AH=48h
typedef struct {
    uint16_t size;           // Size of this structure (26 minimum)
    uint16_t flags;          // Information flags
    uint32_t cylinders;      // Physical cylinders
    uint32_t heads;          // Physical heads
    uint32_t sectors_per_track;  // Physical sectors per track
    uint64_t total_sectors;  // Total number of sectors
    uint16_t bytes_per_sector;   // Bytes per sector
} int13_drive_params_t;

// Resident INT 13h emulation data block (installed in low memory)
typedef struct {
    uint8_t        virtual_drive;      // Our virtual drive number (e.g., 0x81)
    uint8_t        active;             // 1 = handler installed and active
    uint32_t       original_int13_vec; // Saved original INT 13h IVT vector (seg:off)
    
    // USB device reference for xHCI MSC reads
    usb_device_t  *msc_dev;           // Pointer to the USB device struct
    uint8_t        lun;               // LUN to read from (0 = ISO, 1 = persistence)
    uint32_t       total_sectors;     // Total sector count from READ_CAPACITY
    uint32_t       sector_size;       // Sector size (usually 512)
    
    // Bounce buffer for DMA (must be in low memory for real-mode access)
    uint32_t       bounce_buf_phys;   // Physical address of bounce buffer
    uint32_t       bounce_buf_size;   // Size of bounce buffer

    // Legacy CHS return values for AH=0x08
    uint8_t        res_ch;
    uint8_t        res_cl;
    uint8_t        res_dh;
    uint8_t        res_dl;

    // Drive swapping and physical block size translation
    uint8_t        orig_swap_drive;    // Swapped original drive (e.g., 0x83 -> remapped to 0x80)
    uint32_t       device_block_size;  // Physical block size of MSC device (512 or 2048)
} int13_emu_data_t;
#pragma pack(pop)

// Install INT 13h emulation handler for the given USB MSC device
// Returns: 0 on success, -1 on failure
// After this call, INT 13h requests for the virtual drive are handled by our code.
int int13_emu_install(usb_device_t *msc_dev, uint8_t lun,
                      uint32_t total_sectors, uint32_t sector_size,
                      uint32_t device_block_size);

// Uninstall the INT 13h handler and restore original IVT vector
void int13_emu_uninstall(void);

// Get the assigned virtual drive number (valid only after install)
uint8_t int13_emu_get_drive_num(void);

// C-level handler called from the ASM stub when our virtual drive is accessed.
// This is NOT called directly by user code — it's invoked by int13_hook.S
// Returns: 0 on success, AH error code on failure
int int13_emu_dispatch(uint8_t ah_function, uint32_t dap_phys, uint8_t drive);

#endif // INT13_EMU_H
