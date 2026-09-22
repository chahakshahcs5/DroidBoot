#include "usb_msc.h"
#include "../../include/io.h"
#include "../core/printf.h"
#include "../core/timer.h"
#include "../memory/memory.h"
#include <stddef.h>

static uint32_t msc_tag_counter = 0x88000001;

static const char *scsi_opcode_name(uint8_t opcode) {
    switch (opcode) {
        case SCSI_TEST_UNIT_READY:   return "TEST_UNIT_READY (0x00)";
        case SCSI_REQUEST_SENSE:     return "REQUEST_SENSE (0x03)";
        case SCSI_INQUIRY:           return "INQUIRY (0x12)";
        case SCSI_MODE_SENSE:        return "MODE_SENSE (0x1A)";
        case SCSI_READ_CAPACITY_10:  return "READ_CAPACITY_10 (0x25)";
        case SCSI_READ_10:           return "READ_10 (0x28)";
        case SCSI_WRITE_10:          return "WRITE_10 (0x2A)";
        default:                     return "SCSI_COMMAND";
    }
}

static int usb_msc_send_cbw_lun(usb_device_t *dev, uint8_t lun, uint32_t tag, uint32_t length,
                                uint8_t flags, uint8_t cb_len, const uint8_t *cb) {
    if (!dev || !dev->has_msc) return -1;

    usb_msc_cbw_t cbw;
    cbw.dCBWSignature = 0x43425355; // "USBC"
    cbw.dCBWTag = tag;
    cbw.dCBWDataTransferLength = length;
    cbw.bmCBWFlags = flags;
    cbw.bCBWLUN = lun;
    cbw.bCBWCBLength = cb_len;

    for (int i = 0; i < 16; i++) {
        cbw.CBWCB[i] = (i < cb_len) ? cb[i] : 0;
    }

    if (cb[0] != SCSI_READ_10 && cb[0] != SCSI_WRITE_10) {
        log_debug("MSC", "CBW: Tag=0x%08X LUN=%u Op=%s Len=%u Dir=%s",
                  tag, lun, scsi_opcode_name(cb[0]), length, (flags & 0x80) ? "IN" : "OUT");
    }

    uint32_t sent = 0;
    int res = usb_bulk_transfer(dev, dev->msc_bulk_out_ep, &cbw, sizeof(cbw), &sent);
    if (res != 0) {
        log_error("MSC", "Failed to send CBW (error %d, LUN %u, Opcode %s)", res, lun, scsi_opcode_name(cb[0]));
    }
    return res;
}

static int usb_msc_recv_csw(usb_device_t *dev, uint32_t expected_tag) {
    if (!dev || !dev->has_msc) return -1;

    usb_msc_csw_t csw;
    uint32_t recv = 0;
    int res = usb_bulk_transfer(dev, dev->msc_bulk_in_ep, &csw, sizeof(csw), &recv);
    if (res != 0) {
        usb_clear_endpoint_halt(dev, dev->msc_bulk_in_ep);
        log_error("MSC", "Failed to receive CSW (error %d)", res);
        return res;
    }

    if (csw.dCSWSignature != 0x53425355) { // "USBS"
        log_error("MSC", "Invalid CSW signature: 0x%08X (expected 0x53425355)", csw.dCSWSignature);
        return -2;
    }

    if (csw.dCSWTag != expected_tag) {
        log_error("MSC", "CSW Tag mismatch: got 0x%08X, expected 0x%08X", csw.dCSWTag, expected_tag);
        return -3;
    }

    if (csw.bCSWStatus != 0) {
        const char *csw_stat_str = (csw.bCSWStatus == 1) ? "Command Failed" : (csw.bCSWStatus == 2 ? "Phase Error" : "Reserved Error");
        log_error("MSC", "CSW Status returned %u (%s), residue %u B",
                  csw.bCSWStatus, csw_stat_str, csw.dCSWDataResidue);
        return -4;
    }

    if (csw.dCSWDataResidue > 0) {
        log_debug("MSC", "CSW: Tag=0x%08X Status=0 Residue=%u", csw.dCSWTag, csw.dCSWDataResidue);
    }
    return 0;
}

int usb_msc_init_lun(usb_device_t *dev, uint8_t lun) {
    if (!dev || !dev->has_msc) return -1;

    int res = -1;
    for (int retry = 0; retry < 5; retry++) {
        uint32_t tag = ++msc_tag_counter;
        uint8_t cdb[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };

        if (usb_msc_send_cbw_lun(dev, lun, tag, 0, 0x00, 6, cdb) == 0) {
            res = usb_msc_recv_csw(dev, tag);
            if (res == 0) {
                log_info("MSC", "USB Mass Storage device LUN %u is READY on Port %u.", lun, dev->port_num);
                return 0;
            }
        }
        timer_mdelay(20);
    }

    return res;
}

int usb_msc_init_device(usb_device_t *dev) {
    return usb_msc_init_lun(dev, 0);
}

