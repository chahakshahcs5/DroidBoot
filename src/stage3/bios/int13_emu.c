// =============================================================================
// INT 13h Emulation — C-Level Dispatch & USB MSC Bridge
//
// This module handles the high-level INT 13h function dispatch:
//   AH=0x41 — Check Extensions Present
//   AH=0x42 — Extended Read Sectors (routes to xHCI USB MSC)
//   AH=0x48 — Extended Get Drive Parameters
//   AH=0x08 — Legacy Get Drive Parameters
//
// The real-mode ASM stub (int13_hook.S) catches INT 13h, switches to PM,
// and calls int13_emu_dispatch() here. We perform the USB MSC read and
// return the result. The ASM stub then switches back to RM and returns.
// =============================================================================

#include "int13_emu.h"
#include "../usb/usb_msc.h"
#include "../debug/vga.h"
#include "../core/printf.h"
#include <stdint.h>

// Local memory helpers (each .c file defines its own in this freestanding codebase)
static void *k_memset(void *dst, int val, unsigned int n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)val;
    return dst;
}
static void *k_memcpy(void *dest, const void *src, unsigned int n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dest;
}

// Resident data block in low memory
static int13_emu_data_t *emu_data = (int13_emu_data_t *)INT13_EMU_DATA_ADDR;

// Bounce buffer for sectors (64KB in low memory, below 640KB boundary)
// Located at 0x10000 (64 KB boundary), cleanly separated from low data structures
#define BOUNCE_BUF_ADDR  0x00010000
#define BOUNCE_BUF_SIZE  (64 * 1024)  // 64 KB

// ASM hook entry points (defined in int13_hook.S)
extern void int13_hook_install(uint32_t emu_data_addr);
extern void int13_hook_uninstall(void);

// =============================================================================
// Install INT 13h emulation
// =============================================================================
int int13_emu_install(usb_device_t *msc_dev, uint8_t lun,
                      uint32_t total_sectors, uint32_t sector_size,
                      uint32_t device_block_size)
{
    if (!msc_dev || !msc_dev->has_msc) {
        log_error("INT13", "Cannot install INT 13h emulation: no MSC device.");
        return -1;
    }

    // Windows VBR / bootmgr expects the primary boot hard drive to be 0x80.
    // The BIOS originally booted our Stage 1 loader from drive 0x80 (SD card / USB).
    // To make Windows boot seamlessly, we map our phone virtual drive to 0x80,
    // and swap the original BIOS drive 0x80 to (0x80 + num_hdd).
    // Our ASM hook redirects calls to (0x80 + num_hdd) back to BIOS 0x80.
    uint8_t num_hdd = *((volatile uint8_t *)0x0475);
    uint8_t orig_swap = (num_hdd > 0) ? (0x80 + num_hdd) : 0x81;

    // Initialize resident data block
    k_memset(emu_data, 0, sizeof(int13_emu_data_t));
    emu_data->virtual_drive    = 0x80;
    emu_data->orig_swap_drive  = orig_swap;
    emu_data->active           = 1;
    emu_data->msc_dev          = msc_dev;
    emu_data->lun              = lun;
    emu_data->total_sectors    = total_sectors;
    emu_data->sector_size      = sector_size ? sector_size : 512;
    emu_data->device_block_size = device_block_size ? device_block_size : 512;
    emu_data->bounce_buf_phys  = BOUNCE_BUF_ADDR;
    emu_data->bounce_buf_size  = BOUNCE_BUF_SIZE;

    // Save original INT 13h vector from IVT
    volatile uint32_t *ivt = (volatile uint32_t *)0x00000000;
    emu_data->original_int13_vec = ivt[0x13];

    // Increment BIOS HDD count so Windows/BIOS callers see all drives
    *((volatile uint8_t *)0x0475) = num_hdd + 1;

    // Install the ASM hook (modifies IVT[0x13] to point to our handler)
    int13_hook_install(INT13_EMU_DATA_ADDR);

    log_info("INT13", "INT 13h emulation installed: Virtual Drive 0x80 (original drive swapped to 0x%02X)", orig_swap);
    log_info("INT13", "  USB Device: Slot %u, LUN %u, %u sectors (%u MB), phys_block=%u",
             msc_dev->slot_id, lun, total_sectors,
             (uint32_t)((uint64_t)total_sectors * 512 / 1024 / 1024),
             emu_data->device_block_size);
    log_info("INT13", "  Original INT 13h Vector: 0x%08X", emu_data->original_int13_vec);

    return 0;
}

