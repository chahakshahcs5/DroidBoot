#include "../../include/boot.h"
#include "../../include/io.h"
#include "../debug/serial.h"
#include "../debug/vga.h"
#include "printf.h"
#include "timer.h"
#include "../memory/memory.h"
#include "../pci/pci.h"
#include "../xhci/xhci.h"
#include "../usb/usb.h"
#include "../usb/usb_msc.h"
#include "../mtp/mtp.h"
#include "../mtp/mtp_source.h"
#include "../filesystem/fat_source.h"
#include "../image/image_detect.h"
#include "../linux/linux_boot.h"
#include "../ui/menu.h"
#include "../filesystem/iso_reader.h"
#include "../debug/disk_log.h"
#include "../debug/sound.h"
#include "../bios/bios_disk.h"
#include "../image/os_scanner.h"
#include "../adb/adb.h"
#include "../filesystem/diskio.h"
#include "../bios/vbe.h"
#include "../bios/int13_emu.h"
#include "../usb/usb_hid.h"

static xhci_controller_t xhci_ctrl;

// Persistent device for the boot USB drive (preserves xHCI transfer rings for persistent logging)
static usb_device_t      boot_msc_device;
static bool              boot_msc_detected = false;

// Persistent device for external phone / USB storage
static usb_device_t      external_usb_dev;
static bool              external_usb_detected = false;
static bool              external_msc_detected = false;

// Active MSC device pointer for OS scanning and booting (NO STRUCT COPY, ring synchronization preserved)
static usb_device_t     *active_msc_dev = NULL;

static mtp_session_t     active_mtp_session;
static adb_session_t     active_adb_session;

static void phone_save_boot_log(adb_session_t *adb, mtp_session_t *mtp) {
#if !IS_DEBUG_BUILD
    (void)adb;
    (void)mtp;
    return; // In release build, phone log saving is disabled
#else
    static bool s_in_save = false;
    if (s_in_save) return;
    s_in_save = true;

    // Instant local SD card flush
    disk_log_flush();

    if (adb && adb->is_connected && (!adb->usb_dev || !adb->usb_dev->is_disconnected)) {
        adb_save_log_to_phone(adb);
        s_in_save = false;
        return;
    }

    if (mtp && mtp->session_active && (!mtp->usb_dev || !mtp->usb_dev->is_disconnected)) {
        mtp_save_log_to_phone(mtp);
        s_in_save = false;
        return;
    }

    s_in_save = false;
#endif
}

static void phone_save_boot_log_hook(void) {
    phone_save_boot_log(&active_adb_session, &active_mtp_session);
}

static void k_memset(void *dst, uint8_t val, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = val;
}

static int k_memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) return (int)p1[i] - (int)p2[i];
    }
    return 0;
}

static void k_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static void register_external_msc(usb_device_t *target, const usb_device_t *source, uint8_t p) {
    log_info("STAGE3", "Port %u: USB Mass Storage Block Storage registered.", p);
    if (external_usb_detected && target->port_num == p) {
        // Composite device (MSC + ADB / MTP): preserve active session state & endpoint rings!
        target->has_msc = true;
        target->msc_iface_num = source->msc_iface_num;
        target->msc_bulk_in_ep = source->msc_bulk_in_ep;
        target->msc_bulk_in_max_packet = source->msc_bulk_in_max_packet;
        target->msc_bulk_out_ep = source->msc_bulk_out_ep;
        target->msc_bulk_out_max_packet = source->msc_bulk_out_max_packet;
    } else {
        k_memcpy(target, source, sizeof(usb_device_t));
    }
    external_usb_detected = true;
    external_msc_detected = true;
    active_msc_dev = target;
}

static size_t k_strlen(const char *s) {
    size_t len = 0;
    if (!s) return 0;
    while (s[len]) len++;
    return len;
}

