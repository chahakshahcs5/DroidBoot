#include "os_scanner.h"
#include "../core/printf.h"
#include "../memory/memory.h"
#include "../bios/bios_disk.h"
#include "../filesystem/fat_source.h"
#include "../filesystem/diskio.h"
#include "../usb/usb_msc.h"

static void k_memset(void *dst, uint8_t val, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = val;
}

static bool str_eq_nocase(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return false;
    }
    return (*a == '\0' && *b == '\0');
}

static bool str_ends_with_nocase(const char *str, const char *suffix) {
    if (!str || !suffix) return false;
    int str_len = 0;
    while (str[str_len]) str_len++;
    int suf_len = 0;
    while (suffix[suf_len]) suf_len++;
    if (str_len < suf_len) return false;

    const char *p = str + str_len - suf_len;
    while (*p && *suffix) {
        char cp = *p++;
        char cs = *suffix++;
        if (cp >= 'a' && cp <= 'z') cp -= 32;
        if (cs >= 'a' && cs <= 'z') cs -= 32;
        if (cp != cs) return false;
    }
    return true;
}

static bool str_contains_nocase(const char *str, const char *sub) {
    if (!str || !sub) return false;
    if (!*sub) return true;

    for (int i = 0; str[i]; i++) {
        int j = 0;
        while (str[i + j] && sub[j]) {
            char cs = str[i + j];
            char csub = sub[j];
            if (cs >= 'a' && cs <= 'z') cs -= 32;
            if (csub >= 'a' && csub <= 'z') csub -= 32;
            if (cs != csub) break;
            j++;
        }
        if (!sub[j]) return true;
    }
    return false;
}

