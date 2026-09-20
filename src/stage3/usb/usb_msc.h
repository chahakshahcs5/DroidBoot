#ifndef USB_MSC_H
#define USB_MSC_H

#include "usb.h"
#include <stdint.h>
#include <stdbool.h>

// SCSI Command Opcodes
#define SCSI_TEST_UNIT_READY    0x00
#define SCSI_REQUEST_SENSE      0x03
#define SCSI_INQUIRY            0x12
#define SCSI_READ_CAPACITY_10   0x25
#define SCSI_READ_10            0x28
#define SCSI_WRITE_10           0x2A

#pragma pack(push, 1)

// Command Block Wrapper (CBW) - 31 bytes
typedef struct {
    uint32_t dCBWSignature;          // 0x43425355 ("USBC")
    uint32_t dCBWTag;                // Command identifier tag
    uint32_t dCBWDataTransferLength; // Number of bytes to transfer
    uint8_t  bmCBWFlags;             // 0x00 = Host-to-Device, 0x80 = Device-to-Host
    uint8_t  bCBWLUN;                // Logical Unit Number (0)
    uint8_t  bCBWCBLength;           // Length of SCSI CDB (e.g. 6 or 10)
    uint8_t  CBWCB[16];              // SCSI Command Descriptor Block
} usb_msc_cbw_t;

// Command Status Wrapper (CSW) - 13 bytes
typedef struct {
    uint32_t dCSWSignature;          // 0x53425355 ("USBS")
    uint32_t dCSWTag;                // Must match dCBWTag
    uint32_t dCSWDataResidue;        // Difference between reported and actual
    uint8_t  bCSWStatus;             // 0 = Success, 1 = Failed, 2 = Phase Error
} usb_msc_csw_t;

#pragma pack(pop)

// Initialize and test USB Mass Storage device
int usb_msc_init_device(usb_device_t *dev);

// Read 512-byte sectors from USB Mass Storage device
int usb_msc_read_sectors(usb_device_t *dev, uint32_t lba, uint16_t count, void *buf);

// Write 512-byte sectors to USB Mass Storage device
int usb_msc_write_sectors(usb_device_t *dev, uint32_t lba, uint16_t count, const void *buf);

// Read capacity (10) from USB Mass Storage device
int usb_msc_read_capacity(usb_device_t *dev, uint32_t *out_last_lba, uint32_t *out_block_size);

// Create a generic boot_source_t wrapping a USB Mass Storage device
#include "../../include/boot_source.h"
boot_source_t *boot_source_msc_create(usb_device_t *dev);

#endif // USB_MSC_H
