#include "iso_reader.h"
#include "boot_cfg_parser.h"
#include "../core/printf.h"
#include "../memory/memory.h"

static inline int to_lower(int c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static bool str_eq_nocase_len(const char *a, const char *b, uint32_t b_len) {
    if (!a || !b) return false;
    uint32_t i = 0;
    while (a[i] && i < b_len) {
        if (to_lower((unsigned char)a[i]) != to_lower((unsigned char)b[i])) return false;
        i++;
    }
    return (a[i] == '\0' && i == b_len);
}

static bool str_prefix_nocase(const char *str, const char *prefix) {
    if (!str || !prefix) return false;
    while (*prefix) {
        if (to_lower((unsigned char)*str) != to_lower((unsigned char)*prefix)) return false;
        str++; prefix++;
    }
    return true;
}

/* Extract real name from directory record (Rock Ridge NM or standard 8.3/ISO name) */
static void extract_record_name(const uint8_t *rec, char *out_name, uint32_t max_out) {
    uint8_t rlen = rec[0];
    uint8_t nlen = rec[32];
    const char *name_src = (const char *)&rec[33];

    out_name[0] = '\0';
    if (nlen == 0 || max_out == 0) return;

    // Check for Rock Ridge SUSP records after directory name
    uint32_t susp_offset = 33 + nlen + ((nlen % 2 == 0) ? 1 : 0);
    while (susp_offset + 4 <= rlen) {
        const uint8_t *susp = &rec[susp_offset];
        uint8_t entry_len = susp[2];
        if (entry_len < 4 || susp_offset + entry_len > rlen) break;

        // Rock Ridge NM (Alternate Name)
        if (susp[0] == 'N' && susp[1] == 'M') {
            uint8_t flags = susp[4];
            (void)flags;
            uint32_t nm_len = entry_len - 5;
            const char *nm_str = (const char *)&susp[5];
            uint32_t copy_len = nm_len < max_out - 1 ? nm_len : max_out - 1;
            for (uint32_t i = 0; i < copy_len; i++) out_name[i] = nm_str[i];
            out_name[copy_len] = '\0';
            return;
        }
        susp_offset += entry_len;
    }

    // Standard ISO name: trim trailing ";1" and "."
    uint32_t eff_len = nlen;
    for (uint32_t i = 0; i < nlen; i++) {
        if (name_src[i] == ';') {
            eff_len = i;
            break;
        }
    }
    if (eff_len > 0 && name_src[eff_len - 1] == '.') eff_len--;

    uint32_t copy_len = eff_len < max_out - 1 ? eff_len : max_out - 1;
    for (uint32_t i = 0; i < copy_len; i++) out_name[i] = name_src[i];
    out_name[copy_len] = '\0';
}

/* Locate any file in ISO image by path */
int iso_find_file(boot_source_t *iso_src, const char *path, uint32_t *out_lba, uint32_t *out_size) {
    if (!iso_src || !path) return -1;

    uint8_t sector[2048];

    // Read Sector 16 (0x8000) - Primary Volume Descriptor
    if (iso_src->seek(iso_src, 0x8000) != 0) return -2;
    if (iso_src->read(iso_src, sector, 2048) != 2048) return -3;

    if (sector[1] != 'C' || sector[2] != 'D' || sector[3] != '0' || sector[4] != '0' || sector[5] != '1') {
        return -4; // Not valid ISO9660
    }

    // Root directory record at offset 156
    uint32_t cur_dir_lba = *(uint32_t *)(&sector[156 + 2]);
    uint32_t cur_dir_size = *(uint32_t *)(&sector[156 + 10]);

    const char *p = path;
    while (*p == '/' || *p == '\\') p++;
    if (*p == '\0') {
        if (out_lba) *out_lba = cur_dir_lba;
        if (out_size) *out_size = cur_dir_size;
        return 0;
    }

    char component[64];
    while (*p) {
        int clen = 0;
        while (*p && *p != '/' && *p != '\\' && clen < 63) {
            component[clen++] = *p++;
        }
        component[clen] = '\0';
        while (*p == '/' || *p == '\\') p++;
        bool is_last = (*p == '\0');

        bool comp_found = false;
        uint32_t num_sectors = (cur_dir_size + 2047) / 2048;
        if (num_sectors > 64) num_sectors = 64; // Max 128 KB per directory scan

        for (uint32_t s = 0; s < num_sectors && !comp_found; s++) {
            uint64_t offset = (uint64_t)(cur_dir_lba + s) * 2048;
            if (iso_src->seek(iso_src, offset) != 0) break;
            if (iso_src->read(iso_src, sector, 2048) != 2048) break;

            uint32_t pos = 0;
            while (pos < 2048) {
                uint8_t rlen = sector[pos];
                if (rlen == 0) break;

                uint8_t flags = sector[pos + 25];
                uint32_t lba = *(uint32_t *)(&sector[pos + 2]);
                uint32_t size = *(uint32_t *)(&sector[pos + 10]);

                char entry_name[128];
                extract_record_name(&sector[pos], entry_name, sizeof(entry_name));

                if (str_eq_nocase_len(component, entry_name, clen)) {
                    if (is_last) {
                        if (out_lba) *out_lba = lba;
                        if (out_size) *out_size = size;
                        return 0; // Success!
                    } else if (flags & 0x02) { // Directory
                        cur_dir_lba = lba;
                        cur_dir_size = size;
                        comp_found = true;
                        break;
                    }
                }
                pos += rlen;
            }
        }

        if (!comp_found) return -5; // Component not found
    }

    return -6;
}

static void search_dir_heuristic(boot_source_t *iso_src, uint32_t dir_lba, uint32_t dir_size,
                                 iso_boot_files_t *out_files, uint8_t *sec_buf) {
    if (dir_lba == 0 || dir_size == 0) return;
    uint32_t num_sectors = (dir_size + 2047) / 2048;
    if (num_sectors > 32) num_sectors = 32;

    for (uint32_t s = 0; s < num_sectors; s++) {
        uint64_t offset = (uint64_t)(dir_lba + s) * 2048;
        if (iso_src->seek(iso_src, offset) != 0) break;
        if (iso_src->read(iso_src, sec_buf, 2048) != 2048) break;

        uint32_t pos = 0;
        while (pos < 2048) {
            uint8_t rlen = sec_buf[pos];
            if (rlen == 0) break;

            uint32_t lba = *(uint32_t *)(&sec_buf[pos + 2]);
            uint32_t size = *(uint32_t *)(&sec_buf[pos + 10]);

            char name[128];
            extract_record_name(&sec_buf[pos], name, sizeof(name));

            if (!out_files->found_kernel &&
                (str_prefix_nocase(name, "vmlinuz") || str_prefix_nocase(name, "bzimage"))) {
                out_files->kernel_lba = lba;
                out_files->kernel_size = size;
                out_files->found_kernel = true;
                log_info("ISO", "  Found Kernel    : '%s' (LBA %u, %u MB)", name, lba, size / 1024 / 1024);
            } else if (!out_files->found_initrd &&
                       (str_prefix_nocase(name, "initramfs") || str_prefix_nocase(name, "initrd") ||
                        str_prefix_nocase(name, "core") || str_prefix_nocase(name, "rootfs"))) {
                out_files->initrd_lba = lba;
                out_files->initrd_size = size;
                out_files->found_initrd = true;
                log_info("ISO", "  Found Initramfs : '%s' (LBA %u, %u MB)", name, lba, size / 1024 / 1024);
            }

            pos += rlen;
        }

        if (out_files->found_kernel && out_files->found_initrd) break;
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
    out_files->title[0] = '\0';
    out_files->cmdline[0] = '\0';

    // Step 1: Attempt to dynamically discover and parse bootloader config from ISO
    parsed_boot_config_t cfg;
    if (boot_cfg_find_and_parse(iso_src, iso_src->name, &cfg) == 0 && cfg.count > 0) {
        parsed_boot_entry_t *entry = &cfg.entries[cfg.default_index];
        log_info("ISO", "Applying boot configuration from '%s': '%s'", cfg.config_source, entry->title);

        uint32_t klba = 0, ksz = 0;
        if (iso_find_file(iso_src, entry->kernel_path, &klba, &ksz) == 0 && ksz > 0) {
            out_files->kernel_lba = klba;
            out_files->kernel_size = ksz;
            out_files->found_kernel = true;
            log_info("ISO", "  Resolved Kernel : '%s' -> LBA %u (%u MB)", entry->kernel_path, klba, ksz / 1024 / 1024);
        }

        uint32_t ilba = 0, isz = 0;
        if (entry->initrd_path[0] && iso_find_file(iso_src, entry->initrd_path, &ilba, &isz) == 0 && isz > 0) {
            out_files->initrd_lba = ilba;
            out_files->initrd_size = isz;
            out_files->found_initrd = true;
            log_info("ISO", "  Resolved Initrd : '%s' -> LBA %u (%u MB)", entry->initrd_path, ilba, isz / 1024 / 1024);
        }

        if (out_files->found_kernel) {
            for (int i = 0; entry->title[i] && i < 95; i++) out_files->title[i] = entry->title[i];
            out_files->title[95] = '\0';
            for (int i = 0; entry->cmdline[i] && i < 383; i++) out_files->cmdline[i] = entry->cmdline[i];
            out_files->cmdline[383] = '\0';
            if (str_prefix_nocase(entry->cmdline, "boot=casper") ||
                str_prefix_nocase(entry->kernel_path, "/casper/")) {
                out_files->is_casper = true;
            }
            return 0;
        }
    }

    // Step 2: Heuristic scanning fallback across /casper, /boot, /live, /arch/boot, /isolinux, and root
    static uint8_t sector[2048];
    if (iso_src->seek(iso_src, 0x8000) != 0) return -2;
    if (iso_src->read(iso_src, sector, 2048) != 2048) return -3;

    if (sector[1] != 'C' || sector[2] != 'D' || sector[3] != '0' || sector[4] != '0' || sector[5] != '1') {
        return -4;
    }

    uint32_t root_lba = *(uint32_t *)(&sector[156 + 2]);
    uint32_t root_size = *(uint32_t *)(&sector[156 + 10]);

    const char *search_dirs[] = {
        "/casper",
        "/boot",
        "/live",
        "/arch/boot/x86_64",
        "/isolinux",
        "/images/pxeboot",
        ""
    };

    for (int i = 0; i < 7 && !out_files->found_kernel; i++) {
        uint32_t dir_lba = root_lba;
        uint32_t dir_size = root_size;

        if (search_dirs[i][0] != '\0') {
            if (iso_find_file(iso_src, search_dirs[i], &dir_lba, &dir_size) != 0) {
                continue;
            }
            log_info("ISO", "Scanning directory '%s' for boot binaries...", search_dirs[i]);
            if (str_prefix_nocase(search_dirs[i], "/casper")) {
                out_files->is_casper = true;
            }
        } else {
            log_info("ISO", "Scanning root directory for boot binaries...");
        }

        search_dir_heuristic(iso_src, dir_lba, dir_size, out_files, sector);
    }

    return out_files->found_kernel ? 0 : -9;
}
