#include "usb_msc.h"
#include "../../include/io.h"
#include "../core/printf.h"
#include "../memory/memory.h"
#include <stddef.h>

static uint32_t msc_tag_counter = 0x88000001;

static int usb_msc_send_cbw(usb_device_t *dev, uint32_t tag, uint32_t length, uint8_t flags, uint8_t cb_len, const uint8_t *cb) {
    if (!dev || !dev->has_msc) return -1;

    usb_msc_cbw_t cbw;
    cbw.dCBWSignature = 0x43425355; // "USBC"
    cbw.dCBWTag = tag;
    cbw.dCBWDataTransferLength = length;
    cbw.bmCBWFlags = flags;
    cbw.bCBWLUN = 0;
    cbw.bCBWCBLength = cb_len;

    for (int i = 0; i < 16; i++) {
        cbw.CBWCB[i] = (i < cb_len) ? cb[i] : 0;
    }

    uint32_t sent = 0;
    int res = usb_bulk_transfer(dev, dev->msc_bulk_out_ep, &cbw, sizeof(cbw), &sent);
    if (res != 0) {
        log_error("MSC", "Failed to send CBW (error %d)", res);
    }
    return res;
}

static int usb_msc_recv_csw(usb_device_t *dev, uint32_t expected_tag) {
    if (!dev || !dev->has_msc) return -1;

    usb_msc_csw_t csw;
    uint32_t recv = 0;
    int res = usb_bulk_transfer(dev, dev->msc_bulk_in_ep, &csw, sizeof(csw), &recv);
    if (res != 0) {
        log_error("MSC", "Failed to receive CSW (error %d)", res);
        return res;
    }

    if (csw.dCSWSignature != 0x53425355) { // "USBS"
        log_error("MSC", "Invalid CSW signature: 0x%08X", csw.dCSWSignature);
        return -2;
    }

    if (csw.dCSWTag != expected_tag) {
        log_error("MSC", "CSW Tag mismatch: got 0x%08X, expected 0x%08X", csw.dCSWTag, expected_tag);
        return -3;
    }

    if (csw.bCSWStatus != 0) {
        log_error("MSC", "CSW Status returned error %u (residue %u)", csw.bCSWStatus, csw.dCSWDataResidue);
        return -4;
    }

    return 0;
}

int usb_msc_init_device(usb_device_t *dev) {
    if (!dev || !dev->has_msc) return -1;

    // Retry TEST UNIT READY up to 5 times to clear Unit Attention after USB port reset
    int res = -1;
    for (int retry = 0; retry < 5; retry++) {
        uint32_t tag = ++msc_tag_counter;
        uint8_t cdb[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };

        if (usb_msc_send_cbw(dev, tag, 0, 0x00, 6, cdb) == 0) {
            res = usb_msc_recv_csw(dev, tag);
            if (res == 0) {
                log_info("MSC", "USB Mass Storage device is READY on Port %u.", dev->port_num);
                return 0;
            }
        }
        // Unit Attention or Busy: wait 20ms and retry
        for (int d = 0; d < 20000; d++) io_wait();
    }

    log_info("MSC", "USB Mass Storage Test Unit Ready status: %d. Ready for I/O.", res);
    return 0;
}