int usb_msc_read_sectors_lun(usb_device_t *dev, uint8_t lun, uint32_t lba, uint16_t count, uint32_t block_size, void *buf) {
    if (!dev || !dev->has_msc || !buf || count == 0) return -1;
    if (block_size == 0) block_size = 512;

    uint8_t *ptr = (uint8_t *)buf;
    uint32_t cur_lba = lba;
    uint16_t remaining = count;

    while (remaining > 0) {
        uint16_t chunk = (remaining > 16) ? 16 : remaining;
        uint32_t chunk_bytes = (uint32_t)chunk * block_size;
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

        int res = usb_msc_send_cbw_lun(dev, lun, tag, chunk_bytes, 0x80, 10, cdb);
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

int usb_msc_read_sectors(usb_device_t *dev, uint32_t lba, uint16_t count, void *buf) {
    return usb_msc_read_sectors_lun(dev, 0, lba, count, 512, buf);
}

int usb_msc_write_sectors_lun(usb_device_t *dev, uint8_t lun, uint32_t lba, uint16_t count, uint32_t block_size, const void *buf) {
    if (!dev || !dev->has_msc || !buf || count == 0) return -1;
    if (block_size == 0) block_size = 512;

    const uint8_t *ptr = (const uint8_t *)buf;
    uint32_t cur_lba = lba;
    uint16_t remaining = count;

    while (remaining > 0) {
        uint16_t chunk = (remaining > 16) ? 16 : remaining;
        uint32_t chunk_bytes = (uint32_t)chunk * block_size;
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

        int res = usb_msc_send_cbw_lun(dev, lun, tag, chunk_bytes, 0x00, 10, cdb);
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

int usb_msc_write_sectors(usb_device_t *dev, uint32_t lba, uint16_t count, const void *buf) {
    return usb_msc_write_sectors_lun(dev, 0, lba, count, 512, buf);
}

int usb_msc_read_capacity_lun(usb_device_t *dev, uint8_t lun, uint32_t *out_last_lba, uint32_t *out_block_size) {
    if (!dev || !dev->has_msc) return -1;

    uint32_t tag = ++msc_tag_counter;
    uint8_t cdb[10] = { SCSI_READ_CAPACITY_10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    int res = usb_msc_send_cbw_lun(dev, lun, tag, 8, 0x80, 10, cdb);
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

int usb_msc_read_capacity(usb_device_t *dev, uint32_t *out_last_lba, uint32_t *out_block_size) {
    return usb_msc_read_capacity_lun(dev, 0, out_last_lba, out_block_size);
}

typedef struct msc_source_priv {
    usb_device_t *dev;
    uint8_t       lun;
    uint32_t      block_size;
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
    uint32_t bsz = priv->block_size ? priv->block_size : 512;

    while (bytes_read < size) {
        uint64_t cur_pos = priv->offset + bytes_read;
        uint32_t lba = (uint32_t)(cur_pos / bsz);
        uint32_t sec_offset = (uint32_t)(cur_pos % bsz);

        if (sec_offset == 0 && (size - bytes_read) >= bsz) {
            uint32_t sectors_to_read = (size - bytes_read) / bsz;
            if (sectors_to_read > 128) sectors_to_read = 128; // Max 64 KB transfer chunk
            int res = usb_msc_read_sectors_lun(priv->dev, priv->lun, lba, (uint16_t)sectors_to_read, bsz, dst + bytes_read);
            if (res != 0) {
                log_error("MSC", "Read failed at LBA %u (error %d, read %u / %u bytes)", lba, res, bytes_read, size);
                break;
            }
            bytes_read += sectors_to_read * bsz;
        } else {
            static uint8_t bounce[4096];
            int res = usb_msc_read_sectors_lun(priv->dev, priv->lun, lba, 1, bsz, bounce);
            if (res != 0) {
                log_error("MSC", "Read bounce failed at LBA %u (error %d, read %u / %u bytes)", lba, res, bytes_read, size);
                break;
            }
            uint32_t chunk = bsz - sec_offset;
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

boot_source_t *boot_source_msc_create_lun(usb_device_t *dev, uint8_t lun) {
    if (!dev || !dev->has_msc || dev->is_disconnected) return NULL;

    uint32_t last_lba = 0;
    uint32_t block_size = 512;
    int cap_res = usb_msc_read_capacity_lun(dev, lun, &last_lba, &block_size);
    if (cap_res != 0 || last_lba == 0 || block_size == 0) {
        log_info("MSC", "LUN %u has no active media or capacity (code %d).", lun, cap_res);
        return NULL;
    }

    msc_source_priv_t *priv = (msc_source_priv_t *)kmalloc(sizeof(msc_source_priv_t));
    if (!priv) return NULL;

    priv->dev = dev;
    priv->lun = lun;
    priv->offset = 0;
    priv->block_size = block_size;
    priv->total_size = (uint64_t)(last_lba + 1) * block_size;

    log_info("MSC", "USB MSC LUN %u Capacity: %u MB (%u sectors of %u B)",
             lun, (uint32_t)(priv->total_size >> 20), last_lba + 1, block_size);

    boot_source_t *src = (boot_source_t *)kmalloc(sizeof(boot_source_t));
    if (!src) {
        kfree(priv);
        return NULL;
    }

    src->name = (lun == 0) ? "USB-MSC-LUN0" : "USB-MSC-LUN1";
    src->open = msc_src_open;
    src->read = msc_src_read;
    src->seek = msc_src_seek;
    src->tell = msc_src_tell;
    src->size = msc_src_size;
    src->close = msc_src_close;
    src->priv = priv;

    return src;
}

boot_source_t *boot_source_msc_create(usb_device_t *dev) {
    return boot_source_msc_create_lun(dev, 0);
}
