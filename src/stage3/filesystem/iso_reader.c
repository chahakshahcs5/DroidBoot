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

static void search_directory_for_boot(boot_source_t *iso_src, uint32_t dir_lba, uint32_t dir_size,
                                      iso_boot_files_t *out_files, uint8_t *sector_buf) {
    if (dir_lba == 0 || dir_size == 0) return;

    uint32_t num_sectors = (dir_size + 2047) / 2048;
    if (num_sectors > 32) num_sectors = 32; // Limit directory search to 64KB

    for (uint32_t s = 0; s < num_sectors; s++) {
        uint64_t offset = (uint64_t)(dir_lba + s) * 2048;
        if (iso_src->seek(iso_src, offset) != 0) break;
        if (iso_src->read(iso_src, sector_buf, 2048) != 2048) break;

        uint32_t pos = 0;
        while (pos < 2048) {
            uint8_t rlen = sector_buf[pos];
            if (rlen == 0) break; // End of entries in this sector

            uint8_t nlen = sector_buf[pos + 32];
            const char *name = (const char *)&sector_buf[pos + 33];
            uint32_t lba = *(uint32_t *)(&sector_buf[pos + 2]);
            uint32_t size = *(uint32_t *)(&sector_buf[pos + 10]);

            if (!out_files->found_kernel && nlen >= 7 &&
                (prefix_match(name, "VMLINUZ") || prefix_match(name, "BZIMAGE"))) {
                out_files->kernel_lba = lba;
                out_files->kernel_size = size;
                out_files->found_kernel = true;
                log_info("ISO", "  Found Kernel    : LBA %u, Size %u bytes (%u MB)",
                         lba, size, size / 1024 / 1024);
            } else if (!out_files->found_initrd && nlen >= 4 &&
                       (prefix_match(name, "INITRAMFS") || prefix_match(name, "INITRD") ||
                        prefix_match(name, "CORE") || prefix_match(name, "ROOTFS"))) {
                out_files->initrd_lba = lba;
                out_files->initrd_size = size;
                out_files->found_initrd = true;
                log_info("ISO", "  Found Initramfs : LBA %u, Size %u bytes (%u MB)",
                         lba, size, size / 1024 / 1024);
            }

            pos += rlen;
        }

        if (out_files->found_kernel && out_files->found_initrd) {
            break;
        }
    }
}

int iso_find_boot_files(boot_source_t *iso_src, iso_boot_files_t *out_files) {
    if (!iso_src || !out_files) return -1;

    out_files->found_kernel = false;
    out_files->found_initrd = false;
    out_files->is_casper = false;
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

    // 2. Scan Root Directory to discover /casper, /boot, /live folders
    uint32_t boot_lba = 0, boot_size = 0;
    uint32_t casper_lba = 0, casper_size = 0;
    uint32_t live_lba = 0, live_size = 0;

    uint32_t root_sectors = (root_size + 2047) / 2048;
    if (root_sectors > 16) root_sectors = 16;

    for (uint32_t s = 0; s < root_sectors; s++) {
        uint64_t offset = (uint64_t)(root_lba + s) * 2048;
        if (iso_src->seek(iso_src, offset) != 0) break;
        if (iso_src->read(iso_src, sector, 2048) != 2048) break;

        uint32_t pos = 0;
        while (pos < 2048) {
            uint8_t rlen = sector[pos];
            if (rlen == 0) break;
            uint8_t flags = sector[pos + 25];
            uint8_t nlen = sector[pos + 32];
            const char *name = (const char *)&sector[pos + 33];
            uint32_t lba = *(uint32_t *)(&sector[pos + 2]);
            uint32_t size = *(uint32_t *)(&sector[pos + 10]);

            if (flags & 0x02) { // Subdirectory
                if (nlen >= 6 && prefix_match(name, "CASPER")) {
                    casper_lba = lba;
                    casper_size = size;
                    log_info("ISO", "Found /casper directory at LBA %u (size %u B)", lba, size);
                } else if (nlen >= 4 && prefix_match(name, "BOOT")) {
                    boot_lba = lba;
                    boot_size = size;
                    log_info("ISO", "Found /boot directory at LBA %u (size %u B)", lba, size);
                } else if (nlen >= 4 && prefix_match(name, "LIVE")) {
                    live_lba = lba;
                    live_size = size;
                    log_info("ISO", "Found /live directory at LBA %u (size %u B)", lba, size);
                }
            }
            pos += rlen;
        }
    }

    // 3. Search candidate directories: /casper (Ubuntu), /boot (Alpine/Arch), /live (Debian), or root
    if (casper_lba != 0) {
        log_info("ISO", "Searching /casper directory...");
        search_directory_for_boot(iso_src, casper_lba, casper_size, out_files, sector);
        if (out_files->found_kernel) {
            out_files->is_casper = true;
            log_info("ISO", "Detected Ubuntu / Casper live image!");
        }
    }

    if (!out_files->found_kernel && boot_lba != 0) {
        log_info("ISO", "Searching /boot directory...");
        search_directory_for_boot(iso_src, boot_lba, boot_size, out_files, sector);
    }

    if (!out_files->found_kernel && live_lba != 0) {
        log_info("ISO", "Searching /live directory...");
        search_directory_for_boot(iso_src, live_lba, live_size, out_files, sector);
    }

    if (!out_files->found_kernel) {
        log_info("ISO", "Searching root directory...");
        search_directory_for_boot(iso_src, root_lba, root_size, out_files, sector);
    }

    return out_files->found_kernel ? 0 : -9;
}
