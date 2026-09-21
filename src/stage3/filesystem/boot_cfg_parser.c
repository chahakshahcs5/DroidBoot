#include "boot_cfg_parser.h"
#include "iso_reader.h"
#include "../core/printf.h"
#include "../memory/memory.h"

static inline bool is_space(char c) {
    return (c == ' ' || c == '\t' || c == '\r' || c == '\n');
}

static inline int to_lower(int c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static bool str_starts_with_nocase(const char *str, const char *prefix) {
    while (*prefix) {
        if (to_lower((unsigned char)*str) != to_lower((unsigned char)*prefix)) return false;
        str++; prefix++;
    }
    return true;
}

static void copy_string(char *dst, const char *src, uint32_t max_len) {
    if (!dst || max_len == 0) return;
    uint32_t i = 0;
    if (src) {
        while (src[i] && i < max_len - 1) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

/* Parse a single quoted string e.g. "Ubuntu 24.04" or 'Alpine Linux' */
static void extract_quoted_text(const char *src, char *out, uint32_t max_len) {
    out[0] = '\0';
    while (*src && *src != '"' && *src != '\'') src++;
    if (!*src) return;
    char quote = *src++;
    uint32_t idx = 0;
    while (*src && *src != quote && idx + 1 < max_len) {
        out[idx++] = *src++;
    }
    out[idx] = '\0';
}

/* Substitute template variables in kernel command line */
static void substitute_cmdline_vars(char *cmdline, uint32_t max_len, const char *iso_filename) {
    if (!cmdline || !iso_filename) return;

    char buf[384];
    uint32_t b_idx = 0;
    const char *p = cmdline;

    while (*p && b_idx + 1 < sizeof(buf)) {
        if (str_starts_with_nocase(p, "${iso_path}")) {
            p += 11;
            if (iso_filename[0] != '/') buf[b_idx++] = '/';
            for (int i = 0; iso_filename[i] && b_idx + 1 < sizeof(buf); i++) {
                buf[b_idx++] = iso_filename[i];
            }
        } else if (str_starts_with_nocase(p, "@ISO_NAME@")) {
            p += 10;
            for (int i = 0; iso_filename[i] && b_idx + 1 < sizeof(buf); i++) {
                buf[b_idx++] = iso_filename[i];
            }
        } else {
            buf[b_idx++] = *p++;
        }
    }
    buf[b_idx] = '\0';
    copy_string(cmdline, buf, max_len);
}

int boot_cfg_parse(const char *cfg_data, uint32_t len, const char *iso_filename, parsed_boot_config_t *out_cfg) {
    if (!cfg_data || len == 0 || !out_cfg) return -1;

    out_cfg->count = 0;
    out_cfg->default_index = 0;

    parsed_boot_entry_t *cur = NULL;
    uint32_t pos = 0;

    while (pos < len && out_cfg->count < MAX_PARSED_ENTRIES) {
        // Read line
        char line[256];
        uint32_t l_len = 0;
        while (pos < len && cfg_data[pos] != '\n' && l_len + 1 < sizeof(line)) {
            line[l_len++] = cfg_data[pos++];
        }
        if (pos < len && cfg_data[pos] == '\n') pos++;
        line[l_len] = '\0';

        // Trim trailing \r, \n, and whitespace
        while (l_len > 0 && (line[l_len - 1] == '\r' || line[l_len - 1] == '\n' || line[l_len - 1] == ' ' || line[l_len - 1] == '\t')) {
            line[--l_len] = '\0';
        }

        // Trim leading spaces
        const char *p = line;
        while (*p && is_space(*p)) p++;
        if (*p == '\0' || *p == '#') continue; // Blank or comment

        // 1. GRUB: menuentry "..."
        if (str_starts_with_nocase(p, "menuentry")) {
            if (cur && cur->kernel_path[0]) {
                cur->valid = true;
                out_cfg->count++;
                if (out_cfg->count >= MAX_PARSED_ENTRIES) break;
            }
            cur = &out_cfg->entries[out_cfg->count];
            cur->title[0] = '\0';
            cur->kernel_path[0] = '\0';
            cur->initrd_path[0] = '\0';
            cur->cmdline[0] = '\0';
            cur->valid = false;

            extract_quoted_text(p, cur->title, sizeof(cur->title));
            if (cur->title[0] == '\0') {
                copy_string(cur->title, "Linux Live System", sizeof(cur->title));
            }
            continue;
        }

        // 2. Syslinux: LABEL <name> or MENU LABEL <name>
        if (str_starts_with_nocase(p, "menu label") && cur) {
            p += 10;
            while (*p && is_space(*p)) p++;
            copy_string(cur->title, p, sizeof(cur->title));
            continue;
        }
        if (str_starts_with_nocase(p, "label ")) {
            if (cur && cur->kernel_path[0]) {
                cur->valid = true;
                out_cfg->count++;
                if (out_cfg->count >= MAX_PARSED_ENTRIES) break;
            }
            cur = &out_cfg->entries[out_cfg->count];
            cur->title[0] = '\0';
            cur->kernel_path[0] = '\0';
            cur->initrd_path[0] = '\0';
            cur->cmdline[0] = '\0';
            cur->valid = false;

            p += 6;
            while (*p && is_space(*p)) p++;
            copy_string(cur->title, p, sizeof(cur->title));
            continue;
        }

        // 3. Kernel path: linux / linux16 / linuxefi / kernel
        if (cur) {
            const char *k_tag = NULL;
            if (str_starts_with_nocase(p, "linux ")) k_tag = p + 6;
            else if (str_starts_with_nocase(p, "linux16 ")) k_tag = p + 8;
            else if (str_starts_with_nocase(p, "linuxefi ")) k_tag = p + 9;
            else if (str_starts_with_nocase(p, "kernel ")) k_tag = p + 7;

            if (k_tag) {
                while (*k_tag && is_space(*k_tag)) k_tag++;
                int k_idx = 0;
                while (*k_tag && !is_space(*k_tag) && k_idx + 1 < (int)sizeof(cur->kernel_path)) {
                    cur->kernel_path[k_idx++] = *k_tag++;
                }
                cur->kernel_path[k_idx] = '\0';

                // Remainder is kernel arguments
                while (*k_tag && is_space(*k_tag)) k_tag++;
                if (*k_tag) {
                    copy_string(cur->cmdline, k_tag, sizeof(cur->cmdline));
                    substitute_cmdline_vars(cur->cmdline, sizeof(cur->cmdline), iso_filename);
                }
                continue;
            }

            // 4. Initrd path: initrd / initrd16 / initrdefi
            const char *i_tag = NULL;
            if (str_starts_with_nocase(p, "initrd ")) i_tag = p + 7;
            else if (str_starts_with_nocase(p, "initrd16 ")) i_tag = p + 9;
            else if (str_starts_with_nocase(p, "initrdefi ")) i_tag = p + 10;

            if (i_tag) {
                while (*i_tag && is_space(*i_tag)) i_tag++;
                int i_idx = 0;
                while (*i_tag && !is_space(*i_tag) && i_idx + 1 < (int)sizeof(cur->initrd_path)) {
                    cur->initrd_path[i_idx++] = *i_tag++;
                }
                cur->initrd_path[i_idx] = '\0';
                continue;
            }

            // 5. Syslinux APPEND <args>
            if (str_starts_with_nocase(p, "append ")) {
                p += 7;
                while (*p && is_space(*p)) p++;

                // Check if initrd= is inside append
                const char *initrd_sub = NULL;
                for (int i = 0; p[i]; i++) {
                    if (str_starts_with_nocase(&p[i], "initrd=")) {
                        initrd_sub = &p[i] + 7;
                        break;
                    }
                }
                if (initrd_sub && cur->initrd_path[0] == '\0') {
                    int idx = 0;
                    while (*initrd_sub && !is_space(*initrd_sub) && idx + 1 < (int)sizeof(cur->initrd_path)) {
                        cur->initrd_path[idx++] = *initrd_sub++;
                    }
                    cur->initrd_path[idx] = '\0';
                }

                copy_string(cur->cmdline, p, sizeof(cur->cmdline));
                substitute_cmdline_vars(cur->cmdline, sizeof(cur->cmdline), iso_filename);
                continue;
            }
        }
    }

    if (cur && cur->kernel_path[0]) {
        cur->valid = true;
        log_debug("BOOTCFG", "  [%u] Title: '%s' | Kernel: '%s' | Initrd: '%s'",
                  out_cfg->count, cur->title, cur->kernel_path, cur->initrd_path);
        out_cfg->count++;
    }

    return (out_cfg->count > 0) ? 0 : -1;
}

int boot_cfg_find_and_parse(boot_source_t *iso_src, const char *iso_filename, parsed_boot_config_t *out_cfg) {
    if (!iso_src || !out_cfg) return -1;

    const char *candidate_configs[] = {
        "/boot/grub/loopback.cfg",
        "/boot/grub/grub.cfg",
        "/EFI/BOOT/grub.cfg",
        "/isolinux/isolinux.cfg",
        "/syslinux/syslinux.cfg",
        "/isolinux.cfg",
        "/boot/syslinux/syslinux.cfg"
    };

    for (int i = 0; i < 7; i++) {
        uint32_t cfg_lba = 0, cfg_size = 0;
        if (iso_find_file(iso_src, candidate_configs[i], &cfg_lba, &cfg_size) == 0 && cfg_size > 0) {
            log_info("BOOTCFG", "Discovered boot config at '%s' (%u bytes)", candidate_configs[i], cfg_size);

            uint32_t read_len = cfg_size > 8192 ? 8192 : cfg_size;
            char *buf = (char *)kmalloc(read_len + 1);
            if (!buf) continue;

            if (iso_src->seek(iso_src, (uint64_t)cfg_lba * 2048) != 0) {
                kfree(buf);
                continue;
            }

            uint32_t nread = iso_src->read(iso_src, buf, read_len);
            if (nread > 0) {
                buf[nread] = '\0';
                copy_string(out_cfg->config_source, candidate_configs[i], sizeof(out_cfg->config_source));
                int res = boot_cfg_parse(buf, nread, iso_filename, out_cfg);
                kfree(buf);
                if (res == 0 && out_cfg->count > 0) {
                    log_info("BOOTCFG", "Successfully extracted %u boot entries from '%s'",
                             out_cfg->count, candidate_configs[i]);
                    return 0;
                }
            } else {
                kfree(buf);
            }
        }
    }

    return -1; // No valid boot configuration found
}
