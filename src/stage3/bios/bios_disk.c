#include "bios_disk.h"

static void disk_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

int bios_disk_read(uint8_t drive, uint64_t lba, uint16_t count, void *dst) {
    if (!dst || count == 0) return 0;

    uint8_t *out = (uint8_t *)dst;
    while (count > 0) {
        uint16_t chunk = (count > BIOS_MAX_CHUNK_SECTORS) ? BIOS_MAX_CHUNK_SECTORS : count;
        bios_dap_t *dap = (bios_dap_t *)BIOS_DAP_ADDR;
        dap->size = sizeof(bios_dap_t);
        dap->reserved = 0;
        dap->sector_count = chunk;
        dap->buffer_offset = BIOS_BOUNCE_OFF;
        dap->buffer_segment = BIOS_BOUNCE_SEG;
        dap->lba = lba;

        int res = bios_int13_call(drive, BIOS_DAP_ADDR, BIOS_CMD_READ_EXT);
        if (res != 0) {
            return res;
        }

        disk_memcpy(out, (const void *)BIOS_BOUNCE_ADDR, chunk * BIOS_SECTOR_SIZE);

        out += (chunk * BIOS_SECTOR_SIZE);
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

int bios_disk_write(uint8_t drive, uint64_t lba, uint16_t count, const void *src) {
    if (!src || count == 0) return 0;

    const uint8_t *in = (const uint8_t *)src;
    while (count > 0) {
        uint16_t chunk = (count > BIOS_MAX_CHUNK_SECTORS) ? BIOS_MAX_CHUNK_SECTORS : count;

        disk_memcpy((void *)BIOS_BOUNCE_ADDR, in, chunk * BIOS_SECTOR_SIZE);

        bios_dap_t *dap = (bios_dap_t *)BIOS_DAP_ADDR;
        dap->size = sizeof(bios_dap_t);
        dap->reserved = 0;
        dap->sector_count = chunk;
        dap->buffer_offset = BIOS_BOUNCE_OFF;
        dap->buffer_segment = BIOS_BOUNCE_SEG;
        dap->lba = lba;

        int res = bios_int13_call(drive, BIOS_DAP_ADDR, BIOS_CMD_WRITE_EXT);
        if (res != 0) {
            return res;
        }

        in += (chunk * BIOS_SECTOR_SIZE);
        lba += chunk;
        count -= chunk;
    }
    return 0;
}