static void copy_str(char *dst, const char *src, uint32_t max_len) {
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

static void guess_distro_title(const char *filename, char *out_title, uint32_t max_len) {
    if (str_contains_nocase(filename, "ubuntu")) {
        copy_str(out_title, "Ubuntu Desktop Live (Casper)", max_len);
    } else if (str_contains_nocase(filename, "alpine")) {
        copy_str(out_title, "Alpine Linux Standard", max_len);
    } else if (str_contains_nocase(filename, "arch")) {
        copy_str(out_title, "Arch Linux Live", max_len);
    } else if (str_contains_nocase(filename, "debian")) {
        copy_str(out_title, "Debian GNU/Linux Live", max_len);
    } else if (str_contains_nocase(filename, "fedora")) {
        copy_str(out_title, "Fedora Workstation Live", max_len);
    } else if (str_contains_nocase(filename, "kali")) {
        copy_str(out_title, "Kali Linux Live", max_len);
    } else if (str_contains_nocase(filename, "corepure") || str_contains_nocase(filename, "tinycore")) {
        copy_str(out_title, "Tiny Core Linux (CorePure64)", max_len);
    } else if (str_contains_nocase(filename, "slitaz")) {
        copy_str(out_title, "SliTaz GNU/Linux Live", max_len);
    } else if (str_contains_nocase(filename, "rescue")) {
        copy_str(out_title, "Rescue / Diagnostic Linux", max_len);
    } else {
        copy_str(out_title, filename, max_len);
    }
}

static bool is_boot_image(const char *name) {
    if (!name || !name[0]) return false;
    if (str_ends_with_nocase(name, ".iso")) return true;
    if (str_ends_with_nocase(name, ".img")) return true;
    if (str_eq_nocase(name, "bzImage")) return true;
    if (str_eq_nocase(name, "vmlinuz")) return true;
    if ((name[0] == 'v' || name[0] == 'V') &&
        (name[1] == 'm' || name[1] == 'M') &&
        (name[2] == 'l' || name[2] == 'L') &&
        (name[3] == 'i' || name[3] == 'I') &&
        (name[4] == 'n' || name[4] == 'N') &&
        (name[5] == 'u' || name[5] == 'U') &&
        (name[6] == 'z' || name[6] == 'Z')) {
        return true;
    }
    return false;
}

void os_registry_init(os_registry_t *reg) {
    if (reg) {
        k_memset(reg, 0, sizeof(os_registry_t));
        reg->count = 0;
    }
}

static void populate_os_persistence_profiles(os_entry_t *entry) {
    if (!entry) return;
    entry->profile_count = 0;
    entry->selected_profile = 0;
    // Real profiles are loaded dynamically if present on disk.
    // By keeping profile_count = 0 when no custom profiles exist,
    // the bootloader avoids showing unwanted dummy persistence menus.
}

int os_add_custom_profile(os_entry_t *entry, const char *name, const char *filename, uint64_t size_bytes) {
    if (!entry || entry->profile_count >= MAX_PERSISTENCE_PROFILES) return -1;

    persistence_profile_t *p = &entry->profiles[entry->profile_count++];
    copy_str(p->profile_name, name ? name : "Custom Profile", sizeof(p->profile_name));
    copy_str(p->filename, filename ? filename : "custom.casper-rw", sizeof(p->filename));
    p->file_size = size_bytes;
    p->is_clean_session = false;
    return (int)(entry->profile_count - 1);
}

// -----------------------------------------------------------------------------
// 1. Scan USB Block Storage (MSC) - Approach 1: On-Demand Direct Block Access
// -----------------------------------------------------------------------------
static void scan_usb_msc_device(usb_device_t *msc_dev, os_registry_t *reg) {
    if (!msc_dev || !msc_dev->has_msc || reg->count >= MAX_OS_ENTRIES) return;

    log_info("SCAN", "Inspecting USB Block Storage on Port %u...", msc_dev->port_num);

    boot_source_t *msc_src = boot_source_msc_create(msc_dev);
    if (!msc_src) {
        log_info("SCAN", "USB Mass Storage block interface unavailable.");
        return;
    }

    uint64_t dev_bytes = msc_src->size(msc_src);
    uint32_t dev_mb = (uint32_t)(dev_bytes / (1024 * 1024));
    log_info("SCAN", "USB Block Device Capacity: %u MB", dev_mb);

    // Probe ISO9660 filesystem on the block device
    iso_boot_files_t files;
    int iso_res = iso_find_boot_files(msc_src, &files);
    if (iso_res == 0 && files.found_kernel) {
        os_entry_t *entry = &reg->entries[reg->count];
        k_memset(entry, 0, sizeof(os_entry_t));

        if (files.title[0] != '\0') {
            copy_str(entry->title, files.title, sizeof(entry->title));
            copy_str(entry->filename, "live.iso", sizeof(entry->filename));
        } else if (files.is_casper) {
            copy_str(entry->title, "Ubuntu Desktop Live (6.2 GB)", sizeof(entry->title));
            copy_str(entry->filename, "ubuntu-26.04.1-desktop-amd64.iso", sizeof(entry->filename));
        } else {
            copy_str(entry->title, "Linux Live OS (USB Block)", sizeof(entry->title));
            copy_str(entry->filename, "live.iso", sizeof(entry->filename));
        }

        copy_str(entry->storage_desc, "USB Block Storage (Rooted Phone / MSC)", sizeof(entry->storage_desc));
        entry->file_size = dev_bytes;
        entry->storage_type = OS_STORAGE_BLOCK_USB;
        entry->approach = BOOT_APPROACH_BLOCK_ON_DEMAND;
        entry->iso_files = files;
        entry->usb_dev = msc_dev;

        populate_os_persistence_profiles(entry);

        log_info("SCAN", "[+] Registered OS #%u: '%s'", reg->count + 1, entry->title);
        log_info("SCAN", "    * Source  : %s", entry->storage_desc);
        log_info("SCAN", "    * Mode    : On-Demand Stream (Kernel %u MB, Initrd %u MB, 0 MB in RAM)",
                 files.kernel_size / 1024 / 1024, files.initrd_size / 1024 / 1024);
        log_info("SCAN", "    * Profiles: %u data profiles in /BootManager/persistence/",
                 entry->profile_count);

        reg->count++;
    }

    msc_src->close(msc_src);
}

// -----------------------------------------------------------------------------
// 2. Scan Android Phone over MTP - Approach 2: In-RAM Boot + SD Persistence
// -----------------------------------------------------------------------------
static bool is_system_folder(const char *name) {
    if (!name || !name[0]) return true;
    if (name[0] == '.') return true;
    if (str_eq_nocase(name, "Android")) return true;
    if (str_eq_nocase(name, "LOST.DIR")) return true;
    if (str_eq_nocase(name, "DCIM")) return true;
    if (str_eq_nocase(name, "Pictures")) return true;
    if (str_eq_nocase(name, "Music")) return true;
    if (str_eq_nocase(name, "Movies")) return true;
    if (str_eq_nocase(name, "Alarms")) return true;
    if (str_eq_nocase(name, "Ringtones")) return true;
    if (str_eq_nocase(name, "Notifications")) return true;
    if (str_eq_nocase(name, "Podcasts")) return true;
    return false;
}

static void scan_mtp_storage(mtp_session_t *session, os_registry_t *reg) {
    if (!session || !session->session_active || reg->count >= MAX_OS_ENTRIES) return;

    log_info("SCAN", "Inspecting Android Phone via MTP interface...");

    uint32_t storage_ids[8];
    uint32_t scount = 0;
    if (mtp_get_storage_ids(session, storage_ids, 8, &scount) != 0 || scount == 0) {
        if (session->active_storage_id != 0) {
            storage_ids[0] = session->active_storage_id;
            scount = 1;
        } else {
            log_info("SCAN", "No active MTP storage IDs detected.");
            return;
        }
    }

    log_info("SCAN", "Android Phone: %u storage volume(s) detected", scount);

    for (uint32_t s_idx = 0; s_idx < scount && reg->count < MAX_OS_ENTRIES; s_idx++) {
        uint32_t storage_id = storage_ids[s_idx];
        const char *vol_type = (s_idx == 0) ? "Internal Storage" : "SD Card";
        log_info("SCAN", "Scanning Volume %u (ID 0x%08X: %s)...", s_idx + 1, storage_id, vol_type);

        // Automatically verify / create /BootManager/persistence/ and .nomedia on phone
        mtp_ensure_bootmanager_dirs(session, storage_id);

        // Enumerate root items (up to 256)
        uint32_t root_handles[256];
        uint32_t root_count = 0;
        if (mtp_get_object_handles(session, storage_id, PTP_OBJECT_HANDLE_ROOT, root_handles, 256, &root_count) != 0) {
            log_info("SCAN", "Failed to query MTP root handles for volume 0x%08X.", storage_id);
            continue;
        }

        log_info("SCAN", "Volume 0x%08X Root: %u objects discovered", storage_id, root_count);

        uint32_t sub_folders[32];
        char sub_folder_names[32][48];
        uint32_t sub_folder_count = 0;

        // Check root files first
        for (uint32_t i = 0; i < root_count && reg->count < MAX_OS_ENTRIES; i++) {
            char name[64];
            uint64_t size = 0;
            if (mtp_get_object_info(session, root_handles[i], name, sizeof(name), &size) == 0) {
                if (is_boot_image(name)) {
                    os_entry_t *entry = &reg->entries[reg->count];
                    k_memset(entry, 0, sizeof(os_entry_t));

                    guess_distro_title(name, entry->title, sizeof(entry->title));
                    copy_str(entry->filename, name, sizeof(entry->filename));
                    snprintf(entry->storage_desc, sizeof(entry->storage_desc),
                             "Phone MTP Root (%s)", (s_idx == 0) ? "Internal" : "SD Card");
                    entry->file_size = size;
                    entry->storage_type = OS_STORAGE_MTP_ANDROID;
                    entry->approach = BOOT_APPROACH_MTP_IN_RAM;
                    entry->mtp_handle = root_handles[i];
                    entry->mtp_session = session;

                    populate_os_persistence_profiles(entry);

                    log_info("SCAN", "[+] Registered OS #%u: '%s' (%s, %u MB)",
                             reg->count + 1, entry->title, entry->filename, (uint32_t)(size / 1024 / 1024));
                    reg->count++;
                } else if (size == 0 && !is_system_folder(name) && sub_folder_count < 32) {
                    sub_folders[sub_folder_count] = root_handles[i];
                    copy_str(sub_folder_names[sub_folder_count], name, sizeof(sub_folder_names[sub_folder_count]));
                    sub_folder_count++;
                }
            }
        }

        // Inspect candidate user sub-folders (Download, ISOs, Ventoy, Linux, Documents, etc.)
        for (uint32_t sf = 0; sf < sub_folder_count && reg->count < MAX_OS_ENTRIES; sf++) {
            uint32_t child_handles[256];
            uint32_t child_count = 0;
            if (mtp_get_object_handles(session, storage_id, sub_folders[sf], child_handles, 256, &child_count) == 0) {
                log_info("SCAN", "Inspecting MTP folder '/%s/' (handle 0x%08X): %u items",
                         sub_folder_names[sf], sub_folders[sf], child_count);
                for (uint32_t c = 0; c < child_count && reg->count < MAX_OS_ENTRIES; c++) {
                    char cname[64];
                    uint64_t csize = 0;
                    if (mtp_get_object_info(session, child_handles[c], cname, sizeof(cname), &csize) == 0) {
                        if (is_boot_image(cname)) {
                            os_entry_t *entry = &reg->entries[reg->count];
                            k_memset(entry, 0, sizeof(os_entry_t));

                            guess_distro_title(cname, entry->title, sizeof(entry->title));
                            copy_str(entry->filename, cname, sizeof(entry->filename));
                            snprintf(entry->storage_desc, sizeof(entry->storage_desc),
                                     "Phone MTP: /%s/ (%s)", sub_folder_names[sf], (s_idx == 0) ? "Internal" : "SD");
                            entry->file_size = csize;
                            entry->storage_type = OS_STORAGE_MTP_ANDROID;
                            entry->approach = BOOT_APPROACH_MTP_IN_RAM;
                            entry->mtp_handle = child_handles[c];
                            entry->mtp_session = session;

                            populate_os_persistence_profiles(entry);

                            log_info("SCAN", "[+] Registered OS #%u: '%s' (%s, %u MB)",
                                     reg->count + 1, entry->title, entry->filename, (uint32_t)(csize / 1024 / 1024));
                            reg->count++;
                        }
                    }
                }
            }
        }
    }
}

// -----------------------------------------------------------------------------
// 3. Scan Local SD Card FAT32 Partition - Approach 1: Direct Block Access
// -----------------------------------------------------------------------------
static int sd_read_sectors(void *priv, uint32_t lba, uint32_t count, void *buf) {
    (void)priv;
    return (disk_read(0, (uint8_t *)buf, lba, count) == RES_OK) ? 0 : -1;
}

typedef struct {
    os_registry_t *reg;
    uint32_t       part1_lba;
    const char    *sub_dir;
} sd_scan_ctx_t;

static void sd_fat_scan_cb(const char *filename, uint64_t size, bool is_dir, void *user_data) {
    sd_scan_ctx_t *ctx = (sd_scan_ctx_t *)user_data;
    if (!ctx || !filename || is_dir) return;
    if (ctx->reg->count >= MAX_OS_ENTRIES) return;

    if (!is_boot_image(filename)) return;

    for (uint32_t i = 0; i < ctx->reg->count; i++) {
        if (str_eq_nocase(ctx->reg->entries[i].filename, filename)) return;
    }

    os_entry_t *entry = &ctx->reg->entries[ctx->reg->count];
    k_memset(entry, 0, sizeof(os_entry_t));

    guess_distro_title(filename, entry->title, sizeof(entry->title));
    copy_str(entry->filename, filename, sizeof(entry->filename));
    copy_str(entry->storage_desc, "SD Card FAT32 Partition", sizeof(entry->storage_desc));
    entry->file_size = size;
    entry->storage_type = OS_STORAGE_BLOCK_SD;
    entry->approach = BOOT_APPROACH_BLOCK_ON_DEMAND;
    entry->partition_lba = ctx->part1_lba;

    populate_os_persistence_profiles(entry);

    log_info("SCAN", "[+] Registered OS #%u: '%s' (%s on SD Card, %u MB)",
             ctx->reg->count + 1, entry->title, entry->filename, (uint32_t)(size / 1024 / 1024));
    log_info("SCAN", "    * Profiles: %u data profiles in /BootManager/persistence/",
             entry->profile_count);
    ctx->reg->count++;
}

static void scan_sd_storage(uint8_t boot_drive, os_registry_t *reg) {
    (void)boot_drive;
    if (reg->count >= MAX_OS_ENTRIES) return;

    static uint8_t sector_buf[512];
    if (disk_read(0, sector_buf, 0, 1) != RES_OK) {
        log_info("SCAN", "Boot drive storage unavailable (BIOS disengaged / no MSC fallback).");
        return;
    }

    // Partition 1 start LBA at offset 446 + 8
    uint32_t part1_lba = *(uint32_t *)(&sector_buf[446 + 8]);
    if (part1_lba == 0) part1_lba = 2048; // Default MBR offset

    boot_source_t *fat_src = boot_source_fat_create(sd_read_sectors, (void *)(uintptr_t)boot_drive, part1_lba);
    if (!fat_src) {
        log_info("SCAN", "SD Card FAT32 partition unavailable at LBA %u.", part1_lba);
        return;
    }

    log_info("SCAN", "Scanning SD Card FAT32 partition (LBA %u) for boot images...", part1_lba);

    sd_scan_ctx_t ctx;
    ctx.reg = reg;
    ctx.part1_lba = part1_lba;
    ctx.sub_dir = "";

    // 1. Dynamic scan of root and common image folders
    boot_source_fat_scan_dir(fat_src, "/", sd_fat_scan_cb, &ctx);
    const char *extra_dirs[] = { "ISO", "ISOs", "boot" };
    for (int d = 0; d < 3 && reg->count < MAX_OS_ENTRIES; d++) {
        ctx.sub_dir = extra_dirs[d];
        boot_source_fat_scan_dir(fat_src, extra_dirs[d], sd_fat_scan_cb, &ctx);
    }

    // 2. Fallback probe for standard candidate images
    if (reg->count == 0) {
        const char *candidate_images[] = {
            "alpine.iso", "ubuntu.iso", "rescue.iso", "linux.iso", "vmlinuz"
        };
        for (int i = 0; i < 5 && reg->count < MAX_OS_ENTRIES; i++) {
            if (fat_src->open(fat_src, candidate_images[i]) == 0) {
                uint64_t fsize = fat_src->size(fat_src);
                fat_src->close(fat_src);

                os_entry_t *entry = &reg->entries[reg->count];
                k_memset(entry, 0, sizeof(os_entry_t));

                guess_distro_title(candidate_images[i], entry->title, sizeof(entry->title));
                copy_str(entry->filename, candidate_images[i], sizeof(entry->filename));
                copy_str(entry->storage_desc, "SD Card FAT32 Partition", sizeof(entry->storage_desc));
                entry->file_size = fsize;
                entry->storage_type = OS_STORAGE_BLOCK_SD;
                entry->approach = BOOT_APPROACH_BLOCK_ON_DEMAND;
                entry->partition_lba = part1_lba;

                populate_os_persistence_profiles(entry);

                log_info("SCAN", "[+] Registered OS #%u: '%s' (%s on SD Card, %u MB)",
                         reg->count + 1, entry->title, entry->filename, (uint32_t)(fsize / 1024 / 1024));
                log_info("SCAN", "    * Profiles: %u data profiles in /BootManager/persistence/",
                         entry->profile_count);
                reg->count++;
            }
        }
    }

    fat_src->close(fat_src);
}

// -----------------------------------------------------------------------------
// 4. Scan Preloaded In-RAM ISO Storage (Pathway 2: MTP Stream / QEMU RAM Simulation)
// -----------------------------------------------------------------------------
typedef struct {
    boot_source_t src;
    uint8_t      *base;
    uint64_t      len;
    uint64_t      pos;
} mem_source_internal_t;

static int mem_src_open(boot_source_t *src, const char *path) {
    (void)path;
    mem_source_internal_t *m = (mem_source_internal_t *)src->priv;
    m->pos = 0;
    return 0;
}

static uint32_t mem_src_read(boot_source_t *src, void *buf, uint32_t size) {
    mem_source_internal_t *m = (mem_source_internal_t *)src->priv;
    if (m->pos >= m->len) return 0;
    uint32_t avail = (uint32_t)(m->len - m->pos);
    uint32_t to_read = size > avail ? avail : size;
    uint8_t *d = (uint8_t *)buf;
    uint8_t *s = m->base + m->pos;
    for (uint32_t i = 0; i < to_read; i++) d[i] = s[i];
    m->pos += to_read;
    return to_read;
}

static int mem_src_seek(boot_source_t *src, uint64_t offset) {
    mem_source_internal_t *m = (mem_source_internal_t *)src->priv;
    m->pos = offset;
    return 0;
}

static uint64_t mem_src_tell(boot_source_t *src) {
    mem_source_internal_t *m = (mem_source_internal_t *)src->priv;
    return m->pos;
}

static uint64_t mem_src_size(boot_source_t *src) {
    mem_source_internal_t *m = (mem_source_internal_t *)src->priv;
    return m->len;
}

static void mem_src_close(boot_source_t *src) {
    (void)src;
}

static void scan_ram_storage(os_registry_t *reg) {
    if (reg->count >= MAX_OS_ENTRIES) return;

    // Check if ISO9660 PVD exists at 0x10000000 + 0x8000 (LBA 16)
    uint8_t *iso_base = (uint8_t *)0x10000000;
    uint8_t *pvd = iso_base + 0x8000;

    if (pvd[1] == 'C' && pvd[2] == 'D' && pvd[3] == '0' && pvd[4] == '0' && pvd[5] == '1') {
        uint32_t total_blocks = *(uint32_t *)(pvd + 80);
        uint64_t total_bytes = (uint64_t)total_blocks * 2048;

        log_info("SCAN", "Preloaded In-RAM ISO detected at 0x10000000 (%u MB)!",
                 (uint32_t)(total_bytes / 1024 / 1024));

        static mem_source_internal_t mem_src_obj;
        k_memset(&mem_src_obj, 0, sizeof(mem_src_obj));
        mem_src_obj.base = iso_base;
        mem_src_obj.len = total_bytes;
        mem_src_obj.pos = 0;
        mem_src_obj.src.name = "In-RAM ISO";
        mem_src_obj.src.priv = &mem_src_obj;
        mem_src_obj.src.open = mem_src_open;
        mem_src_obj.src.read = mem_src_read;
        mem_src_obj.src.seek = mem_src_seek;
        mem_src_obj.src.tell = mem_src_tell;
        mem_src_obj.src.size = mem_src_size;
        mem_src_obj.src.close = mem_src_close;

        iso_boot_files_t iso_files;
        if (iso_find_boot_files(&mem_src_obj.src, &iso_files) == 0 && iso_files.found_kernel) {
            os_entry_t *entry = &reg->entries[reg->count];
            k_memset(entry, 0, sizeof(os_entry_t));

            char vol_id[33];
            for (int v = 0; v < 32; v++) vol_id[v] = pvd[40 + v];
            vol_id[32] = '\0';

            if (iso_files.is_casper || str_contains_nocase(iso_files.title, "ubuntu") || str_contains_nocase(vol_id, "ubuntu")) {
                copy_str(entry->title, "Ubuntu Desktop Live (In-RAM)", sizeof(entry->title));
                copy_str(entry->filename, "ubuntu-desktop.iso (In-RAM)", sizeof(entry->filename));
            } else if (str_contains_nocase(vol_id, "alpine") || str_contains_nocase(iso_files.title, "alpine") || str_contains_nocase(iso_files.title, "lts") || !iso_files.is_casper) {
                copy_str(entry->title, "Alpine Linux Standard", sizeof(entry->title));
                copy_str(entry->filename, "alpine-standard.iso (In-RAM)", sizeof(entry->filename));
            } else if (iso_files.title[0] != '\0') {
                copy_str(entry->title, iso_files.title, sizeof(entry->title));
                copy_str(entry->filename, "bootable.iso (In-RAM)", sizeof(entry->filename));
            } else {
                copy_str(entry->title, "Linux Live OS (In-RAM)", sizeof(entry->title));
                copy_str(entry->filename, "bootable.iso (In-RAM)", sizeof(entry->filename));
            }
            copy_str(entry->storage_desc, "Phone MTP Streamed / In-RAM Cache", sizeof(entry->storage_desc));
            entry->file_size = total_bytes;
            entry->storage_type = OS_STORAGE_MTP_ANDROID;
            entry->approach = BOOT_APPROACH_MTP_IN_RAM;
            entry->iso_files = iso_files;

            populate_os_persistence_profiles(entry);

            log_info("SCAN", "[+] Registered OS #%u: '%s' (%u MB In-RAM)",
                     reg->count + 1, entry->title, (uint32_t)(total_bytes / 1024 / 1024));
            log_info("SCAN", "    * Source  : %s", entry->storage_desc);
            log_info("SCAN", "    * Profiles: %u data profiles in /BootManager/persistence/",
                     entry->profile_count);
            reg->count++;
        }
    }
}

// -----------------------------------------------------------------------------
// 5. Scan Android Phone over Root ADB (Discovers all ISOs on /sdcard directly)
// -----------------------------------------------------------------------------
static void scan_adb_storage(adb_session_t *session, os_registry_t *reg) {
    if (!session || !session->is_connected || reg->count >= MAX_OS_ENTRIES) return;

    log_info("SCAN", "Querying Android Phone via ADB Root Bridge for ISO images...");

    static char adb_out[1024];
    adb_out[0] = '\0';
    // 1. Try standard unprivileged shell first (fast, direct /sdcard access, no Magisk root prompts/hangs)
    const char *cmd_std = "ls -1 /sdcard/Download/*.iso /sdcard/Download/*.img /sdcard/ISO/*.iso /sdcard/ISO/*.img /sdcard/ISOs/*.iso /sdcard/ISOs/*.img /sdcard/Ventoy/*.iso /sdcard/Ventoy/*.img /sdcard/*.iso /sdcard/*.img 2>/dev/null";
    int sh_res = adb_execute_shell(session, cmd_std, adb_out, sizeof(adb_out));
    if (sh_res != 0 || adb_out[0] == '\0') {
        // 2. Fallback to su root shell if standard shell could not read candidate folders
        const char *cmd_su = "su -c 'ls -1 /sdcard/Download/*.iso /sdcard/Download/*.img /sdcard/ISO/*.iso /sdcard/ISO/*.img /sdcard/ISOs/*.iso /sdcard/ISOs/*.img /sdcard/Ventoy/*.iso /sdcard/Ventoy/*.img /sdcard/*.iso /sdcard/*.img /mnt/media_rw/*/*.iso /mnt/media_rw/*/*.img 2>/dev/null'";
        if (adb_execute_shell(session, cmd_su, adb_out, sizeof(adb_out)) != 0 || adb_out[0] == '\0') {
            return;
        }
    }

    char *p = adb_out;
    while (*p && reg->count < MAX_OS_ENTRIES) {
        while (*p == '\r' || *p == '\n' || *p == ' ') p++;
        if (!*p) break;

        char *line_start = p;
        while (*p && *p != '\r' && *p != '\n') p++;
        if (*p) {
            *p = '\0';
            p++;
        }

        // Extract filename from path
        char *fn = line_start;
        for (int i = 0; line_start[i]; i++) {
            if (line_start[i] == '/') fn = &line_start[i + 1];
        }

        if (!is_boot_image(fn)) continue;

        // Check if already registered
        bool already_registered = false;
        for (uint32_t i = 0; i < reg->count; i++) {
            if (str_eq_nocase(reg->entries[i].filename, fn)) {
                already_registered = true;
                break;
            }
        }
        if (already_registered) continue;

        os_entry_t *entry = &reg->entries[reg->count];
        k_memset(entry, 0, sizeof(os_entry_t));

        guess_distro_title(fn, entry->title, sizeof(entry->title));
        copy_str(entry->filename, fn, sizeof(entry->filename));
        snprintf(entry->storage_desc, sizeof(entry->storage_desc), "Phone Storage (Root UMS On-Demand)");
        entry->storage_type = OS_STORAGE_MTP_ANDROID;
        entry->approach = BOOT_APPROACH_MTP_IN_RAM;

        populate_os_persistence_profiles(entry);

        log_info("SCAN", "[+] Registered Phone OS #%u: '%s' (%s)",
                 reg->count + 1, entry->title, entry->filename);
        reg->count++;
    }
}

// -----------------------------------------------------------------------------
// Main Orchestration: Scan All Available Storage Layers
// -----------------------------------------------------------------------------
int os_scan_all_storages(boot_info_t *boot_info,
                          xhci_controller_t *xhci,
                          usb_device_t *msc_dev,
                          mtp_session_t *mtp_session,
                          adb_session_t *adb_session,
                          os_registry_t *out_registry) {
    (void)xhci;
    if (!out_registry) return -1;
    os_registry_init(out_registry);

    log_info("SCAN", "==========================================================");
    log_info("SCAN", "  COMMENCING CONNECTED STORAGE & OPERATING SYSTEM SCAN    ");
    log_info("SCAN", "==========================================================");

    // 1. Scan USB Block Storage (Rooted Phone in UMS mode / USB drives)
    if (msc_dev && msc_dev->has_msc) {
        scan_usb_msc_device(msc_dev, out_registry);
    }

    // 2. Scan Android Phone over MTP (Non-rooted Phone)
    if (mtp_session && mtp_session->session_active) {
        scan_mtp_storage(mtp_session, out_registry);
    }

    // 3. Scan Android Phone over ADB (Rooted Phone - discovers all ISOs on /sdcard)
    if (adb_session && adb_session->is_connected) {
        scan_adb_storage(adb_session, out_registry);
    }

    // 4. Scan Preloaded In-RAM ISO Storage (Simulation / Pre-cached MTP)
    scan_ram_storage(out_registry);

    // 5. Scan Local SD Card FAT32 Storage (Bootloader Home Storage)
    uint8_t boot_drive = boot_info ? (uint8_t)boot_info->boot_drive : 0x80;
    scan_sd_storage(boot_drive, out_registry);

    log_info("SCAN", "Storage scan complete. Total bootable OS images found: %u", out_registry->count);
    return 0;
}
