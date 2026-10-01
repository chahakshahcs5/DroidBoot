#include "iso_reader.h"
#include "boot_cfg_parser.h"
#include "../core/printf.h"
#include "../memory/memory.h"

static inline int to_lower(int c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static void k_memset(void *dst, int val, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)val;
}

static int k_memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) return (int)p1[i] - (int)p2[i];
    }
    return 0;
}

static bool iso_parse_el_torito(boot_source_t *iso_src, uint32_t *out_boot_lba, uint32_t *out_boot_size) {
    if (!iso_src || !out_boot_lba || !out_boot_size) return false;

    // Sector 17 (0x8800) is the Boot Record Volume Descriptor (BRVD)
    uint8_t sector[2048];
    if (iso_src->seek(iso_src, 17 * 2048) != 0) return false;
    if (iso_src->read(iso_src, sector, 2048) != 2048) return false;

    // Check type 0, "CD001", and "EL TORITO SPECIFICATION"
    if (sector[0] != 0 || sector[1] != 'C' || sector[2] != 'D' || sector[3] != '0' ||
        sector[4] != '0' || sector[5] != '1') {
        return false;
    }
    if (k_memcmp(&sector[7], "EL TORITO SPECIFICATION", 23) != 0) {
        return false;
    }

    uint32_t cat_sector = *(uint32_t *)&sector[71];
    if (cat_sector == 0) return false;

    // Read the Boot Catalog
    if (iso_src->seek(iso_src, (uint64_t)cat_sector * 2048) != 0) return false;
    if (iso_src->read(iso_src, sector, 2048) != 2048) return false;

    // Validation entry must have Header ID 0x01 and key 0x55, 0xAA
    if (sector[0] != 0x01 || sector[30] != 0x55 || sector[31] != 0xAA) {
        return false;
    }

    // Initial / Default Entry at offset 32
    uint8_t bootable = sector[32];
    uint16_t sector_count = *(uint16_t *)&sector[38];
    uint32_t load_rba = *(uint32_t *)&sector[40];

    if (bootable == 0x88 && load_rba > 0) {
        *out_boot_lba = load_rba;
        uint32_t byte_count = (sector_count > 0) ? ((uint32_t)sector_count * 512) : 2048;
        if (byte_count < 512) byte_count = 512;
        if (byte_count > 8192) byte_count = 8192;
        *out_boot_size = byte_count;
        return true;
    }

    return false;
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

static bool str_contains_nocase(const char *str, const char *sub) {
    if (!str || !sub) return false;
    if (!*sub) return true;
    for (int i = 0; str[i]; i++) {
        int j = 0;
        while (str[i + j] && sub[j]) {
            if (to_lower((unsigned char)str[i + j]) != to_lower((unsigned char)sub[j])) break;
            j++;
        }
        if (!sub[j]) return true;
    }
    return false;
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

    k_memset(out_files, 0, sizeof(iso_boot_files_t));
    out_files->boot_sector_size = 512;

    // Step 0: Read Sector 16 (0x8000) - Primary Volume Descriptor to grab Volume ID
    uint8_t pvd_sector[2048];
    if (iso_src->seek(iso_src, 0x8000) == 0 && iso_src->read(iso_src, pvd_sector, 2048) == 2048) {
        if (pvd_sector[1] == 'C' && pvd_sector[2] == 'D' && pvd_sector[3] == '0' && pvd_sector[4] == '0' && pvd_sector[5] == '1') {
            int vlen = 32;
            while (vlen > 0 && (pvd_sector[40 + vlen - 1] == ' ' || pvd_sector[40 + vlen - 1] == '\0')) vlen--;
            for (int i = 0; i < vlen; i++) out_files->volume_id[i] = (char)pvd_sector[40 + i];
            out_files->volume_id[vlen] = '\0';
            log_info("ISO", "Volume ID: '%s'", out_files->volume_id);
        }
    }

    // Step 1: Attempt to dynamically discover and parse bootloader config from ISO
    static parsed_boot_config_t cfg;
    k_memset(&cfg, 0, sizeof(cfg));
    if (boot_cfg_find_and_parse(iso_src, iso_src->name, &cfg) == 0 && cfg.count > 0) {
        bool is_kali = str_contains_nocase(out_files->volume_id, "kali");
        parsed_boot_entry_t *entry = &cfg.entries[cfg.default_index];
        parsed_boot_entry_t *alt_entry = NULL;

        if (is_kali) {
            out_files->is_kali = true;
            parsed_boot_entry_t *live_entry = NULL;
            parsed_boot_entry_t *text_entry = NULL;
            parsed_boot_entry_t *gui_entry = NULL;

            for (uint32_t e = 0; e < cfg.count; e++) {
                parsed_boot_entry_t *cand = &cfg.entries[e];
                if (!cand->valid) continue;
                if (str_contains_nocase(cand->title, "live") && !str_contains_nocase(cand->title, "forensic") && !str_contains_nocase(cand->title, "failsafe")) {
                    if (!live_entry) live_entry = cand;
                } else if (str_contains_nocase(cand->title, "graphical")) {
                    if (!gui_entry) gui_entry = cand;
                } else if (str_contains_nocase(cand->title, "install")) {
                    if (!text_entry) text_entry = cand;
                }
            }

            // Prefer Live entry (for persistence support). Fall back to installer entries only if no Live entry exists.
            if (live_entry) {
                entry = live_entry;
            } else if (text_entry) {
                entry = text_entry;
                alt_entry = gui_entry;
            } else if (gui_entry) {
                entry = gui_entry;
            }
        }

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

        if (alt_entry && alt_entry->initrd_path[0]) {
            uint32_t ailba = 0, aisz = 0;
            if (iso_find_file(iso_src, alt_entry->initrd_path, &ailba, &aisz) == 0 && aisz > 0) {
                out_files->alt_initrd_lba = ailba;
                out_files->alt_initrd_size = aisz;
                out_files->has_alt_initrd = true;
                log_info("ISO", "  Resolved Alt Initrd (Graphical): '%s' -> LBA %u (%u MB)",
                         alt_entry->initrd_path, ailba, aisz / 1024 / 1024);
            }
        }

        if (out_files->found_kernel) {
            bool is_kali = str_contains_nocase(out_files->volume_id, "kali");
            if (is_kali && str_contains_nocase(entry->title, "live")) {
                const char *ktitle = "Kali Linux Live";
                for (int i = 0; ktitle[i] && i < 95; i++) out_files->title[i] = ktitle[i];
                out_files->title[95] = '\0';
            } else if (is_kali && str_contains_nocase(entry->title, "graphical")) {
                const char *ktitle = "Kali Linux (Graphical Install)";
                for (int i = 0; ktitle[i] && i < 95; i++) out_files->title[i] = ktitle[i];
                out_files->title[95] = '\0';
            } else if (is_kali && str_contains_nocase(entry->title, "install")) {
                const char *ktitle = "Kali Linux (Install)";
                for (int i = 0; ktitle[i] && i < 95; i++) out_files->title[i] = ktitle[i];
                out_files->title[95] = '\0';
            } else {
                int t_idx = 0;
                while (entry->title[t_idx] && t_idx < 95) {
                    out_files->title[t_idx] = entry->title[t_idx];
                    t_idx++;
                }
                out_files->title[t_idx] = '\0';
            }
            int c_idx = 0;
            while (entry->cmdline[c_idx] && c_idx < 383) {
                out_files->cmdline[c_idx] = entry->cmdline[c_idx];
                c_idx++;
            }
            out_files->cmdline[c_idx] = '\0';
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
        "/install.amd",
        "/live",
        "/arch/boot/x86_64",
        "/isolinux",
        "/images/pxeboot",
        ""
    };

    for (int i = 0; i < 8 && !out_files->found_kernel; i++) {
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

    if (out_files->found_kernel && out_files->title[0] == '\0') {
        if (str_contains_nocase(out_files->volume_id, "kali")) {
            const char *ktitle = "Kali Linux Installer";
            for (int i = 0; ktitle[i] && i < 95; i++) out_files->title[i] = ktitle[i];
            out_files->title[95] = '\0';
        }
    }

    if (!out_files->found_kernel) {
        // Check for Windows boot files / volume signatures
        uint32_t bm_lba = 0, bm_sz = 0;
        bool has_bootmgr = (iso_find_file(iso_src, "/bootmgr", &bm_lba, &bm_sz) == 0 && bm_sz > 0);
        if (!has_bootmgr) {
            has_bootmgr = (iso_find_file(iso_src, "/boot/bcd", &bm_lba, &bm_sz) == 0);
        }
        if (!has_bootmgr) {
            has_bootmgr = (iso_find_file(iso_src, "/sources/boot.wim", &bm_lba, &bm_sz) == 0);
        }
        bool has_win_vol = (str_contains_nocase(out_files->volume_id, "CCCOMA") ||
                            str_contains_nocase(out_files->volume_id, "ESD-ISO") ||
                            str_contains_nocase(out_files->volume_id, "WIN") ||
                            str_contains_nocase(out_files->volume_id, "WINDOWS"));

        if (has_bootmgr || has_win_vol) {
            out_files->is_windows = true;
            out_files->bootmgr_lba = bm_lba;
            out_files->bootmgr_size = bm_sz;
            out_files->boot_sector_lba = 0;
            out_files->boot_sector_size = 512;

            uint32_t el_lba = 0, el_size = 0;
            if (iso_parse_el_torito(iso_src, &el_lba, &el_size)) {
                out_files->boot_sector_lba = el_lba;
                out_files->boot_sector_size = el_size;
                log_info("ISO", "  El Torito Boot Image discovered: LBA %u (%u bytes)", el_lba, el_size);
            }

            const char *wtitle = "Windows 10/11 Installer";
            if (str_contains_nocase(out_files->volume_id, "HBCD") || str_contains_nocase(iso_src->name, "HBCD")) {
                wtitle = "Hiren's BootCD PE (Windows 11 Live)";
            } else if (str_contains_nocase(out_files->volume_id, "STRELEC") || str_contains_nocase(iso_src->name, "STRELEC")) {
                wtitle = "Sergei Strelec (Windows Live)";
            } else if (str_contains_nocase(out_files->volume_id, "WINPE") || str_contains_nocase(iso_src->name, "WINPE")) {
                wtitle = "Windows Live WinPE";
            }
            for (int i = 0; wtitle[i] && i < 95; i++) out_files->title[i] = wtitle[i];
            out_files->title[95] = '\0';
            log_info("ISO", "  Windows media verified: '%s' (Volume: '%s', boot LBA %u)",
                     wtitle, out_files->volume_id, out_files->boot_sector_lba);
            return 0;
        }
    }

    return (out_files->found_kernel || out_files->is_windows) ? 0 : -9;
}
