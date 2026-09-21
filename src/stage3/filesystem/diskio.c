#include "diskio.h"
#include "../bios/bios_disk.h"
#include "../usb/usb_msc.h"
#include "../core/printf.h"

static uint8_t      bios_drive = 0x80;
static usb_device_t *msc_device = NULL;

void diskio_set_bios_drive(uint8_t drive) {
    bios_drive = drive;
}

void diskio_set_usb_msc_device(usb_device_t *dev) {
    msc_device = dev;
}

DSTATUS disk_initialize(uint8_t pdrv) {
    if (pdrv == 0) {
        return 0; // BIOS drive ready
    } else if (pdrv == 1) {
        if (!msc_device || !msc_device->has_msc) return STA_NODISK;
        return (usb_msc_init_device(msc_device) == 0) ? 0 : STA_NOINIT;
    }
    return STA_NODISK;
}

DSTATUS disk_status(uint8_t pdrv) {
    if (pdrv == 0) return 0;
    if (pdrv == 1) {
        return (msc_device && msc_device->has_msc) ? 0 : STA_NODISK;
    }
    return STA_NODISK;
}

DRESULT disk_read(uint8_t pdrv, uint8_t *buff, uint32_t sector, uint32_t count) {
    if (!buff || count == 0) return RES_PARERR;

    if (pdrv == 0) {
        int res = bios_disk_read(bios_drive, sector, (uint16_t)count, buff);
        return (res == 0) ? RES_OK : RES_ERROR;
    } else if (pdrv == 1) {
        if (!msc_device) return RES_NOTRDY;
        int res = usb_msc_read_sectors(msc_device, sector, (uint16_t)count, buff);
        return (res == 0) ? RES_OK : RES_ERROR;
    }

    return RES_PARERR;
}

DRESULT disk_write(uint8_t pdrv, const uint8_t *buff, uint32_t sector, uint32_t count) {
    if (!buff || count == 0) return RES_PARERR;

    if (pdrv == 0) {
        int res = bios_disk_write(bios_drive, sector, (uint16_t)count, buff);
        return (res == 0) ? RES_OK : RES_ERROR;
    } else if (pdrv == 1) {
        if (!msc_device) return RES_NOTRDY;
        int res = usb_msc_write_sectors(msc_device, sector, (uint16_t)count, buff);
        return (res == 0) ? RES_OK : RES_ERROR;
    }

    return RES_PARERR;
}

DRESULT disk_ioctl(uint8_t pdrv, uint8_t cmd, void *buff) {
    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;

        case GET_SECTOR_COUNT:
            if (pdrv == 0) {
                if (buff) *(uint32_t *)buff = 131072; // 64 MB default
                return RES_OK;
            } else if (pdrv == 1 && msc_device) {
                uint32_t last_lba = 0, block_sz = 0;
                if (usb_msc_read_capacity(msc_device, &last_lba, &block_sz) == 0 && buff) {
                    *(uint32_t *)buff = last_lba + 1;
                    return RES_OK;
                }
            }
            return RES_ERROR;

        case GET_SECTOR_SIZE:
            if (buff) {
                if (pdrv == 0) {
                    *(uint16_t *)buff = 512;
                } else if (pdrv == 1 && msc_device) {
                    uint32_t last_lba = 0, block_sz = 512;
                    usb_msc_read_capacity(msc_device, &last_lba, &block_sz);
                    *(uint16_t *)buff = (uint16_t)(block_sz ? block_sz : 512);
                } else {
                    *(uint16_t *)buff = 512;
                }
            }
            return RES_OK;

        case GET_BLOCK_SIZE:
            if (buff) *(uint32_t *)buff = 1; // 1 sector erase block
            return RES_OK;

        default:
            return RES_PARERR;
    }
}
