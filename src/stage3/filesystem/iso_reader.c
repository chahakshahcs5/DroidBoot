#include "iso_reader.h"
#include "../core/printf.h"

static bool prefix_match(const char *str, const char *prefix) {
    while (*prefix) {
        char cs = *str++;
        char cp = *prefix++;
        if (cs >= 'a' && cs <= 'z') cs -= 32;
        if (cp >= 'a' && cp <= 'z') cp -= 32;
        if (cs != cp) return false;
    }
    return true;
}

int iso_find_boot_files(boot_source_t *iso_src, iso_boot_files_t *out_files) {
    if (!iso_src || !out_files) return -1;

    out_files->found_kernel = false;
    out_files->found_initrd = false;
    out_files->kernel_lba = 0;
    out_files->kernel_size = 0;
    out_files->initrd_lba = 0;
    out_files->initrd_size = 0;

    static uint8_t sector[2048];

    // 1. Read Sector 16 (0x8000) - Primary Volume Descriptor
    if (iso_src->seek(iso_src, 0x8000) != 0) return -2;
    if (iso_src->read(iso_src, sector, 2048) != 2048) return -3;

    // Check ISO9660 PVD signature
    if (sector[0] != 0x01 || sector[1] != 'C' || sector[2] != 'D' ||
        sector[3] != '0' || sector[4] != '0' || sector[5] != '1') {
        log_error("ISO", "Not a valid ISO9660 image (PVD signature mismatch)");
        return -4;
    }

    // Root directory record is at offset 156
    uint32_t root_lba = *(uint32_t *)(&sector[156 + 2]);
    uint32_t root_size = *(uint32_t *)(&sector[156 + 10]);
    log_info("ISO", "ISO9660 Validated. Root Directory at LBA %u (size %u bytes)", root_lba, root_size);

    // 2. Read Root Directory to find /boot folder
    if (iso_src->seek(iso_src, (uint64_t)root_lba * 2048) != 0) return -5;
    if (iso_src->read(iso_src, sector, 2048) != 2048) return -6;

    uint32_t boot_lba = 0;
    uint32_t offset = 0;
    while (offset < 2048) {
        uint8_t rlen = sector[offset];
        if (rlen == 0) break;
        uint8_t flags = sector[offset + 25];
        uint8_t nlen = sector[offset + 32];
        const char *name = (const char *)&sector[offset + 33];
        uint32_t lba = *(uint32_t *)(&sector[offset + 2]);

        if (nlen >= 4 && prefix_match(name, "BOOT") && (flags & 0x02)) {
            boot_lba = lba;
            log_info("ISO", "Found /boot directory at LBA %u", boot_lba);
            break;
        }
        offset += rlen;
    }

    // If /boot folder not found, search root directory for kernels directly
    uint32_t search_lba = (boot_lba != 0) ? boot_lba : root_lba;

    // 3. Read directory entries to locate vmlinuz and initramfs
    if (iso_src->seek(iso_src, (uint64_t)search_lba * 2048) != 0) return -7;
    if (iso_src->read(iso_src, sector, 2048) != 2048) return -8;

    offset = 0;
    while (offset < 2048) {
        uint8_t rlen = sector[offset];
        if (rlen == 0) break;
        uint8_t nlen = sector[offset + 32];
        const char *name = (const char *)&sector[offset + 33];
        uint32_t lba = *(uint32_t *)(&sector[offset + 2]);
        uint32_t size = *(uint32_t *)(&sector[offset + 10]);

        if (nlen >= 7 && prefix_match(name, "VMLINUZ")) {
            out_files->kernel_lba = lba;
            out_files->kernel_size = size;
            out_files->found_kernel = true;
            log_info("ISO", "  Found Kernel    : LBA %u, Size %u bytes (%u MB)",
                     lba, size, size / 1024 / 1024);
        } else if (nlen >= 6 && (prefix_match(name, "INITRAMFS") || prefix_match(name, "INITRD"))) {
            out_files->initrd_lba = lba;
            out_files->initrd_size = size;
            out_files->found_initrd = true;
            log_info("ISO", "  Found Initramfs : LBA %u, Size %u bytes (%u MB)",
                     lba, size, size / 1024 / 1024);
        }
        offset += rlen;
    }

    return out_files->found_kernel ? 0 : -9;
}