int usb_msc_read_sectors(usb_device_t *dev, uint32_t lba, uint16_t count, void *buf) {
    if (!dev || !dev->has_msc || !buf || count == 0) return -1;

    uint8_t *ptr = (uint8_t *)buf;
    uint32_t cur_lba = lba;
    uint16_t remaining = count;

    while (remaining > 0) {
        uint16_t chunk = (remaining > 16) ? 16 : remaining;
        uint32_t chunk_bytes = (uint32_t)chunk * 512;
        uint32_t tag = ++msc_tag_counter;

        uint8_t cdb[10];
        cdb[0] = SCSI_READ_10;
        cdb[1] = 0;
        cdb[2] = (uint8_t)((cur_lba >> 24) & 0xFF);
        cdb[3] = (uint8_t)((cur_lba >> 16) & 0xFF);
        cdb[4] = (uint8_t)((cur_lba >> 8) & 0xFF);
        cdb[5] = (uint8_t)(cur_lba & 0xFF);
        cdb[6] = 0;
        cdb[7] = (uint8_t)((chunk >> 8) & 0xFF);
        cdb[8] = (uint8_t)(chunk & 0xFF);
        cdb[9] = 0;

        int res = usb_msc_send_cbw(dev, tag, chunk_bytes, 0x80, 10, cdb);
        if (res != 0) return res;

        uint32_t transferred = 0;
        res = usb_bulk_transfer(dev, dev->msc_bulk_in_ep, ptr, chunk_bytes, &transferred);
        if (res != 0) return res;

        res = usb_msc_recv_csw(dev, tag);
        if (res != 0) return res;

        cur_lba += chunk;
        ptr += chunk_bytes;
        remaining -= chunk;
    }

    return 0;
}

int usb_msc_write_sectors(usb_device_t *dev, uint32_t lba, uint16_t count, const void *buf) {
    if (!dev || !dev->has_msc || !buf || count == 0) return -1;

    const uint8_t *ptr = (const uint8_t *)buf;
    uint32_t cur_lba = lba;
    uint16_t remaining = count;

    while (remaining > 0) {
        uint16_t chunk = (remaining > 16) ? 16 : remaining;
        uint32_t chunk_bytes = (uint32_t)chunk * 512;
        uint32_t tag = ++msc_tag_counter;

        uint8_t cdb[10];
        cdb[0] = SCSI_WRITE_10;
        cdb[1] = 0;
        cdb[2] = (uint8_t)((cur_lba >> 24) & 0xFF);
        cdb[3] = (uint8_t)((cur_lba >> 16) & 0xFF);
        cdb[4] = (uint8_t)((cur_lba >> 8) & 0xFF);
        cdb[5] = (uint8_t)(cur_lba & 0xFF);
        cdb[6] = 0;
        cdb[7] = (uint8_t)((chunk >> 8) & 0xFF);
        cdb[8] = (uint8_t)(chunk & 0xFF);
        cdb[9] = 0;

        int res = usb_msc_send_cbw(dev, tag, chunk_bytes, 0x00, 10, cdb);
        if (res != 0) return res;

        uint32_t transferred = 0;
        res = usb_bulk_transfer(dev, dev->msc_bulk_out_ep, (void *)ptr, chunk_bytes, &transferred);
        if (res != 0) return res;

        res = usb_msc_recv_csw(dev, tag);
        if (res != 0) return res;

        cur_lba += chunk;
        ptr += chunk_bytes;
        remaining -= chunk;
    }

    return 0;
}