// =============================================================================
// Uninstall INT 13h emulation
// =============================================================================
void int13_emu_uninstall(void)
{
    if (!emu_data->active) return;

    // Restore original INT 13h vector
    volatile uint32_t *ivt = (volatile uint32_t *)0x00000000;
    ivt[0x13] = emu_data->original_int13_vec;

    // Decrement HDD count
    uint8_t num_hdd = *((volatile uint8_t *)0x0475);
    if (num_hdd > 0) {
        *((volatile uint8_t *)0x0475) = num_hdd - 1;
    }

    emu_data->active = 0;
    log_info("INT13", "INT 13h emulation uninstalled. Original BIOS handler restored.");
}

// =============================================================================
// Get virtual drive number
// =============================================================================
uint8_t int13_emu_get_drive_num(void)
{
    return emu_data->virtual_drive;
}

// =============================================================================
// Helper to read 512-byte logical sectors with block size translation
// =============================================================================
static int int13_read_helper(uint32_t lba, uint16_t count, uint32_t buf_linear)
{
    if (!emu_data->msc_dev || count == 0) return 0;

    uint32_t dev_blk = emu_data->device_block_size;
    if (dev_blk == 0) dev_blk = 512;

    if (dev_blk == 512) {
        uint32_t max_sec = BOUNCE_BUF_SIZE / 512;
        while (count > 0) {
            uint16_t chunk = count;
            if (chunk > max_sec) chunk = (uint16_t)max_sec;

            int res = usb_msc_read_sectors_lun(
                emu_data->msc_dev, emu_data->lun,
                lba, chunk, 512, (void *)BOUNCE_BUF_ADDR
            );
            if (res != 0) return 0x20; // AH=0x20 = Controller failure

            k_memcpy((void *)buf_linear, (void *)BOUNCE_BUF_ADDR, (uint32_t)chunk * 512);

            lba += chunk;
            buf_linear += (uint32_t)chunk * 512;
            count -= chunk;
        }
        return 0;
    } else if (dev_blk == 2048) {
        // Physical device uses 2048B sectors (e.g. CD-ROM emulation).
        // Each physical block holds 4 logical 512B sectors.
        while (count > 0) {
            uint32_t sec_offset = lba % 4;
            if (sec_offset == 0 && count >= 4) {
                // Aligned multi-block read
                uint16_t phys_blocks = count / 4;
                uint32_t max_phys = BOUNCE_BUF_SIZE / 2048;
                if (phys_blocks > max_phys) phys_blocks = (uint16_t)max_phys;

                int res = usb_msc_read_sectors_lun(
                    emu_data->msc_dev, emu_data->lun,
                    lba / 4, phys_blocks, 2048, (void *)BOUNCE_BUF_ADDR
                );
                if (res != 0) return 0x20;

                uint32_t bytes = (uint32_t)phys_blocks * 2048;
                k_memcpy((void *)buf_linear, (void *)BOUNCE_BUF_ADDR, bytes);

                uint16_t sec_read = phys_blocks * 4;
                lba += sec_read;
                buf_linear += bytes;
                count -= sec_read;
            } else {
                // Unaligned or partial block read
                uint32_t phys_lba = lba / 4;
                uint32_t sec_avail = 4 - sec_offset;
                uint16_t take = (count < sec_avail) ? count : (uint16_t)sec_avail;

                int res = usb_msc_read_sectors_lun(
                    emu_data->msc_dev, emu_data->lun,
                    phys_lba, 1, 2048, (void *)BOUNCE_BUF_ADDR
                );
                if (res != 0) return 0x20;

                k_memcpy((void *)buf_linear, (void *)(BOUNCE_BUF_ADDR + sec_offset * 512), (uint32_t)take * 512);

                lba += take;
                buf_linear += (uint32_t)take * 512;
                count -= take;
            }
        }
        return 0;
    } else {
        return 0x20;
    }
}

