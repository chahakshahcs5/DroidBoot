#ifndef DISKIO_H
#define DISKIO_H

#include <stdint.h>

typedef uint8_t DSTATUS;

typedef enum {
    RES_OK = 0,
    RES_ERROR,
    RES_WRPRT,
    RES_NOTRDY,
    RES_PARERR
} DRESULT;

// Status bits
#define STA_NOINIT      0x01
#define STA_NODISK      0x02
#define STA_PROTECT     0x04

// Driver registration
void diskio_set_bios_drive(uint8_t drive);
void diskio_disable_bios(void);
void diskio_enable_bios_fallback(void);
struct usb_device;
void diskio_set_usb_msc_device(struct usb_device *dev);

// Functions
DSTATUS disk_initialize(uint8_t pdrv);
DSTATUS disk_status(uint8_t pdrv);
DRESULT disk_read(uint8_t pdrv, uint8_t *buff, uint32_t sector, uint32_t count);
DRESULT disk_write(uint8_t pdrv, const uint8_t *buff, uint32_t sector, uint32_t count);
DRESULT disk_ioctl(uint8_t pdrv, uint8_t cmd, void *buff);

// IOCTL commands
#define CTRL_SYNC           0
#define GET_SECTOR_COUNT    1
#define GET_SECTOR_SIZE     2
#define GET_BLOCK_SIZE      3

#endif // DISKIO_H