int usb_msc_read_capacity(usb_device_t *dev, uint32_t *out_last_lba, uint32_t *out_block_size) {
    if (!dev || !dev->has_msc) return -1;

    uint32_t tag = ++msc_tag_counter;
    uint8_t cdb[10] = { SCSI_READ_CAPACITY_10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    int res = usb_msc_send_cbw(dev, tag, 8, 0x80, 10, cdb);
    if (res != 0) return res;

    uint8_t cap_data[8];
    uint32_t transferred = 0;
    res = usb_bulk_transfer(dev, dev->msc_bulk_in_ep, cap_data, 8, &transferred);
    if (res != 0) return res;

    res = usb_msc_recv_csw(dev, tag);
    if (res != 0) return res;

    uint32_t last_lba = ((uint32_t)cap_data[0] << 24) |
                        ((uint32_t)cap_data[1] << 16) |
                        ((uint32_t)cap_data[2] << 8)  |
                        (uint32_t)cap_data[3];
    uint32_t block_size = ((uint32_t)cap_data[4] << 24) |
                          ((uint32_t)cap_data[5] << 16) |
                          ((uint32_t)cap_data[6] << 8)  |
                          (uint32_t)cap_data[7];

    if (out_last_lba) *out_last_lba = last_lba;
    if (out_block_size) *out_block_size = block_size;

    return 0;
}

typedef struct msc_source_priv {
    usb_device_t *dev;
    uint64_t      offset;
    uint64_t      total_size;
} msc_source_priv_t;

static int msc_src_open(boot_source_t *src, const char *path) {
    (void)src;
    (void)path;
    return 0;
}

static uint32_t msc_src_read(boot_source_t *src, void *buf, uint32_t size) {
    msc_source_priv_t *priv = (msc_source_priv_t *)src->priv;
    if (!priv || !priv->dev || !buf || size == 0) return 0;

    uint32_t bytes_read = 0;
    uint8_t *dst = (uint8_t *)buf;

    while (bytes_read < size) {
        uint64_t cur_pos = priv->offset + bytes_read;
        uint32_t lba = (uint32_t)(cur_pos >> 9);
        uint32_t sec_offset = (uint32_t)(cur_pos & 511);

        if (sec_offset == 0 && (size - bytes_read) >= 512) {
            uint32_t sectors_to_read = (size - bytes_read) >> 9;
            if (sectors_to_read > 64) sectors_to_read = 64; // Max 32KB per bulk transfer
            int res = usb_msc_read_sectors(priv->dev, lba, (uint16_t)sectors_to_read, dst + bytes_read);
            if (res != 0) {
                log_error("MSC", "Read failed at LBA %u (error %d, read %u / %u bytes)", lba, res, bytes_read, size);
                break;
            }
            bytes_read += sectors_to_read * 512;
        } else {
            static uint8_t bounce[512];
            int res = usb_msc_read_sectors(priv->dev, lba, 1, bounce);
            if (res != 0) {
                log_error("MSC", "Read bounce failed at LBA %u (error %d, read %u / %u bytes)", lba, res, bytes_read, size);
                break;
            }
            uint32_t chunk = 512 - sec_offset;
            if (chunk > (size - bytes_read)) chunk = size - bytes_read;
            for (uint32_t i = 0; i < chunk; i++) {
                dst[bytes_read + i] = bounce[sec_offset + i];
            }
            bytes_read += chunk;
        }
    }

    priv->offset += bytes_read;
    return bytes_read;
}

static int msc_src_seek(boot_source_t *src, uint64_t offset) {
    msc_source_priv_t *priv = (msc_source_priv_t *)src->priv;
    if (!priv) return -1;
    priv->offset = offset;
    return 0;
}

static uint64_t msc_src_tell(boot_source_t *src) {
    msc_source_priv_t *priv = (msc_source_priv_t *)src->priv;
    return priv ? priv->offset : 0;
}

static uint64_t msc_src_size(boot_source_t *src) {
    msc_source_priv_t *priv = (msc_source_priv_t *)src->priv;
    return priv ? priv->total_size : 0;
}

static void msc_src_close(boot_source_t *src) {
    (void)src;
}

boot_source_t *boot_source_msc_create(usb_device_t *dev) {
    if (!dev || !dev->has_msc) return NULL;

    msc_source_priv_t *priv = (msc_source_priv_t *)kmalloc(sizeof(msc_source_priv_t));
    if (!priv) return NULL;

    priv->dev = dev;
    priv->offset = 0;
    priv->total_size = 0;

    uint32_t last_lba = 0;
    uint32_t block_size = 512;
    if (usb_msc_read_capacity(dev, &last_lba, &block_size) == 0 && block_size > 0) {
        priv->total_size = (uint64_t)(last_lba + 1) * block_size;
        log_info("MSC", "USB MSC Capacity: %u MB (%u sectors of %u B)",
                 (uint32_t)(priv->total_size >> 20), last_lba + 1, block_size);
    } else {
        log_info("MSC", "Could not read capacity, defaulting to unconstrained block access.");
    }

    boot_source_t *src = (boot_source_t *)kmalloc(sizeof(boot_source_t));
    if (!src) {
        kfree(priv);
        return NULL;
    }

    src->name = "USB-MSC";
    src->open = msc_src_open;
    src->read = msc_src_read;
    src->seek = msc_src_seek;
    src->tell = msc_src_tell;
    src->size = msc_src_size;
    src->close = msc_src_close;
    src->priv = priv;

    return src;
}