// =============================================================================
// C-Level INT 13h Dispatch (called from ASM stub in protected mode)
//
// This is the core handler — called when our virtual drive is accessed.
// The ASM stub has already verified DL matches our virtual_drive.
//
// Returns: 0 on success (CF=0), or AH error code on failure (CF=1)
// =============================================================================
int int13_emu_dispatch(uint8_t ah_function, uint32_t dap_phys, uint8_t drive)
{
    (void)drive; // Always our virtual drive when we get here

    switch (ah_function) {
        // -----------------------------------------------------------------
        // AH=0x00: Reset Disk System
        // AH=0x01: Get Status of Last Drive Operation
        // AH=0x04: Verify Disk Sectors
        // -----------------------------------------------------------------
        case 0x00:
        case 0x01:
        case 0x04:
            return 0; // Success (AH=0, CF=0)

        // -----------------------------------------------------------------
        // AH=0x02: Legacy Read Sectors (CHS)
        //   Input:  AL = Sector count
        //           CH = Cylinder bits 0-7
        //           CL = Cylinder bits 8-9 (bits 6-7), Sector 1-63 (bits 0-5)
        //           DH = Head
        //           ES:BX -> Destination buffer (passed via dap_phys)
        // -----------------------------------------------------------------
        case 0x02: {
            volatile uint8_t *mailbox = (volatile uint8_t *)INT13_HOOK_MAILBOX_ADDR;
            uint8_t count = mailbox[0x0C]; // AL
            uint16_t cx = *(volatile uint16_t *)(mailbox + 0x10);
            uint16_t dx = *(volatile uint16_t *)(mailbox + 0x12);

            if (count == 0) return 0;

            uint32_t cyl = (uint32_t)(cx >> 8) | ((uint32_t)(cx & 0xC0) << 2);
            uint32_t sec = (uint32_t)(cx & 0x3F);
            uint32_t head = (uint32_t)(dx >> 8);

            uint32_t heads = 255;
            uint32_t spt = 63;
            uint32_t lba = (cyl * heads + head) * spt + (sec > 0 ? sec - 1 : 0);

            return int13_read_helper(lba, count, dap_phys);
        }

        // -----------------------------------------------------------------
        // AH=0x41: Check Extensions Present
        //   Input:  BX=0x55AA, DL=drive
        //   Output: BX=0xAA55, CX=support bitmap, AH=version
        //   CF=0 on success (handled in int13_hook.S)
        // -----------------------------------------------------------------
        case 0x41: {
            return 0; // Handled directly in int13_hook.S (.ret_41h)
        }

        // -----------------------------------------------------------------
        // AH=0x42: Extended Read Sectors (LBA)
        //   Input:  DS:SI -> Disk Address Packet (DAP)
        //   Output: CF=0 success, CF=1 error (AH=error code)
        // -----------------------------------------------------------------
        case 0x42: {
            volatile int13_dap_t *dap = (volatile int13_dap_t *)dap_phys;
            uint32_t lba = (uint32_t)dap->lba;
            uint16_t count = dap->count;
            uint32_t buf_linear = ((uint32_t)dap->buf_segment << 4) + (uint32_t)dap->buf_offset;

            if (count == 0) return 0;
            return int13_read_helper(lba, count, buf_linear);
        }

        // -----------------------------------------------------------------
        // AH=0x48: Extended Get Drive Parameters
        //   Input:  DS:SI -> buffer for drive parameters
        //   Output: CF=0, buffer filled
        // -----------------------------------------------------------------
        case 0x48: {
            volatile int13_drive_params_t *params = (volatile int13_drive_params_t *)dap_phys;

            // Calculate CHS geometry from total sectors
            uint32_t total = emu_data->total_sectors;
            uint32_t heads = 255;
            uint32_t spt = 63;
            uint32_t cyls = total / (heads * spt);
            if (cyls == 0) cyls = 1;

            params->size             = 26;
            params->flags            = 0x0002; // Bit 1: geometry valid
            params->cylinders        = cyls;
            params->heads            = heads;
            params->sectors_per_track = spt;
            params->total_sectors    = (uint64_t)total;
            params->bytes_per_sector = 512;
            return 0;
        }

        // -----------------------------------------------------------------
        // AH=0x08: Legacy Get Drive Parameters (CHS)
        //   Returns CHS geometry in registers via emu_data for ASM stub
        // -----------------------------------------------------------------
        case 0x08: {
            uint32_t total = emu_data->total_sectors;
            uint32_t heads = 255;
            uint32_t spt = 63;
            uint32_t cyls = total / (heads * spt);
            if (cyls == 0) cyls = 1;
            if (cyls > 1023) cyls = 1023;

            emu_data->res_ch = (uint8_t)(cyls & 0xFF);
            emu_data->res_cl = (uint8_t)(spt | ((cyls >> 2) & 0xC0));
            emu_data->res_dh = (uint8_t)(heads - 1);
            emu_data->res_dl = *((volatile uint8_t *)0x0475);
            return 0;
        }

        // -----------------------------------------------------------------
        // AH=0x15: Get Disk Type
        //   Returns drive type in AH (03 = hard disk with valid geometry)
        // -----------------------------------------------------------------
        case 0x15: {
            return 0; // Handled directly in int13_hook.S (.ret_15h)
        }

        default:
            // Unsupported function — return error
            return 0x01; // AH=0x01 = Invalid function
    }
}