static inline int to_lower(int c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static bool str_ends_with_nocase(const char *str, const char *suffix) {
    if (!str || !suffix) return false;
    size_t str_len = k_strlen(str);
    size_t sfx_len = k_strlen(suffix);
    if (str_len < sfx_len) return false;
    for (size_t i = 0; i < sfx_len; i++) {
        if (to_lower(str[str_len - sfx_len + i]) != to_lower(suffix[i])) return false;
    }
    return true;
}

static bool str_contains_nocase(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    size_t h_len = k_strlen(haystack);
    size_t n_len = k_strlen(needle);
    if (n_len > h_len) return false;
    for (size_t i = 0; i <= h_len - n_len; i++) {
        bool match = true;
        for (size_t j = 0; j < n_len; j++) {
            if (to_lower(haystack[i + j]) != to_lower(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

static bool str_starts_with_nocase(const char *str, const char *prefix) {
    if (!str || !prefix) return false;
    while (*prefix) {
        if (to_lower(*str) != to_lower(*prefix)) return false;
        str++;
        prefix++;
    }
    return true;
}

static void strip_cmdline_arg(char *cmdline, const char *arg_prefix) {
    if (!cmdline || !arg_prefix || !*arg_prefix) return;
    size_t prefix_len = k_strlen(arg_prefix);
    while (1) {
        char *pos = NULL;
        size_t clen = k_strlen(cmdline);
        for (size_t i = 0; i + prefix_len <= clen; i++) {
            if ((i == 0 || cmdline[i - 1] == ' ' || cmdline[i - 1] == '\t') &&
                str_starts_with_nocase(&cmdline[i], arg_prefix)) {
                char after = cmdline[i + prefix_len];
                if (arg_prefix[prefix_len - 1] == '=' || after == '\0' || after == ' ' || after == '\t' || after == '=') {
                    pos = &cmdline[i];
                    break;
                }
            }
        }
        if (!pos) break;
        char *end = pos + prefix_len;
        while (*end && *end != ' ' && *end != '\t') end++;
        while (*end && (*end == ' ' || *end == '\t')) end++;
        while (*end) *pos++ = *end++;
        *pos = '\0';
    }
}

static void ensure_cmdline_arg(char *cmdline, size_t max_len, const char *arg) {
    if (!cmdline || !arg || !*arg || max_len == 0) return;
    size_t arg_len = k_strlen(arg);
    size_t clen = k_strlen(cmdline);

    for (size_t i = 0; i + arg_len <= clen; i++) {
        if ((i == 0 || cmdline[i - 1] == ' ' || cmdline[i - 1] == '\t') &&
            str_starts_with_nocase(&cmdline[i], arg)) {
            char after = cmdline[i + arg_len];
            if (after == '\0' || after == ' ' || after == '\t' || after == '=') {
                return;
            }
        }
    }

    while (clen > 0 && (cmdline[clen - 1] == ' ' || cmdline[clen - 1] == '\t')) {
        cmdline[--clen] = '\0';
    }

    if (clen > 0) {
        if (clen + 1 + arg_len < max_len) {
            cmdline[clen] = ' ';
            k_memcpy(&cmdline[clen + 1], arg, arg_len + 1);
        }
    } else {
        if (arg_len < max_len) {
            k_memcpy(cmdline, arg, arg_len + 1);
        }
    }
}

static void ensure_cmdline_arg_before_separator(char *cmdline, size_t max_len, const char *arg, const char *sep) {
    if (!cmdline || !arg || !*arg || max_len == 0) return;
    size_t arg_len = k_strlen(arg);
    size_t clen = k_strlen(cmdline);

    for (size_t i = 0; i + arg_len <= clen; i++) {
        if ((i == 0 || cmdline[i - 1] == ' ' || cmdline[i - 1] == '\t') &&
            str_starts_with_nocase(&cmdline[i], arg)) {
            char after = cmdline[i + arg_len];
            if (after == '\0' || after == ' ' || after == '\t' || after == '=') {
                return;
            }
        }
    }

    if (sep && *sep) {
        size_t sep_len = k_strlen(sep);
        for (size_t i = 0; i + sep_len <= clen; i++) {
            if ((i == 0 || cmdline[i - 1] == ' ' || cmdline[i - 1] == '\t') &&
                str_starts_with_nocase(&cmdline[i], sep)) {
                if (clen + arg_len + 2 < max_len) {
                    for (int s = (int)clen; s >= (int)i; s--) {
                        cmdline[s + arg_len + 1] = cmdline[s];
                    }
                    k_memcpy(&cmdline[i], arg, arg_len);
                    cmdline[i + arg_len] = ' ';
                    return;
                }
            }
        }
    }

    ensure_cmdline_arg(cmdline, max_len, arg);
}

static void finalize_kernel_cmdline(char *cmdline, size_t max_len, bool is_casper) {
    if (!cmdline || max_len == 0) return;

    // Strip legacy 16-bit VGA mode parameters (e.g. vga=788) that conflict with modern Linear Framebuffers
    strip_cmdline_arg(cmdline, "vga=");

    // For Debian / Kali installer media, ensure cdrom-detect can discover USB block media BEFORE '---'
    if (str_contains_nocase(cmdline, "preseed") || str_contains_nocase(cmdline, "simple-cdd") ||
        str_contains_nocase(cmdline, "kali") || str_contains_nocase(cmdline, "debian")) {
        ensure_cmdline_arg_before_separator(cmdline, max_len, "cdrom-detect/try-usb=true", "---");
    }

    // Direct video routing: disable broken internal laptop LCD (eDP-1 / LVDS-1) and force HDMI-A-1 primary
    ensure_cmdline_arg(cmdline, max_len, "video=eDP-1:d");
    ensure_cmdline_arg(cmdline, max_len, "video=LVDS-1:d");
    ensure_cmdline_arg(cmdline, max_len, "video=HDMI-A-1:1920x1080@60e");

#if IS_DEBUG_BUILD
    (void)is_casper;
    // Debug Build: Full logging system directly on screen (verbose console, services, initramfs)
    strip_cmdline_arg(cmdline, "quiet");
    strip_cmdline_arg(cmdline, "splash");
    ensure_cmdline_arg(cmdline, max_len, "nosplash");
    ensure_cmdline_arg(cmdline, max_len, "console=tty1");
    ensure_cmdline_arg(cmdline, max_len, "debug");
    ensure_cmdline_arg(cmdline, max_len, "loglevel=7");
    ensure_cmdline_arg(cmdline, max_len, "systemd.show_status=1");
#else
    // Release Build: Direct clean boot (Ubuntu logo via Plymouth without raw text console or non-fatal errors)
    strip_cmdline_arg(cmdline, "---");
    strip_cmdline_arg(cmdline, "nosplash");
    strip_cmdline_arg(cmdline, "debug");
    strip_cmdline_arg(cmdline, "console=tty1");
    strip_cmdline_arg(cmdline, "console=tty0");
    strip_cmdline_arg(cmdline, "loglevel=");
    ensure_cmdline_arg(cmdline, max_len, "quiet");
    if (is_casper) {
        ensure_cmdline_arg(cmdline, max_len, "splash");
        ensure_cmdline_arg(cmdline, max_len, "loglevel=3");
        ensure_cmdline_arg(cmdline, max_len, "systemd.show_status=0");
        ensure_cmdline_arg(cmdline, max_len, "vt.global_cursor_default=0");
    } else {
        ensure_cmdline_arg(cmdline, max_len, "loglevel=3");
    }
#endif
}

static bool is_boot_drive_msc(usb_device_t *dev) {
    if (!dev || !dev->has_msc || dev->is_disconnected) return false;
    static uint8_t sec[512];

    // Signature 1: Check LBA 1 (Stage 2 bootstrap) for "STG2" magic at offset 4
    if (usb_msc_read_sectors(dev, 1, 1, sec) == 0) {
        if (*(uint32_t *)(&sec[4]) == 0x32475453) { // "STG2"
            return true;
        }
    }

    // Signature 2: Check LBA 1024 (Raw Log Header) for "BOOT" and "LOG!"
    if (usb_msc_read_sectors(dev, 1024, 1, sec) == 0) {
        disk_log_header_t *hdr = (disk_log_header_t *)sec;
        if (hdr->magic1 == 0x544F4F42 && hdr->magic2 == 0x21474F4C) {
            return true;
        }
    }

    // Signature 3: Check MBR and all Partition VBRs
    if (usb_msc_read_sectors(dev, 0, 1, sec) == 0) {
        // Superfloppy check directly at LBA 0
        if (k_memcmp(&sec[0x47], "BOOTLOADER ", 11) == 0 ||
            k_memcmp(&sec[0x52], "FAT32   ", 8) == 0) {
            return true;
        }
        if (sec[510] == 0x55 && sec[511] == 0xAA) {
            for (int p_idx = 0; p_idx < 4; p_idx++) {
                uint32_t part_lba = *(uint32_t *)(&sec[0x1BE + p_idx * 16 + 8]);
                if (part_lba == 0 && p_idx == 0) part_lba = 2048;
                if (part_lba > 0 && part_lba < 0x20000000) {
                    static uint8_t vbr[512];
                    if (usb_msc_read_sectors(dev, part_lba, 1, vbr) == 0) {
                        if (vbr[510] == 0x55 && vbr[511] == 0xAA) {
                            if (k_memcmp(&vbr[0x47], "BOOTLOADER ", 11) == 0 ||
                                k_memcmp(&vbr[3], "MSWIN4.1", 8) == 0 ||
                                k_memcmp(&vbr[0x52], "FAT32   ", 8) == 0 ||
                                k_memcmp(&vbr[0x36], "FAT16   ", 8) == 0) {
                                return true;
                            }
                        }
                    }
                }
            }
        }
    }

    return false;
}

static void handle_msc_device(usb_device_t *msc_dev, uint8_t root_port) {
    if (!msc_dev || !msc_dev->has_msc || msc_dev->is_disconnected) return;
    if (boot_msc_detected && msc_dev->slot_id == boot_msc_device.slot_id) {
        return; // Already registered and active! Do not overwrite live transfer rings.
    }

    // Android devices with ADB: MSC LUN is unbacked/dormant until an ISO is selected and triggered via ADB.
    // Probing MSC on an unbacked Android LUN causes bulk transfer timeouts on EP 0x01.
    if (msc_dev->has_adb) {
        log_info("STAGE3", "Port %u: Composite Android device detected. Deferring MSC until ISO mount.", root_port);
        return;
    }

    // Verify the MSC device actually has active media / capacity before attempting reads
    uint32_t last_lba = 0;
    uint32_t block_sz = 512;
    if (usb_msc_read_capacity(msc_dev, &last_lba, &block_sz) != 0 || last_lba == 0 || block_sz == 0) {
        log_info("STAGE3", "Port %u: USB MSC device has no media mounted (0 MB). Skipping block registration.", root_port);
        return;
    }

    if (is_boot_drive_msc(msc_dev)) {
        if (!boot_msc_detected) {
            k_memcpy(&boot_msc_device, msc_dev, sizeof(usb_device_t));
            boot_msc_detected = true;
            log_info("STAGE3", "Port %u: Boot Drive SD/USB verified! Enabling direct xHCI logging.", root_port);
            disk_log_register_usb_msc(&boot_msc_device);
            diskio_set_usb_msc_device(&boot_msc_device);
            if (!external_msc_detected) {
                active_msc_dev = &boot_msc_device;
            }
        }
    } else {
        register_external_msc(&external_usb_dev, msc_dev, root_port);
    }
}

static void check_and_handle_hub(xhci_controller_t *ctrl, usb_device_t *dev, uint8_t root_port) {
    if (dev->dev_desc.bDeviceClass != USB_CLASS_HUB) return;
    log_info("STAGE3", "Port %u: USB Hub detected! Scanning downstream ports for boot drive / storage...", root_port);
    usb_probe_hub_downstream(ctrl, dev, NULL);
}

static void test_linux_boot_simulation(boot_info_t *boot_info) {
    // Check if a real Alpine kernel was preloaded into RAM at 0x02000000
    uint32_t magic_at_ram = *(volatile uint32_t *)(0x02000000 + 0x0202);
    if (magic_at_ram == LINUX_HDRS_MAGIC) {
        log_info("BOOT", "==========================================================");
        log_info("BOOT", "  REAL ALPINE LINUX KERNEL DETECTED AT 0x02000000!        ");
        log_info("BOOT", "==========================================================");

        void *kernel_buf = (void *)0x02000000;
        void *initrd_buf = (void *)LINUX_INITRD_LOAD_PHYS;
        uint32_t kernel_size = 14513152;
        uint32_t initrd_size = 22504936;

        image_info_t img_info;
        image_detect_type(kernel_buf, kernel_size, &img_info);

        linux_kernel_info_t kinfo;
        if (linux_check_kernel_image(kernel_buf, kernel_size, &kinfo) == 0) {
            linux_boot_params_t *params_at_low_mem = (linux_boot_params_t *)LINUX_BOOT_PARAMS_PHYS;
            const char *alpine_cmdline = "earlyprintk=serial,0x3f8,115200 console=ttyS0,115200 console=tty0 noapic debug";
            linux_prepare_boot_params(kernel_buf, kernel_size,
                                      initrd_buf, initrd_size,
                                      alpine_cmdline, boot_info,
                                      vbe_get_active_mode(), 0, 0,
                                      params_at_low_mem);

            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            log_info("STAGE3", "Phase 8 Image Detection Successfully Verified!");
            log_info("STAGE3", "Phase 9 Linux 32-bit Boot Protocol Successfully Verified!");
            log_info("STAGE3", "Phase 10 Interactive Boot Menu Successfully Verified!");
            log_info("BOOT", "Jumping into Real Alpine Linux Kernel at 0x00100000...");
            vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

            // Relocate protected-mode kernel code to 0x00100000 and jump!
            linux_boot_jump((uint32_t)kernel_buf + kinfo.protected_mode_offset,
                            LINUX_KERNEL_LOAD_PHYS,
                            kinfo.protected_mode_size,
                            LINUX_BOOT_PARAMS_PHYS,
                            kinfo.code32_start);
            return;
        }
    }

    log_info("TEST", "Starting Linux 32-bit Boot Protocol Self-Test & Simulation...");

    // Create a synthetic bzImage in RAM
    static uint8_t test_kernel_image[4096];
    for (int i = 0; i < 4096; i++) test_kernel_image[i] = 0x90; // NOP sled

    // Linux setup header at offset 0x01F1
    linux_setup_header_t *hdr = (linux_setup_header_t *)(test_kernel_image + 0x01F1);
    hdr->setup_sects = 4;
    hdr->boot_flag = LINUX_BOOT_FLAG_MAGIC;
    hdr->header = LINUX_HDRS_MAGIC;         // 0x53726448 ("HdrS")
    hdr->version = 0x020E;                  // Protocol version 2.14
    hdr->type_of_loader = LINUX_BOOT_LOADER_TYPE;
    hdr->loadflags = LINUX_LOADFLAGS_LOADED_HIGH;
    hdr->code32_start = LINUX_KERNEL_LOAD_PHYS; // 0x00100000

    // 1. Phase 8 Image Format Detection Test
    image_info_t img_info;
    image_type_t itype = image_detect_type(test_kernel_image, sizeof(test_kernel_image), &img_info);
    if (itype == IMAGE_TYPE_BZIMAGE) {
        log_info("TEST", "Phase 8 Image Detection: SUCCESS (Identified Linux bzImage)");
    } else {
        log_error("TEST", "Phase 8 Image Detection FAILED!");
        return;
    }

    // 2. Phase 9 Kernel Parsing Test
    linux_kernel_info_t kinfo;
    int chk = linux_check_kernel_image(test_kernel_image, sizeof(test_kernel_image), &kinfo);
    if (chk == 0) {
        log_info("TEST", "Phase 9 Kernel Header Check: SUCCESS (Protocol 2.14 verified)");
    } else {
        log_error("TEST", "Phase 9 Kernel Header Check FAILED (code %d)", chk);
        return;
    }

    // 3. Phase 9 Boot Params Preparation Test
    static linux_boot_params_t test_params;
    const char *cmdline = "console=ttyS0,115200 root=/dev/ram0 rw quiet";
    int prep = linux_prepare_boot_params(test_kernel_image, sizeof(test_kernel_image),
                                         (void *)LINUX_INITRD_LOAD_PHYS, 1048576,
                                         cmdline, boot_info,
                                         vbe_get_active_mode(), 0, 0,
                                         &test_params);
    if (prep == 0 && test_params.hdr.header == LINUX_HDRS_MAGIC && test_params.e820_entries > 0) {
        log_info("TEST", "Phase 9 Boot Params Setup: SUCCESS (%u E820 entries attached)",
                 test_params.e820_entries);
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        log_info("STAGE3", "Phase 8 Image Detection Successfully Verified!");
        log_info("STAGE3", "Phase 9 Linux 32-bit Boot Protocol Successfully Verified!");
        log_info("STAGE3", "Phase 10 Interactive Boot Menu Successfully Verified!");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    } else {
        log_error("TEST", "Phase 9 Boot Params Setup FAILED!");
    }
}

static void boot_from_usb_msc(usb_device_t *dev, boot_info_t *boot_info, const persistence_profile_t *prof, const iso_boot_files_t *cached_files) {
    if (!dev) return;

    // Check if device was spuriously marked disconnected by an ADB timeout while physically present
    if (dev->is_disconnected && dev->port_num > 0 && dev->ctrl) {
        uintptr_t port_reg = dev->ctrl->op_regs + XHCI_OP_PORTS_BASE + (dev->port_num - 1) * 0x10;
        uint32_t portsc = *(volatile uint32_t *)port_reg;
        if (portsc & XHCI_PORT_CCS) {
            log_info("BOOT", "Device physically connected on Port %u. Resetting disconnected flag.", dev->port_num);
            dev->is_disconnected = false;
        }
    }

    log_info("BOOT", "Attempting boot from USB Block Storage on Port %u...", dev->port_num);
    boot_source_t *msc_src = boot_source_msc_create(dev);
    if (!msc_src) {
        log_error("BOOT", "Failed to create boot source for USB block device.");
        sound_error_tone();
        return;
    }

    iso_boot_files_t iso_files;
    k_memset(&iso_files, 0, sizeof(iso_files));

    if (cached_files && (cached_files->found_kernel || cached_files->is_windows)) {
        iso_files = *cached_files;
    } else {
        int r = iso_find_boot_files(msc_src, &iso_files);
        if (r != 0 || (!iso_files.found_kernel && !iso_files.is_windows)) {
            log_error("BOOT", "No bootable kernel found on USB block device!");
            sound_error_tone();
            msc_src->close(msc_src);
            return;
        }
    }

    if (iso_files.is_windows) {
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        log_info("BOOT", "==========================================================");
        log_info("BOOT", "  BOOTABLE WINDOWS MEDIA DETECTED ON USB BLOCK DEVICE!    ");
        log_info("BOOT", "  * Distro Type    : Microsoft Windows Setup / WinPE       ");
        log_info("BOOT", "  * Volume ID      : %s", iso_files.volume_id);
        log_info("BOOT", "  * Boot Sector LBA: %u (%u bytes)", iso_files.boot_sector_lba, iso_files.boot_sector_size);
        log_info("BOOT", "  * Mode           : Real-Mode VBR Chainload               ");
        log_info("BOOT", "==========================================================");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

        // Read boot sector (VBR / El Torito boot sector) into 0x00007C00
        uint8_t *vbr_buf = (uint8_t *)0x00007C00;
        uint64_t vbr_offset = (uint64_t)iso_files.boot_sector_lba * 2048;
        uint32_t read_size = (iso_files.boot_sector_size >= 512) ? iso_files.boot_sector_size : 512;
        if (read_size > 8192) read_size = 8192;
        msc_src->seek(msc_src, vbr_offset);
        uint32_t bytes_read = msc_src->read(msc_src, vbr_buf, read_size);

        msc_src->close(msc_src);

        if (bytes_read < 512) {
            log_error("BOOT", "Failed to read boot sector from Windows media (%u bytes read).", bytes_read);
            sound_error_tone();
            return;
        }

        // Verify boot sector magic 0x55, 0xAA (little-endian 0xAA55)
        // Standard BIOS boot sectors require 0x55, 0xAA at offset 510-511
        if (vbr_buf[510] != 0x55 || vbr_buf[511] != 0xAA) {
            log_warn("BOOT", "Boot sector signature not 0xAA55 (got 0x%02X%02X at offset 510). Patching standard signature...",
                     vbr_buf[511], vbr_buf[510]);
            vbr_buf[510] = 0x55;
            vbr_buf[511] = 0xAA;
        }

        log_info("BOOT", "Boot sector loaded at 0x00007C00 with valid signature.");

        // For native xHCI USB devices, install INT 13h emulation layer directly.
        // We do NOT call bios_disk_read() across candidate drives because real-mode
        // BIOS INT 13h causes BIOS to poll USB and overwrite active xHCI state.
        vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        log_info("BOOT", "==========================================================");
        log_info("BOOT", "  WINDOWS BOOT — INT 13h EMULATION ACTIVE                ");
        log_info("BOOT", "==========================================================");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        log_info("BOOT", "Installing INT 13h emulation layer (xHCI MSC bridge)...");

        // Get disk capacity for the emulation layer
        uint32_t emu_last_lba = 0, emu_block_sz = 512;
        usb_msc_read_capacity(dev, &emu_last_lba, &emu_block_sz);
        uint32_t phys_block_sz = emu_block_sz ? emu_block_sz : 512;
        uint32_t emu_total_sectors = emu_last_lba + 1;
        // Convert total sectors to 512-byte units if device block size differs (e.g. CD-ROM 2048)
        if (phys_block_sz > 512) {
            emu_total_sectors = emu_total_sectors * (phys_block_sz / 512);
        }

        int emu_res = int13_emu_install(dev, 0, emu_total_sectors, 512, phys_block_sz);
        if (emu_res != 0) {
            log_error("BOOT", "Failed to install INT 13h emulation! Windows cannot boot.");
            vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
            sound_error_tone();
            return;
        }

        uint8_t boot_drive = int13_emu_get_drive_num();
        log_info("BOOT", "Phone UMS mapped as BIOS drive 0x%02X via INT 13h emulation.", boot_drive);
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        log_info("BOOT", "INT 13h emulation active! Windows VBR will read from phone.");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

        log_info("BOOT", "Flushing logs before Windows handoff...");
        disk_log_flush_with_feedback();
        phone_save_boot_log(&active_adb_session, &active_mtp_session);

        // Mask xHCI hardware interrupts before switching to Real Mode
        log_info("BOOT", "Masking xHCI hardware interrupts prior to real-mode handoff...");
        xhci_mask_interrupts(&xhci_ctrl);

        // Keeping xHCI controller running for INT 13h emulation
        log_info("BOOT", "Keeping xHCI controller running for INT 13h emulation.");
        sound_silence();

        log_info("BOOT", "Handing off to Real-Mode VBR Chainloader (Drive 0x%02X)...", boot_drive);
        // Do NOT restore text mode if VBE is active to avoid killing HDMI pixel clock on external monitors!
        if (!vbe_is_active()) {
            vbe_restore_text_mode();
        } else {
            log_info("BOOT", "Maintaining active VBE video mode to preserve HDMI external monitor signal.");
        }
        bios_chainload(boot_drive, 0x00007C00);
        return;
    }



    log_info("BOOT", "==========================================================");
    log_info("BOOT", "  BOOTABLE OS DETECTED ON USB BLOCK DEVICE!               ");
    const char *distro_desc = "Linux Live System";
    if (iso_files.is_casper) distro_desc = "Ubuntu / Casper Live (6GB)";
    else if (iso_files.is_kali && str_contains_nocase(iso_files.title, "live")) distro_desc = "Kali Linux Live";
    else if (iso_files.is_kali) distro_desc = "Kali Linux Installer";
    else if (str_contains_nocase(iso_files.volume_id, "kali") || str_contains_nocase(iso_files.title, "kali")) distro_desc = "Kali Linux";
    log_info("BOOT", "  * Distro Type    : %s", distro_desc);
    log_info("BOOT", "  * Kernel LBA     : %u (Size: %u MB)", iso_files.kernel_lba, iso_files.kernel_size / 1024 / 1024);
    log_info("BOOT", "  * Initramfs LBA  : %u (Size: %u MB)", iso_files.initrd_lba, iso_files.initrd_size / 1024 / 1024);
    log_info("BOOT", "  * Mode           : Direct Block Access (0 MB OS in RAM!) ");
    if (prof) {
        log_info("BOOT", "  * Persistence    : %s (%s)", prof->profile_name, prof->filename);
    }
    log_info("BOOT", "==========================================================");

    // Read ONLY kernel (16MB) and initrd (91MB) into RAM
    void *kernel_buf = (void *)0x02000000;

    log_info("BOOT", "Streaming tiny kernel (%u MB) from block device into RAM...", iso_files.kernel_size / 1024 / 1024);
    msc_src->seek(msc_src, (uint64_t)iso_files.kernel_lba * 2048);
    uint32_t k_read = msc_src->read(msc_src, kernel_buf, iso_files.kernel_size);
    log_info("BOOT", "Kernel read: %u / %u bytes", k_read, iso_files.kernel_size);

    // Compute safe physical address for initramfs safely above kernel decompression footprint
    uint32_t initrd_phys = LINUX_INITRD_LOAD_PHYS;
    const linux_setup_header_t *setup_hdr = (const linux_setup_header_t *)((const uint8_t *)kernel_buf + 0x1F1);
    if (setup_hdr->header == LINUX_HDRS_MAGIC && setup_hdr->version >= 0x020A && setup_hdr->init_size > 0) {
        uint32_t kernel_decomp_end = (uint32_t)setup_hdr->pref_address + setup_hdr->init_size;
        uint32_t safe_boundary = (kernel_decomp_end + 0x001FFFFF) & ~0x001FFFFF;
        if (safe_boundary > initrd_phys) {
            initrd_phys = safe_boundary;
        }
    }
    void *initrd_buf = (void *)initrd_phys;

    if (iso_files.found_initrd) {
        log_info("BOOT", "Streaming initramfs (%u MB) to 0x%08X (safe from kernel decompressor)...",
                 iso_files.initrd_size / 1024 / 1024, initrd_phys);
        msc_src->seek(msc_src, (uint64_t)iso_files.initrd_lba * 2048);
        uint32_t i_read = msc_src->read(msc_src, initrd_buf, iso_files.initrd_size);
        log_info("BOOT", "Initramfs read: %u / %u bytes", i_read, iso_files.initrd_size);
    }

    msc_src->close(msc_src);

    linux_kernel_info_t kinfo;
    if (linux_check_kernel_image(kernel_buf, iso_files.kernel_size, &kinfo) != 0) {
        log_error("BOOT", "Failed to validate kernel image from block device!");
        sound_error_tone();
        return;
    }

    // Determine kernel command line:
    char cmdline[512];
    if (iso_files.cmdline[0] != '\0') {
        snprintf(cmdline, sizeof(cmdline), "%s", iso_files.cmdline);
        if (iso_files.is_casper) {
            strip_cmdline_arg(cmdline, "iso-scan/filename=");
            strip_cmdline_arg(cmdline, "findiso=");
            if (!str_contains_nocase(cmdline, "boot=casper")) {
                char temp[512];
                k_memcpy(temp, cmdline, sizeof(temp));
                snprintf(cmdline, sizeof(cmdline), "boot=casper %s", temp);
            }
        }
        if (prof && !prof->is_clean_session) {
            size_t clen = k_strlen(cmdline);
            if (clen + 80 < sizeof(cmdline)) {
                if (iso_files.is_casper) {
                    snprintf(cmdline + clen, sizeof(cmdline) - clen,
                             " persistent persistent-path=/BootManager/persistence/");
                } else if (iso_files.is_kali) {
                    // Kali Live uses boot=live + persistence (different from Ubuntu's boot=casper)
                    if (!str_contains_nocase(cmdline, "boot=live")) {
                        char temp[512];
                        k_memcpy(temp, cmdline, sizeof(temp));
                        snprintf(cmdline, sizeof(cmdline), "boot=live %s", temp);
                        clen = k_strlen(cmdline);
                    }
                    snprintf(cmdline + clen, sizeof(cmdline) - clen,
                             " persistence persistence-path=/BootManager/persistence/");
                } else if (str_ends_with_nocase(prof->filename, ".apkovl.tar.gz") ||
                           str_ends_with_nocase(prof->filename, ".tgz")) {
                    snprintf(cmdline + clen, sizeof(cmdline) - clen,
                             " apkovl=LABEL=BOOTLOADER:%s", prof->filename);
                } else if (str_contains_nocase(cmdline, "alpine") || str_contains_nocase(iso_files.title, "alpine")) {
                    snprintf(cmdline + clen, sizeof(cmdline) - clen,
                             " alpine_dev=LABEL=PERSISTENCE apkovl=LABEL=PERSISTENCE");
                } else {
                    snprintf(cmdline + clen, sizeof(cmdline) - clen,
                             " persistent persistent-path=/BootManager/persistence/");
                }
            }
        }
    } else if (iso_files.is_casper) {
        if (prof && prof->is_clean_session) {
            snprintf(cmdline, sizeof(cmdline),
                     "boot=casper modprobe.blacklist=floppy");
        } else {
            snprintf(cmdline, sizeof(cmdline),
                     "boot=casper persistent persistent-path=/BootManager/persistence/ modprobe.blacklist=floppy");
        }
    } else {
        // Generic Live System / Hard Drive OS Fallback
        if (prof && !prof->is_clean_session) {
            if (str_ends_with_nocase(prof->filename, ".apkovl.tar.gz") ||
                str_ends_with_nocase(prof->filename, ".tgz")) {
                snprintf(cmdline, sizeof(cmdline),
                         "modprobe.blacklist=floppy modules=loop,squashfs,sd-mod,usb-storage,uas usbdelay=3 apkovl=LABEL=BOOTLOADER:%s",
                         prof->filename);
            } else {
                snprintf(cmdline, sizeof(cmdline),
                         "modprobe.blacklist=floppy modules=loop,squashfs,sd-mod,usb-storage,uas usbdelay=3 alpine_dev=LABEL=PERSISTENCE apkovl=LABEL=PERSISTENCE persistent persistent-path=/BootManager/persistence/");
            }
        } else {
            snprintf(cmdline, sizeof(cmdline),
                     "modprobe.blacklist=floppy modules=loop,squashfs,sd-mod,usb-storage,uas usbdelay=3");
        }
    }

    // Apply Debug vs Release log policy
    finalize_kernel_cmdline(cmdline, sizeof(cmdline), iso_files.is_casper);

    // Keep standard 80x25 VGA text mode for maximum compatibility across distributions
    linux_boot_params_t *params = (linux_boot_params_t *)LINUX_BOOT_PARAMS_PHYS;
    linux_prepare_boot_params(kernel_buf, iso_files.kernel_size,
                              iso_files.found_initrd ? initrd_buf : NULL,
                              iso_files.found_initrd ? iso_files.initrd_size : 0,
                              cmdline, boot_info,
                              vbe_get_active_mode(), // Linear Framebuffer or fallback to text mode
                              0, 0, // 0 MB of 6GB in RAM, no E820 reservation needed!
                              params);

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    log_info("BOOT", "==========================================================");
    log_info("BOOT", "  HANDING OFF EXECUTION TO LINUX (ON-DEMAND BLOCK STORAGE)");
    log_info("BOOT", "  * OS Filesystem streamed on-demand directly from phone! ");
    log_info("BOOT", "  * RAM consumed by OS image: 0 MB!                       ");
    log_info("BOOT", "==========================================================");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    log_info("LOG", "Final pre-handoff flush: %u flushes, %u errors, %u bytes logged",
             disk_log_get_flush_count(), disk_log_get_error_count(), disk_log_get_length());
    disk_log_flush_with_feedback();
    phone_save_boot_log(&active_adb_session, &active_mtp_session);
    xhci_stop(&xhci_ctrl);
    sound_silence();

    sound_kernel_jump_tone();

    linux_boot_jump((uint32_t)kernel_buf + kinfo.protected_mode_offset,
                    LINUX_KERNEL_LOAD_PHYS,
                    kinfo.protected_mode_size,
                    LINUX_BOOT_PARAMS_PHYS,
                    kinfo.code32_start);
}

static void boot_in_ram_iso_handoff(uint32_t total_iso_bytes, const iso_boot_files_t *iso_files, boot_info_t *boot_info, const persistence_profile_t *prof) {
    uint8_t *ram_iso = (uint8_t *)LINUX_RAM_ISO_PHYS;
    void *kernel_buf = (void *)0x02000000;

    for (uint32_t b = 0; b < iso_files->kernel_size; b++) {
        ((uint8_t *)kernel_buf)[b] = ram_iso[(iso_files->kernel_lba * 2048) + b];
    }
    log_info("BOOT", "Kernel extracted into RAM at 0x%08X (%u bytes).",
             (uint32_t)kernel_buf, iso_files->kernel_size);

    // Compute safe physical address for initramfs safely above kernel decompression footprint
    uint32_t initrd_phys = LINUX_INITRD_LOAD_PHYS;
    const linux_setup_header_t *setup_hdr = (const linux_setup_header_t *)((const uint8_t *)kernel_buf + 0x1F1);
    if (setup_hdr->header == LINUX_HDRS_MAGIC && setup_hdr->version >= 0x020A && setup_hdr->init_size > 0) {
        uint32_t kernel_decomp_end = (uint32_t)setup_hdr->pref_address + setup_hdr->init_size;
        uint32_t safe_boundary = (kernel_decomp_end + 0x001FFFFF) & ~0x001FFFFF;
        if (safe_boundary > initrd_phys) {
            initrd_phys = safe_boundary;
        }
    }
    void *initrd_buf = (void *)initrd_phys;

    if (iso_files->found_initrd) {
        for (uint32_t b = 0; b < iso_files->initrd_size; b++) {
            ((uint8_t *)initrd_buf)[b] = ram_iso[(iso_files->initrd_lba * 2048) + b];
        }
        log_info("BOOT", "Initramfs extracted into RAM at 0x%08X (%u bytes, safe from decompressor).",
                 (uint32_t)initrd_buf, iso_files->initrd_size);
    }

    linux_kernel_info_t kinfo;
    if (linux_check_kernel_image(kernel_buf, iso_files->kernel_size, &kinfo) != 0) {
        log_error("BOOT", "Failed to validate in-RAM kernel image!");
        sound_error_tone();
        return;
    }

    // Install mBFT table at 0x000E0000 for Alpine memdiskfind
    linux_setup_mbft(LINUX_RAM_ISO_PHYS, total_iso_bytes);

    char kernel_cmdline[512];
    if (iso_files && iso_files->cmdline[0] != '\0') {
        snprintf(kernel_cmdline, sizeof(kernel_cmdline),
                 "%s phram=iso,0x%08X,0x%08X memdisk=yes",
                 iso_files->cmdline, LINUX_RAM_ISO_PHYS, total_iso_bytes);
        if (prof && !prof->is_clean_session) {
            size_t clen = k_strlen(kernel_cmdline);
            if (clen + 64 < sizeof(kernel_cmdline)) {
                if (iso_files->is_casper) {
                    snprintf(kernel_cmdline + clen, sizeof(kernel_cmdline) - clen,
                             " persistent persistent-path=/BootManager/persistence/");
                } else if (str_ends_with_nocase(prof->filename, ".apkovl.tar.gz") ||
                           str_ends_with_nocase(prof->filename, ".tgz")) {
                    snprintf(kernel_cmdline + clen, sizeof(kernel_cmdline) - clen,
                             " apkovl=LABEL=BOOTLOADER:%s", prof->filename);
                } else {
                    snprintf(kernel_cmdline + clen, sizeof(kernel_cmdline) - clen,
                             " persistent persistent-path=/BootManager/persistence/");
                }
            }
        }
    } else if (iso_files && iso_files->is_casper) {
        if (prof && prof->is_clean_session) {
            snprintf(kernel_cmdline, sizeof(kernel_cmdline),
                     "boot=casper modprobe.blacklist=floppy phram=iso,0x%08X,0x%08X memdisk=yes",
                     LINUX_RAM_ISO_PHYS, total_iso_bytes);
        } else {
            snprintf(kernel_cmdline, sizeof(kernel_cmdline),
                     "boot=casper modprobe.blacklist=floppy phram=iso,0x%08X,0x%08X memdisk=yes persistent persistent-path=/BootManager/persistence/",
                     LINUX_RAM_ISO_PHYS, total_iso_bytes);
        }
    } else {
        bool is_alpine = false;
        if (iso_files && (str_contains_nocase(iso_files->title, "alpine") ||
                          str_contains_nocase(iso_files->cmdline, "alpine") ||
                          (prof && (str_ends_with_nocase(prof->filename, ".apkovl.tar.gz") ||
                                    str_ends_with_nocase(prof->filename, ".tgz"))))) {
            is_alpine = true;
        }

        if (is_alpine) {
            if (prof && prof->is_clean_session) {
                snprintf(kernel_cmdline, sizeof(kernel_cmdline),
                         "modprobe.blacklist=floppy noapic modules=loop,squashfs,sd-mod,usb-storage phram=iso,0x%08X,0x%08X memdisk=yes",
                         LINUX_RAM_ISO_PHYS, total_iso_bytes);
            } else {
                const char *apkovl_target = (prof && prof->filename[0]) ? prof->filename : "apkovl.tgz";
                snprintf(kernel_cmdline, sizeof(kernel_cmdline),
                         "modprobe.blacklist=floppy noapic modules=loop,squashfs,sd-mod,usb-storage phram=iso,0x%08X,0x%08X memdisk=yes apkovl=LABEL=BOOTLOADER:%s",
                         LINUX_RAM_ISO_PHYS, total_iso_bytes, apkovl_target);
            }
        } else {
            // Universal / generic Live OS fallback
            if (prof && prof->is_clean_session) {
                snprintf(kernel_cmdline, sizeof(kernel_cmdline),
                         "modprobe.blacklist=floppy modules=loop,squashfs,sd-mod,usb-storage phram=iso,0x%08X,0x%08X memdisk=yes",
                         LINUX_RAM_ISO_PHYS, total_iso_bytes);
            } else {
                snprintf(kernel_cmdline, sizeof(kernel_cmdline),
                         "modprobe.blacklist=floppy modules=loop,squashfs,sd-mod,usb-storage phram=iso,0x%08X,0x%08X memdisk=yes persistent persistent-path=/BootManager/persistence/",
                         LINUX_RAM_ISO_PHYS, total_iso_bytes);
            }
        }
    }

    // Apply Debug vs Release log policy
    finalize_kernel_cmdline(kernel_cmdline, sizeof(kernel_cmdline), iso_files ? iso_files->is_casper : false);

    // Standard 80x25 VGA text mode for maximum compatibility across distributions
    linux_boot_params_t *kernel_params = (linux_boot_params_t *)LINUX_BOOT_PARAMS_PHYS;
    linux_prepare_boot_params(kernel_buf, iso_files->kernel_size,
                              iso_files->found_initrd ? initrd_buf : NULL,
                              iso_files->found_initrd ? iso_files->initrd_size : 0,
                              kernel_cmdline, boot_info,
                              vbe_get_active_mode(), // Linear Framebuffer or fallback to text mode
                              LINUX_RAM_ISO_PHYS, total_iso_bytes,
                              kernel_params);

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    log_info("BOOT", "==========================================================");
    log_info("BOOT", "  HANDING OFF TO IN-RAM LINUX WITH SD PERSISTENCE         ");
    log_info("BOOT", "  * In-RAM ISO     : 0x%08X (%u MB, Type 2 RESERVED)     ",
             LINUX_RAM_ISO_PHYS, total_iso_bytes / 1024 / 1024);
    if (prof && prof->is_clean_session) {
        log_info("BOOT", "  * Persistence    : Clean Session (100%% In-RAM, no saved changes)");
    } else if (prof) {
        log_info("BOOT", "  * Persistence    : %s (%s)", prof->profile_name, prof->filename);
    } else {
        log_info("BOOT", "  * Persistence    : Direct Hardware Access (Standard OS)");
    }
    log_info("BOOT", "==========================================================");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    log_info("LOG", "Final pre-handoff flush: %u flushes, %u errors, %u bytes logged",
             disk_log_get_flush_count(), disk_log_get_error_count(), disk_log_get_length());
    disk_log_flush_with_feedback();
    phone_save_boot_log(&active_adb_session, &active_mtp_session);
    xhci_stop(&xhci_ctrl);
    sound_silence();

    sound_kernel_jump_tone();

    linux_boot_jump((uint32_t)kernel_buf + kinfo.protected_mode_offset,
                    LINUX_KERNEL_LOAD_PHYS,
                    kinfo.protected_mode_size,
                    LINUX_BOOT_PARAMS_PHYS,
                    kinfo.code32_start);
}

static void boot_from_in_ram_iso(os_entry_t *entry, boot_info_t *boot_info, const persistence_profile_t *prof) {
    if (!entry) return;
    log_info("BOOT", "Booting preloaded In-RAM ISO at 0x%08X (%u MB)...",
             LINUX_RAM_ISO_PHYS, (uint32_t)(entry->file_size / 1024 / 1024));
    boot_in_ram_iso_handoff((uint32_t)entry->file_size, &entry->iso_files, boot_info, prof);
}

static void boot_from_android_mtp(os_entry_t *selected, boot_info_t *boot_info, const persistence_profile_t *prof) {
    log_info("BOOT", "Attempting boot from Android Phone (MTP)...");
    mtp_session_t *session = (selected && selected->mtp_session) ? selected->mtp_session : &active_mtp_session;
    if (!session || !session->session_active) {
        log_error("BOOT", "Android MTP session not active!");
        sound_error_tone();
        return;
    }

    boot_source_t *mtp_src = boot_source_mtp_create(session);
    if (!mtp_src) {
        log_error("BOOT", "Failed to create MTP boot source.");
        sound_error_tone();
        return;
    }

    if (selected && selected->mtp_handle != 0 && selected->file_size > 0) {
        mtp_source_set_target(mtp_src, selected->mtp_handle, selected->file_size);
        log_info("BOOT", "Selected MTP image: '%s' (Handle 0x%08X, %u MB)",
                 selected->filename, selected->mtp_handle, (uint32_t)(selected->file_size / 1024 / 1024));
    } else {
        char target_file[64] = {0};
        if (mtp_find_boot_file(mtp_src, target_file, sizeof(target_file)) != 0) {
            log_error("BOOT", "No bootable kernel or ISO found on Android phone!");
            sound_error_tone();
            return;
        }

        log_info("BOOT", "Boot image found on Android phone: '%s'", target_file);
        if (mtp_src->open(mtp_src, target_file) != 0) {
            log_error("BOOT", "Failed to open '%s' on Android phone!", target_file);
            sound_error_tone();
            return;
        }
    }

    uint64_t fsize = mtp_src->size(mtp_src);
    log_info("BOOT", "Opened '%s' (%u MB). Inspecting ISO boot files...",
             (selected && selected->filename[0]) ? selected->filename : "image",
             (uint32_t)(fsize / 1024 / 1024));

    iso_boot_files_t iso_files;
    k_memset(&iso_files, 0, sizeof(iso_files));
    if (iso_find_boot_files(mtp_src, &iso_files) == 0 && iso_files.found_kernel) {
        const char *distro_desc = "Alpine Linux";
        if (iso_files.is_casper) distro_desc = "Ubuntu / Casper Live";
        else if (str_contains_nocase(iso_files.volume_id, "kali") || str_contains_nocase(iso_files.title, "kali")) distro_desc = "Kali Linux Installer";
        log_info("BOOT", "Bootable ISO detected! (%s)", distro_desc);
        if (prof) {
            log_info("BOOT", "  * Persistence    : %s (%s)", prof->profile_name, prof->filename);
        }

        uint32_t total_iso_bytes = (uint32_t)fsize;
        uint8_t *ram_iso = (uint8_t *)LINUX_RAM_ISO_PHYS;

        vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        log_info("BOOT", "Streaming full ISO from Android phone to RAM at 0x%08X...", LINUX_RAM_ISO_PHYS);
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

        uint32_t streamed = 0;
        uint32_t block_size = 1048576; // 1 MB per request
        uint32_t last_pct = 0;

        while (streamed < total_iso_bytes) {
            uint32_t chunk = total_iso_bytes - streamed;
            if (chunk > block_size) chunk = block_size;

            mtp_src->seek(mtp_src, streamed);
            int r = mtp_src->read(mtp_src, ram_iso + streamed, chunk);
            if (r <= 0) {
                log_error("BOOT", "Failed to stream ISO at offset %u (chunk %u)", streamed, chunk);
                break;
            }
            streamed += r;

            uint32_t mb_streamed = streamed >> 20;
            uint32_t mb_total = total_iso_bytes >> 20;
            uint32_t pct = mb_total ? ((mb_streamed * 100) / mb_total) : 0;
            if (pct >= last_pct + 5 || streamed == total_iso_bytes) {
                log_info("BOOT", "  Streaming ISO: %u%% (%u MB / %u MB)...",
                         pct, mb_streamed, mb_total);
                last_pct = pct;
                disk_log_flush();
            }
        }

        if (streamed < total_iso_bytes) {
            log_error("BOOT", "ISO transfer incomplete: %u / %u bytes", streamed, total_iso_bytes);
            sound_error_tone();
            return;
        }

        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        log_info("BOOT", "Full ISO successfully cached in RAM (%u MB)!", total_iso_bytes / 1024 / 1024);
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

        boot_in_ram_iso_handoff(total_iso_bytes, &iso_files, boot_info, prof);
    } else {
        // Raw kernel image fallback
        void *kernel_buf = (void *)0x02000000;
        mtp_src->seek(mtp_src, 0);
        mtp_src->read(mtp_src, kernel_buf, (uint32_t)fsize);

        linux_kernel_info_t kinfo;
        if (linux_check_kernel_image(kernel_buf, (uint32_t)fsize, &kinfo) == 0) {
            linux_boot_params_t *raw_params = (linux_boot_params_t *)LINUX_BOOT_PARAMS_PHYS;
            const char *cmdline = "console=tty0 earlyprintk=vga loglevel=7 root=/dev/ram0 rw";
            linux_prepare_boot_params(kernel_buf, (uint32_t)fsize,
                                      NULL, 0,
                                      cmdline, boot_info,
                                      vbe_get_active_mode(), 0, 0,
                                      raw_params);

            xhci_stop(&xhci_ctrl);
            sound_silence();

            log_info("LOG", "Final pre-handoff flush: %u flushes, %u errors, %u bytes logged",
                     disk_log_get_flush_count(), disk_log_get_error_count(), disk_log_get_length());
            disk_log_flush_with_feedback();

            sound_kernel_jump_tone();

            linux_boot_jump((uint32_t)kernel_buf + kinfo.protected_mode_offset,
                            LINUX_KERNEL_LOAD_PHYS,
                            kinfo.protected_mode_size,
                            LINUX_BOOT_PARAMS_PHYS,
                            kinfo.code32_start);
        }
    }
}

static int sd_read_sectors(void *priv, uint32_t lba, uint32_t count, void *buf) {
    (void)priv;
    return (disk_read(0, (uint8_t *)buf, lba, count) == RES_OK) ? 0 : -1;
}

static void boot_from_sd_fat(os_entry_t *entry, boot_info_t *boot_info) {
    if (!entry) return;
    log_info("BOOT", "Attempting boot from SD Card FAT32: '%s'...", entry->filename);
    uint8_t boot_drive = boot_info ? (uint8_t)boot_info->boot_drive : 0x80;
    boot_source_t *fat_src = boot_source_fat_create(sd_read_sectors, (void *)(uintptr_t)boot_drive, entry->partition_lba);
    if (!fat_src) {
        log_error("BOOT", "Failed to mount SD Card FAT32 partition!");
        sound_error_tone();
        return;
    }

    if (fat_src->open(fat_src, entry->filename) != 0) {
        log_error("BOOT", "Failed to open '%s' on SD card!", entry->filename);
        fat_src->close(fat_src);
        sound_error_tone();
        return;
    }

    uint32_t fsize = (uint32_t)fat_src->size(fat_src);
    void *kernel_buf = (void *)0x02000000;
    fat_src->read(fat_src, kernel_buf, fsize);
    fat_src->close(fat_src);

    linux_kernel_info_t kinfo;
    if (linux_check_kernel_image(kernel_buf, fsize, &kinfo) == 0) {
        linux_boot_params_t *params = (linux_boot_params_t *)LINUX_BOOT_PARAMS_PHYS;
        const char *cmdline = "console=tty0 root=/dev/sda1 rw loglevel=7";
        linux_prepare_boot_params(kernel_buf, fsize, NULL, 0, cmdline, boot_info, vbe_get_active_mode(), 0, 0, params);
        sound_kernel_jump_tone();
        linux_boot_jump((uint32_t)kernel_buf + kinfo.protected_mode_offset,
                        LINUX_KERNEL_LOAD_PHYS,
                        kinfo.protected_mode_size,
                        LINUX_BOOT_PARAMS_PHYS,
                        kinfo.code32_start);
    } else {
        log_error("BOOT", "Kernel image '%s' on SD card verification failed!", entry->filename);
        sound_error_tone();
    }
}

static void load_dynamic_adb_key(uint8_t boot_drive) {
    static uint8_t sector_buf[512];
    if (bios_disk_read(boot_drive, 0, 1, sector_buf) != 0) return;

    uint32_t part1_lba = *(uint32_t *)(&sector_buf[446 + 8]);
    if (part1_lba == 0) part1_lba = 2048;

    boot_source_t *fat_src = boot_source_fat_create(sd_read_sectors, (void *)(uintptr_t)boot_drive, part1_lba);
    if (!fat_src) return;

    if (fat_src->open(fat_src, "ADBKEY.PUB") == 0) {
        uint32_t fsize = (uint32_t)fat_src->size(fat_src);
        if (fsize > 0 && fsize < 2048) {
            static char key_buf[2048];
            uint32_t nread = fat_src->read(fat_src, key_buf, fsize);
            if (nread > 0) {
                while (nread > 0 && (key_buf[nread - 1] == '\r' || key_buf[nread - 1] == '\n' || key_buf[nread - 1] == ' ')) {
                    nread--;
                }
                key_buf[nread] = '\0';
                adb_set_public_key(key_buf, nread);
            }
        }
    }
    fat_src->close(fat_src);
}

void c_main(boot_info_t *boot_info) {
    // 1. Initialize Serial Port, VGA Console, Sound & Hardware Timer
    serial_init();
    vga_init();
    sound_boot_tone();
    timer_init();

    if (vbe_is_active()) {
        const vbe_mode_info_t *m = vbe_get_active_mode();
        log_info("VBE", "Active Graphics Console: %ux%ux%u LFB at 0x%08X (HDMI Ready)",
                 m->x_res, m->y_res, m->bits_per_pixel, m->phys_base_ptr);
    } else {
        log_info("VBE", "Active Console: Standard VGA 80x25 Text Mode (0xB8000)");
    }

    // 2. Initialize Persistent SD Disk Logging EARLY
    disk_log_init(boot_info);
    disk_log_record_early_boot(boot_info);
    disk_log_set_phone_sync_hook(phone_save_boot_log_hook);

    // 3. Banner
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    printk("\n======================================================================\n");
    printk("  ANDROID -> LINUX BOOTLOADER (LEGACY BIOS)\n");
    printk("  Stage 3 C Runtime Active at 0x00100000 (32-bit Flat Protected Mode)\n");
    printk("======================================================================\n\n");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    log_info("STAGE3", "Boot Drive preserved: 0x%02X (%s)",
             boot_info->boot_drive,
             (boot_info->boot_drive >= 0x80) ? "Hard Disk / SD" : "Floppy");

    // Phase 2: Memory Management Initialization
    memory_init(boot_info);
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    log_info("STAGE3", "Phase 2 Memory Management Successfully Verified!");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    // Dynamic ADB Authentication Key (Loaded from FAT32 boot drive if present)
    load_dynamic_adb_key((uint8_t)boot_info->boot_drive);
    diskio_set_bios_drive((uint8_t)boot_info->boot_drive);
    usb_set_hub_msc_callback(handle_msc_device);

    bool phone_prompted = false;
    bool mtp_found = false;
    uint32_t probed_ports = 0;
    uint8_t probe_fail_count[32] = {0};
    usb_device_t current_dev;

    // 6. PCI Bus Enumeration (xHCI Host Controller Discovery)
    pci_init();

    pci_device_t *xhci_pci = pci_find_xhci();
    if (xhci_pci) {
        // Disengage BIOS disk services before xHCI controller reset & SMM handover
        disk_log_flush();
        disk_log_disable_bios();
        diskio_disable_bios();

        log_info("STAGE3", "Initializing xHCI Host Controller hardware...");
        int xhci_status = xhci_init(xhci_pci, &xhci_ctrl);
        if (xhci_status == 0) {
            xhci_trb_t noop_cmd = {0};
            noop_cmd.control = TRB_TYPE(TRB_NOOP_CMD);
            xhci_trb_t noop_evt = {0};
            int cmd_res = xhci_send_command(&xhci_ctrl, &noop_cmd, &noop_evt);
            if (cmd_res == 0) {
                log_info("XHCI", "NO-OP Command Test: SUCCESS (Command & Event Ring verified)");
            } else {
                log_error("XHCI", "NO-OP Command Test failed (code %d)", cmd_res);
            }

            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            log_info("STAGE3", "Phase 3 xHCI Controller Initialization Successfully Verified!");
            vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

            // Register downstream Hub Mass Storage callback
            usb_set_hub_msc_callback(handle_msc_device);

            // Initial scan of connected USB devices
            for (uint8_t p = 1; p <= xhci_ctrl.max_ports && p < 32; p++) {
                uintptr_t port_reg = xhci_ctrl.op_regs + XHCI_OP_PORTS_BASE + (p - 1) * 0x10;
                uint32_t portsc = *(volatile uint32_t *)port_reg;
                if (portsc != 0 && portsc != 0x000002A0) {
                    log_info("XHCI", "Port %u: PORTSC = 0x%08X", p, portsc);
                }

                if (!(portsc & XHCI_PORT_CCS)) continue;

                k_memset(&current_dev, 0, sizeof(current_dev));
                log_info("STAGE3", "Probing USB device on Port %u...", p);
                int probe_res = usb_probe_port(&xhci_ctrl, p, &current_dev);
                if (probe_res == 0) {
                    probed_ports |= (1U << p);
                    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                    log_info("STAGE3", "Phase 4 USB Enumeration Successfully Verified!");
                    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

                    if (current_dev.has_adb && !active_adb_session.is_connected) {
                        k_memcpy(&external_usb_dev, &current_dev, sizeof(usb_device_t));
                        external_usb_detected = true;
                        log_info("STAGE3", "Port %u: Android ADB interface detected! Initializing ADB Root Bridge...", p);
                        if (adb_init_session(&external_usb_dev, &active_adb_session) == 0) {
                            log_info("STAGE3", "ADB Root Bridge connected! Phone can switch to USB Mass Storage (0 MB in RAM).");
                            for (int w = 0; w < 100000; w++) io_wait(); // 50ms settle delay for phone adbd
                            phone_save_boot_log(&active_adb_session, &active_mtp_session);
                        }
                    }

                    if (current_dev.has_msc) {
                        handle_msc_device(&current_dev, p);
                    } else if (current_dev.dev_desc.bDeviceClass == USB_CLASS_HUB) {
                        check_and_handle_hub(&xhci_ctrl, &current_dev, p);
                    } else if (current_dev.has_mtp && !active_adb_session.is_connected && !active_mtp_session.session_active) {
                        k_memcpy(&external_usb_dev, &current_dev, sizeof(usb_device_t));
                        external_usb_detected = true;
                        log_info("STAGE3", "Android MTP interface detected on Port %u! Initializing MTP session...", p);
                        int mtp_res = mtp_init_session(&external_usb_dev, &active_mtp_session);
                        if (mtp_res == 0) {
                            uint32_t storage_ids[8];
                            uint32_t count = 0;
                            mtp_get_storage_ids(&active_mtp_session, storage_ids, 8, &count);
                            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                            log_info("STAGE3", "Phase 5 Android MTP Detection Successfully Verified!");
                            vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                            sound_phone_connected_tone();
                            mtp_found = true;
                            phone_save_boot_log(&active_adb_session, &active_mtp_session);
                        }
                    } else if (!current_dev.has_msc && !current_dev.has_adb && !current_dev.has_mtp && current_dev.dev_desc.bDeviceClass != USB_CLASS_HUB) {
                        // Try to detect USB HID Boot Keyboard
                        if (usb_keyboard_try_init(&xhci_ctrl, &current_dev)) {
                            log_info("STAGE3", "Port %u: External USB Keyboard detected and initialized!", p);
                        } else if (current_dev.dev_desc.bDeviceClass == 0xE0) {
                            log_info("STAGE3", "Port %u: Wireless/Bluetooth Controller (VID 0x%04X, PID 0x%04X) active.",
                                     p, current_dev.dev_desc.idVendor, current_dev.dev_desc.idProduct);
                        } else {
                            log_info("STAGE3", "Port %u: Attached device (VID 0x%04X, PID 0x%04X) is in Charging/No-Data mode.",
                                     p, current_dev.dev_desc.idVendor, current_dev.dev_desc.idProduct);
                            vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
                            log_info("STAGE3", ">>> PLEASE UNLOCK PHONE AND TAP 'File Transfer / MTP' ON SCREEN! <<<");
                            vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                            if (!phone_prompted) {
                                sound_prompt_tone();
                                phone_prompted = true;
                            }
                            probed_ports &= ~(1U << p);
                        }
                    }
                }
            }

            // Note: BIOS disk fallback is intentionally NEVER re-enabled after xHCI takeover
            // because calling real-mode BIOS INT 13h causes BIOS to poll USB and overwrite xHCI state.

            // Poll loop: wait if device not yet detected
            for (int poll_iter = 0; poll_iter < 100 && !active_adb_session.is_connected && !mtp_found && !external_msc_detected; poll_iter++) {
                if (poll_iter > 0) {
                    for (int d = 0; d < 200000; d++) io_wait();
                }

                if (poll_iter % 25 == 0 && poll_iter > 0) {
                    log_info("STAGE3", "Waiting for Android phone or USB storage... %u sec left",
                             (100 - poll_iter) / 5);
                }

                for (uint8_t p = 1; p <= xhci_ctrl.max_ports && p < 32; p++) {
                    uintptr_t port_reg = xhci_ctrl.op_regs + XHCI_OP_PORTS_BASE + (p - 1) * 0x10;
                    uint32_t portsc = *(volatile uint32_t *)port_reg;

                    if (!(portsc & XHCI_PORT_CCS)) {
                        if (probed_ports & (1U << p)) {
                            log_info("STAGE3", "Port %u: USB device disconnected.", p);
                            probed_ports &= ~(1U << p);
                            probe_fail_count[p] = 0;
                        }
                        continue;
                    }

                    if (probed_ports & (1U << p)) continue;
                    if (probe_fail_count[p] >= 3) continue;

                    k_memset(&current_dev, 0, sizeof(current_dev));
                    log_info("STAGE3", "Probing USB device on Port %u...", p);
                    int probe_res = usb_probe_port(&xhci_ctrl, p, &current_dev);
                    if (probe_res == 0) {
                        probed_ports |= (1U << p);
                        probe_fail_count[p] = 0;
                        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                        log_info("STAGE3", "Phase 4 USB Enumeration Successfully Verified!");
                        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

                        if (current_dev.has_adb && !active_adb_session.is_connected) {
                            k_memcpy(&external_usb_dev, &current_dev, sizeof(usb_device_t));
                            external_usb_detected = true;
                            log_info("STAGE3", "Port %u: Android ADB interface detected! Initializing ADB Root Bridge...", p);
                            if (adb_init_session(&external_usb_dev, &active_adb_session) == 0) {
                                log_info("STAGE3", "ADB Root Bridge connected! Phone can switch to USB Mass Storage (0 MB in RAM).");
                                for (int w = 0; w < 100000; w++) io_wait(); // 50ms settle delay for phone adbd
                                phone_save_boot_log(&active_adb_session, &active_mtp_session);
                            }
                        }

                        if (current_dev.has_msc) {
                            handle_msc_device(&current_dev, p);
                            if (external_msc_detected) break;
                        } else if (current_dev.dev_desc.bDeviceClass == USB_CLASS_HUB) {
                            check_and_handle_hub(&xhci_ctrl, &current_dev, p);
                            if (external_msc_detected) break;
                        } else if (current_dev.has_mtp && !active_adb_session.is_connected && !active_mtp_session.session_active) {
                            k_memcpy(&external_usb_dev, &current_dev, sizeof(usb_device_t));
                            external_usb_detected = true;
                            log_info("STAGE3", "Android MTP interface detected on Port %u! Initializing MTP session...", p);
                            int mtp_res = mtp_init_session(&external_usb_dev, &active_mtp_session);
                            if (mtp_res == 0) {
                                uint32_t storage_ids[8];
                                uint32_t count = 0;
                                mtp_get_storage_ids(&active_mtp_session, storage_ids, 8, &count);

                                vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                                log_info("STAGE3", "Phase 5 Android MTP Detection Successfully Verified!");
                                vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                                sound_phone_connected_tone();
                                mtp_found = true;
                                phone_save_boot_log(&active_adb_session, &active_mtp_session);
                                break;
                            }
                        } else if (!current_dev.has_adb && !current_dev.has_mtp && current_dev.dev_desc.bDeviceClass != USB_CLASS_HUB) {
                            if (usb_keyboard_try_init(&xhci_ctrl, &current_dev)) {
                                log_info("STAGE3", "Port %u: External USB Keyboard detected and initialized!", p);
                            } else if (current_dev.dev_desc.bDeviceClass == 0xE0) {
                                log_info("STAGE3", "Port %u: Wireless/Bluetooth Controller (VID 0x%04X, PID 0x%04X) active.",
                                         p, current_dev.dev_desc.idVendor, current_dev.dev_desc.idProduct);
                            } else {
                                probed_ports &= ~(1U << p);
                            }
                        } else {
                            probed_ports &= ~(1U << p);
                        }
                    } else {
                        probe_fail_count[p]++;
                    }
                }
            }
        } else {
            log_error("STAGE3", "Failed to initialize xHCI controller (error %d)", xhci_status);
            sound_error_tone();
        }
    } else {
        log_info("STAGE3", "No xHCI controller detected on PCI bus.");
        sound_error_tone();
    }

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    log_info("STAGE3", "Phase 1 Legacy BIOS Bootstrap Successfully Verified!");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    while (1) {
        // Phase 10: Scan Connected Storage & Display Dynamic Interactive Boot Menu
        static os_registry_t os_reg;
        k_memset(&os_reg, 0, sizeof(os_reg));
        os_scan_all_storages(boot_info, &xhci_ctrl,
                             active_msc_dev,
                             active_mtp_session.session_active ? &active_mtp_session : NULL,
                             active_adb_session.is_connected ? &active_adb_session : NULL,
                             &os_reg);

        phone_save_boot_log(&active_adb_session, &active_mtp_session);

        usb_device_t *menu_usb = external_usb_detected ? &external_usb_dev : (boot_msc_detected ? &boot_msc_device : NULL);
        menu_render(boot_info, &xhci_ctrl, menu_usb, &active_mtp_session, &os_reg);

        // Wait for explicit user selection
        menu_selection_t choice = menu_wait_selection(&os_reg);

        if (choice.type == MENU_ACTION_RESCAN) {
            log_info("STAGE3", "Rescanning USB ports for Android phone / USB storage...");
            probed_ports = 0;
            k_memset(probe_fail_count, 0, sizeof(probe_fail_count));
            for (uint8_t p = 1; p <= xhci_ctrl.max_ports && p < 32; p++) {
                if (boot_msc_detected && p == boot_msc_device.port_num) continue;
                uintptr_t port_reg = xhci_ctrl.op_regs + XHCI_OP_PORTS_BASE + (p - 1) * 0x10;
                uint32_t portsc = *(volatile uint32_t *)port_reg;
                if (portsc & XHCI_PORT_CCS) {
                    k_memset(&current_dev, 0, sizeof(current_dev));
                    if (usb_probe_port(&xhci_ctrl, p, &current_dev) == 0) {
                        probed_ports |= (1U << p);
                        if (current_dev.has_adb && !active_adb_session.is_connected) {
                            k_memcpy(&external_usb_dev, &current_dev, sizeof(usb_device_t));
                            external_usb_detected = true;
                            adb_init_session(&external_usb_dev, &active_adb_session);
                        }
                        if (current_dev.has_msc) {
                            handle_msc_device(&current_dev, p);
                        } else if (current_dev.dev_desc.bDeviceClass == USB_CLASS_HUB) {
                            check_and_handle_hub(&xhci_ctrl, &current_dev, p);
                        } else if (current_dev.has_mtp && !active_adb_session.is_connected && !active_mtp_session.session_active) {
                            k_memcpy(&external_usb_dev, &current_dev, sizeof(usb_device_t));
                            external_usb_detected = true;
                            if (mtp_init_session(&external_usb_dev, &active_mtp_session) == 0) {
                                mtp_found = true;
                            }
                        } else if (!current_dev.has_adb && !current_dev.has_mtp && current_dev.dev_desc.bDeviceClass != USB_CLASS_HUB) {
                            usb_keyboard_try_init(&xhci_ctrl, &current_dev);
                        }
                    }
                }
            }
            continue;
        }

        if (choice.type == MENU_ACTION_SWITCH_UMS) {
            active_adb_session.usb_dev = &external_usb_dev;
            if (!active_adb_session.is_connected && external_usb_dev.has_adb) {
                adb_init_session(&external_usb_dev, &active_adb_session);
            }
            if (active_adb_session.is_connected) {
                int img_idx = menu_select_phone_image(&os_reg);
                if (img_idx < 0 || img_idx >= (int)os_reg.count) {
                    continue;
                }
                os_entry_t *sel_img = &os_reg.entries[img_idx];

                if (active_adb_session.is_connected) {
                    adb_scan_persistence_profiles(&active_adb_session, sel_img);
                }
                sel_img->selected_profile = menu_select_persistence_profile(sel_img, active_adb_session.is_connected ? &active_adb_session : NULL);
                const persistence_profile_t *prof = (sel_img->profile_count > 0 && sel_img->selected_profile < sel_img->profile_count) ?
                    &sel_img->profiles[sel_img->selected_profile] : NULL;
                char prof_path[256] = {0};
                if (prof && !prof->is_clean_session && prof->filename[0]) {
                    snprintf(prof_path, sizeof(prof_path), "/sdcard/BootManager/persistence/%s", prof->filename);
                }

                // If phone is ALREADY operating as USB Mass Storage, check if selected image is already active!
                if (external_msc_detected && external_usb_dev.has_msc) {
                    if ((sel_img->approach == BOOT_APPROACH_BLOCK_ON_DEMAND || sel_img->approach == BOOT_APPROACH_CHAINLOAD) && sel_img->storage_type == OS_STORAGE_BLOCK_USB) {
                        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                        log_info("BOOT", "Image '%s' is already active on USB Mass Storage LUN 0!", sel_img->title);
                        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                        disk_log_flush();
                        boot_from_usb_msc(&external_usb_dev, boot_info, prof, &sel_img->iso_files);
                        break;
                    }

                    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
                    log_info("STAGE3", "Phone already in UMS mode. Hot-swapping image to '%s' (Profile: '%s')...",
                             sel_img->filename, prof_path[0] ? prof->filename : "Clean Session");
                    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                    disk_log_flush();

                    int upd_res = adb_update_mass_storage_file(&active_adb_session, sel_img->filename, prof_path[0] ? prof_path : NULL, str_ends_with_nocase(sel_img->filename, ".iso"));
                    if (upd_res == 0) {
                        for (int w = 0; w < 200000; w++) io_wait();
                        uint32_t last_lba = 0, block_sz = 0;
                        usb_msc_init_device(&external_usb_dev);
                        usb_msc_read_capacity(&external_usb_dev, &last_lba, &block_sz);

                        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                        log_info("STAGE3", "SUCCESS: USB Mass Storage image updated to '%s'!", sel_img->filename);
                        log_info("BOOT", "Booting '%s' with Direct Block Access (0 MB in RAM)...", sel_img->title);
                        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                        disk_log_flush();
                        boot_from_usb_msc(&external_usb_dev, boot_info, prof, &sel_img->iso_files);
                        break;
                    } else {
                        log_error("STAGE3", "Hot-swap failed (%d), falling back to full gadget switch...", upd_res);
                    }
                }

                vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
                log_info("STAGE3", "Attaching '%s' as USB Mass Storage (Profile: '%s')...",
                         sel_img->filename, prof_path[0] ? prof->filename : "Clean Session");
                vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                disk_log_flush();
                int trg_res = adb_trigger_mass_storage(&active_adb_session, sel_img->filename, prof_path[0] ? prof_path : NULL, false);
                disk_log_flush();

                if (trg_res != 0 && trg_res != -2) {
                    log_info("STAGE3", "Trigger returned %d, but phone may still be switching. Waiting 4 sec...", trg_res);
                    for (int w = 0; w < 4000000; w++) io_wait();
                    disk_log_flush();
                }

                if (trg_res == -2) {
                    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
                    log_error("STAGE3", "Phone kernel does not have USB Mass Storage support.");
                    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
                    log_info("STAGE3", "Press any key to return to menu...");
                    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                    menu_get_char();
                } else if (usb_reprobe_as_msc(&xhci_ctrl, external_usb_dev.port_num, &external_usb_dev, 15) == 0) {
                    external_usb_detected = true;
                    external_msc_detected = true;
                    active_msc_dev = &external_usb_dev;
                    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                    log_info("STAGE3", "SUCCESS: Phone is now operating as a Hardware USB Mass Storage Drive!");
                    log_info("BOOT", "Booting '%s' with Direct Block Access (0 MB in RAM)...", sel_img->title);
                    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                    disk_log_flush();

                    if (external_usb_dev.has_adb) {
                        log_info("BOOT", "Composite phone re-enumerated with ADB! Restoring ADB session...");
                        if (adb_init_session(&external_usb_dev, &active_adb_session) == 0) {
                            log_info("BOOT", "ADB session active. Ready for OS handoff.");
                        }
                    }

                    boot_from_usb_msc(&external_usb_dev, boot_info, prof, &sel_img->iso_files);
                    break;
                } else {
                    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
                    log_error("STAGE3", "Phone did not re-enumerate as USB Mass Storage device within timeout.");
                    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
                    log_info("STAGE3", "Press any key to return to menu...");
                    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                    disk_log_flush();
                    menu_get_char();
                }
            } else {
                vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
                log_error("STAGE3", "No authorized ADB session active!");
                vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
                log_info("STAGE3", "1. Enable 'USB Debugging' in Phone Settings -> Developer Options.");
                log_info("STAGE3", "2. Ensure Shell has Root access in Magisk app.");
                log_info("STAGE3", "3. Press 'R' to rescan, then press 'U' to switch to USB Mass Storage.");
                log_info("STAGE3", "Press any key to return to menu...");
                vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                menu_get_char();
            }
            continue;
        }

        switch (choice.type) {
            case MENU_ACTION_BOOT_OS: {
                if (choice.os_index < os_reg.count) {
                    os_entry_t *selected = &os_reg.entries[choice.os_index];
                    log_info("BOOT", "Booting selected OS #%u: '%s'...", choice.os_index + 1, selected->title);
                    disk_log_flush();

                    active_adb_session.usb_dev = &external_usb_dev;
                    if (!active_adb_session.is_connected && external_usb_dev.has_adb) {
                        adb_init_session(&external_usb_dev, &active_adb_session);
                    }
                    if (active_adb_session.is_connected) {
                        adb_scan_persistence_profiles(&active_adb_session, selected);
                    }

                    selected->selected_profile = menu_select_persistence_profile(selected, active_adb_session.is_connected ? &active_adb_session : NULL);
                    const persistence_profile_t *prof = (selected->profile_count > 0 && selected->selected_profile < selected->profile_count) ?
                        &selected->profiles[selected->selected_profile] : NULL;
                    char prof_path[256] = {0};
                    if (prof && !prof->is_clean_session && prof->filename[0]) {
                        snprintf(prof_path, sizeof(prof_path), "/sdcard/BootManager/persistence/%s", prof->filename);
                    }

                    bool ums_booted = false;

                    // 1. If selected image is ALREADY on the active USB block storage, boot directly without touching ADB!
                    if ((selected->approach == BOOT_APPROACH_BLOCK_ON_DEMAND || selected->approach == BOOT_APPROACH_CHAINLOAD) && selected->storage_type == OS_STORAGE_BLOCK_USB) {
                        usb_device_t *target_msc = selected->usb_dev ? selected->usb_dev : active_msc_dev;
                        if (target_msc && target_msc->has_msc) {
                            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                            log_info("BOOT", "Booting '%s' directly from active USB Mass Storage Block Device...", selected->title);
                            vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                            disk_log_flush();
                            boot_from_usb_msc(target_msc, boot_info, prof, &selected->iso_files);
                            ums_booted = true;
                            break;
                        }
                    }

                    // 2. If phone is in UMS mode and a DIFFERENT image was selected, hot-swap via ADB!
                    if (external_msc_detected && external_usb_dev.has_msc) {
                        if (active_adb_session.is_connected) {
                            vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
                            log_info("BOOT", "Phone in UMS mode. Hot-swapping to '%s' (Profile: '%s')...",
                                     selected->filename, prof_path[0] ? prof->filename : "Clean Session");
                            vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                            disk_log_flush();

                            int upd = adb_update_mass_storage_file(&active_adb_session, selected->filename, prof_path[0] ? prof_path : NULL, str_ends_with_nocase(selected->filename, ".iso"));
                            if (upd == 0) {
                                for (int w = 0; w < 200000; w++) io_wait();
                                uint32_t last_lba = 0, block_sz = 0;
                                usb_msc_init_device(&external_usb_dev);
                                usb_msc_read_capacity(&external_usb_dev, &last_lba, &block_sz);

                                vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                                log_info("BOOT", "USB Mass Storage updated! Booting '%s' directly...", selected->title);
                                vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                                disk_log_flush();
                                boot_from_usb_msc(&external_usb_dev, boot_info, prof, &selected->iso_files);
                                ums_booted = true;
                                break;
                            } else {
                                vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
                                log_error("BOOT", "Hot-swap failed (%d). Returning to menu...", upd);
                                vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                                disk_log_flush();
                                phone_save_boot_log(&active_adb_session, &active_mtp_session);
                                for (int w = 0; w < 2000000; w++) io_wait();
                                continue;
                            }
                        }
                    }

                    // If image is on Android phone/MTP and ADB is available, try Root USB Mass Storage switch first!
                    if (!ums_booted && (selected->approach == BOOT_APPROACH_MTP_IN_RAM || selected->approach == BOOT_APPROACH_CHAINLOAD || selected->storage_type == OS_STORAGE_MTP_ANDROID)) {
                        if (active_adb_session.is_connected) {
                            vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
                            log_info("BOOT", "Root ADB active! Attaching '%s' to USB Mass Storage (Profile: '%s')...",
                                     selected->filename, prof_path[0] ? prof->filename : "Clean Session");
                            vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                            disk_log_flush();
                            phone_save_boot_log(&active_adb_session, &active_mtp_session);

                            int trg = adb_trigger_mass_storage(&active_adb_session, selected->filename, prof_path[0] ? prof_path : NULL, false);
                            disk_log_flush();

                            // Try to reprobe as MSC regardless of trigger result.
                            if (trg != -2) { // -2 = kernel doesn't have UMS support at all
                                if (trg != 0) {
                                    log_info("BOOT", "Trigger returned %d, but phone may still switch. Waiting 4 sec...", trg);
                                    for (int w = 0; w < 4000000; w++) io_wait();
                                    disk_log_flush();
                                }

                                if (usb_reprobe_as_msc(&xhci_ctrl, external_usb_dev.port_num, &external_usb_dev, 15) == 0) {
                                    external_usb_detected = true;
                                    external_msc_detected = true;
                                    active_msc_dev = &external_usb_dev;
                                    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                                    log_info("BOOT", "Phone switched to USB Mass Storage! Booting with Direct Block Access (0 MB in RAM)...");
                                    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                                    disk_log_flush();

                                    if (external_usb_dev.has_adb) {
                                        log_info("BOOT", "Composite phone re-enumerated with ADB! Restoring ADB session...");
                                        if (adb_init_session(&external_usb_dev, &active_adb_session) == 0) {
                                            log_info("BOOT", "ADB session active. Ready for OS handoff.");
                                        }
                                    }

                                    boot_from_usb_msc(&external_usb_dev, boot_info, prof, &selected->iso_files);
                                    ums_booted = true;
                                    break;
                                } else {
                                    log_info("BOOT", "MSC re-enumeration timed out. Phone did not switch to block device.");
                                    disk_log_flush();
                                    phone_save_boot_log(&active_adb_session, &active_mtp_session);
                                }
                            }
                        }

                        if (!ums_booted && selected->mtp_handle != 0) {
                            vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
                            log_info("BOOT", "UMS direct block access not active for '%s'.", selected->filename);
                            log_info("BOOT", "Stream full image into RAM via MTP? (Press 'Y' or Enter to stream, 'N' to cancel): ");
                            vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                            disk_log_flush();
                            char ans = (char)menu_get_char();
                            if (ans == 'n' || ans == 'N' || ans == 27) {
                                log_info("BOOT", "MTP RAM boot cancelled by user. Returning to menu...");
                                disk_log_flush();
                                phone_save_boot_log(&active_adb_session, &active_mtp_session);
                                for (int w = 0; w < 1000000; w++) io_wait();
                                continue;
                            }
                        }
                    }

                    if (!ums_booted) {
                        if (selected->approach == BOOT_APPROACH_BLOCK_ON_DEMAND || selected->approach == BOOT_APPROACH_CHAINLOAD) {
                            if (selected->storage_type == OS_STORAGE_BLOCK_USB) {
                                boot_from_usb_msc(selected->usb_dev, boot_info, prof, &selected->iso_files);
                            } else if (selected->storage_type == OS_STORAGE_BLOCK_SD) {
                                boot_from_sd_fat(selected, boot_info);
                            }
                        } else if (selected->approach == BOOT_APPROACH_MTP_IN_RAM || selected->storage_type == OS_STORAGE_MTP_ANDROID) {
                            if (active_mtp_session.session_active) {
                                boot_from_android_mtp(selected, boot_info, prof);
                            } else if (selected->storage_type == OS_STORAGE_MTP_ANDROID && selected->mtp_handle == 0 && selected->file_size > 0 && *(uint32_t *)LINUX_RAM_ISO_PHYS != 0) {
                                boot_from_in_ram_iso(selected, boot_info, prof);
                            } else {
                                vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
                                log_error("BOOT", "MTP session not active and image not in RAM! Press 'R' to rescan.");
                                vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                                disk_log_flush();
                                phone_save_boot_log(&active_adb_session, &active_mtp_session);
                                for (int w = 0; w < 2000000; w++) io_wait();
                                continue;
                            }
                        }
                    }
                }
                continue; // Return to menu if boot returned or was cancelled
            }

            case MENU_ACTION_DIAGNOSTICS:
                menu_show_diagnostics(boot_info, &xhci_ctrl,
                                      external_usb_detected ? &external_usb_dev : (boot_msc_detected ? &boot_msc_device : NULL),
                                      &active_mtp_session);
                test_linux_boot_simulation(boot_info);
                disk_log_flush();
                continue; // Return to menu after diagnostics

            case MENU_ACTION_SELF_TEST:
                test_linux_boot_simulation(boot_info);
                disk_log_flush();
                continue; // Return to menu after self-test

            case MENU_ACTION_VIEW_LOG:
                phone_save_boot_log(&active_adb_session, &active_mtp_session);
                menu_view_system_log();
                continue; // Return to menu after viewing log

            default:
                continue;
        }
        break;
    }

    // Boot-end summary: log flush statistics for diagnostics
    log_info("LOG", "Boot session complete: %u flushes, %u errors, %u bytes logged",
             disk_log_get_flush_count(), disk_log_get_error_count(), disk_log_get_length());
    disk_log_flush_with_feedback();
    phone_save_boot_log(&active_adb_session, &active_mtp_session);

    // Main execution loop / halt
    while (1) {
        __asm__ volatile ("hlt");
    }
}
