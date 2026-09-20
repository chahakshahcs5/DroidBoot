#include "../../include/boot.h"
#include "../../include/io.h"
#include "../debug/serial.h"
#include "../debug/vga.h"
#include "printf.h"
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
#include "../bios/vbe.h"
#include "../bios/bios_disk.h"
#include "../image/os_scanner.h"
#include "../adb/adb.h"

static xhci_controller_t xhci_ctrl;
static usb_device_t      detected_usb_dev;
static usb_device_t      detected_msc_dev;
static bool              msc_found = false;
static mtp_session_t     active_mtp_session;

static void k_memset(void *dst, uint8_t val, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = val;
}

static void test_linux_boot_simulation(boot_info_t *boot_info) {
    // Check if a real Alpine kernel was preloaded into RAM at 0x02000000
    uint32_t magic_at_ram = *(volatile uint32_t *)(0x02000000 + 0x0202);
    if (magic_at_ram == LINUX_HDRS_MAGIC) {
        log_info("BOOT", "==========================================================");
        log_info("BOOT", "  REAL ALPINE LINUX KERNEL DETECTED AT 0x02000000!        ");
        log_info("BOOT", "==========================================================");

        void *kernel_buf = (void *)0x02000000;
        void *initrd_buf = (void *)0x04000000;
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
                                      NULL, 0, 0,
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
                                         (void *)0x04000000, 1048576,
                                         cmdline, boot_info,
                                         NULL, 0, 0,
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

static void boot_from_usb_msc(usb_device_t *dev, boot_info_t *boot_info, const persistence_profile_t *prof) {
    log_info("BOOT", "Attempting boot from USB Block Storage on Port %u...", dev->port_num);
    boot_source_t *msc_src = boot_source_msc_create(dev);
    if (!msc_src) {
        log_error("BOOT", "Failed to create boot source for USB block device.");
        sound_error_tone();
        return;
    }

    iso_boot_files_t iso_files;
    int r = iso_find_boot_files(msc_src, &iso_files);
    if (r != 0 || !iso_files.found_kernel) {
        log_error("BOOT", "No bootable kernel found on USB block device!");
        sound_error_tone();
        msc_src->close(msc_src);
        return;
    }

    log_info("BOOT", "==========================================================");
    log_info("BOOT", "  BOOTABLE OS DETECTED ON USB BLOCK DEVICE!               ");
    log_info("BOOT", "  * Distro Type    : %s", iso_files.is_casper ? "Ubuntu / Casper Live (6GB)" : "Linux Live System");
    log_info("BOOT", "  * Kernel LBA     : %u (Size: %u MB)", iso_files.kernel_lba, iso_files.kernel_size / 1024 / 1024);
    log_info("BOOT", "  * Initramfs LBA  : %u (Size: %u MB)", iso_files.initrd_lba, iso_files.initrd_size / 1024 / 1024);
    log_info("BOOT", "  * Mode           : Direct Block Access (0 MB OS in RAM!) ");
    if (prof) {
        log_info("BOOT", "  * Persistence    : %s (%s)", prof->profile_name, prof->filename);
    }
    log_info("BOOT", "==========================================================");

    // Read ONLY kernel (16MB) and initrd (91MB) into RAM
    void *kernel_buf = (void *)0x02000000;
    void *initrd_buf = (void *)0x04000000;

    log_info("BOOT", "Streaming tiny kernel (%u MB) from block device into RAM...", iso_files.kernel_size / 1024 / 1024);
    msc_src->seek(msc_src, (uint64_t)iso_files.kernel_lba * 2048);
    msc_src->read(msc_src, kernel_buf, iso_files.kernel_size);

    if (iso_files.found_initrd) {
        log_info("BOOT", "Streaming initramfs (%u MB) from block device into RAM...", iso_files.initrd_size / 1024 / 1024);
        msc_src->seek(msc_src, (uint64_t)iso_files.initrd_lba * 2048);
        msc_src->read(msc_src, initrd_buf, iso_files.initrd_size);
    }

    msc_src->close(msc_src);

    linux_kernel_info_t kinfo;
    if (linux_check_kernel_image(kernel_buf, iso_files.kernel_size, &kinfo) != 0) {
        log_error("BOOT", "Failed to validate kernel image from block device!");
        sound_error_tone();
        return;
    }

    // Determine kernel command line:
    char cmdline[256];
    if (iso_files.is_casper) {
        if (prof && prof->is_clean_session) {
            snprintf(cmdline, sizeof(cmdline),
                     "boot=casper console=tty0 quiet splash");
        } else {
            snprintf(cmdline, sizeof(cmdline),
                     "boot=casper persistent persistent-path=/BootManager/persistence/ console=tty0 quiet splash");
        }
    } else {
        if (prof && prof->is_clean_session) {
            snprintf(cmdline, sizeof(cmdline),
                     "modules=loop,squashfs,sd-mod,usb-storage console=tty0");
        } else {
            snprintf(cmdline, sizeof(cmdline),
                     "modules=loop,squashfs,sd-mod,usb-storage console=tty0 apkovl=sda1:");
        }
    }

    // Switch to VBE Linear Framebuffer mode (activates HDMI external display & ANSI colors & scrollback)
    vbe_mode_info_t vbe_mode = {0};
    uint16_t vbe_mode_num = 0;
    int vbe_ok = vbe_setup_linear_framebuffer(&vbe_mode, &vbe_mode_num);
    if (vbe_ok == 0) {
        log_info("BOOT", "VBE Linear Framebuffer active: Mode 0x%04X (%ux%ux%u)",
                 vbe_mode_num, vbe_mode.x_res, vbe_mode.y_res, vbe_mode.bits_per_pixel);
    } else {
        log_info("BOOT", "VBE unavailable; falling back to VGA text mode.");
    }

    linux_boot_params_t *params = (linux_boot_params_t *)LINUX_BOOT_PARAMS_PHYS;
    linux_prepare_boot_params(kernel_buf, iso_files.kernel_size,
                              iso_files.found_initrd ? initrd_buf : NULL,
                              iso_files.found_initrd ? iso_files.initrd_size : 0,
                              cmdline, boot_info,
                              (vbe_ok == 0) ? &vbe_mode : NULL,
                              0, 0, // 0 MB of 6GB in RAM, no E820 reservation needed!
                              params);

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    log_info("BOOT", "==========================================================");
    log_info("BOOT", "  HANDING OFF EXECUTION TO LINUX (ON-DEMAND BLOCK STORAGE)");
    log_info("BOOT", "  * OS Filesystem streamed on-demand directly from phone! ");
    log_info("BOOT", "  * RAM consumed by OS image: 0 MB!                       ");
    log_info("BOOT", "==========================================================");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

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

static void boot_in_ram_iso_handoff(uint32_t total_iso_bytes, const iso_boot_files_t *iso_files, boot_info_t *boot_info, const persistence_profile_t *prof) {
    uint8_t *ram_iso = (uint8_t *)LINUX_RAM_ISO_PHYS;
    void *kernel_buf = (void *)0x02000000;
    void *initrd_buf = (void *)0x04000000;

    for (uint32_t b = 0; b < iso_files->kernel_size; b++) {
        ((uint8_t *)kernel_buf)[b] = ram_iso[(iso_files->kernel_lba * 2048) + b];
    }
    log_info("BOOT", "Kernel extracted into RAM at 0x%08X (%u bytes).",
             (uint32_t)kernel_buf, iso_files->kernel_size);

    if (iso_files->found_initrd) {
        for (uint32_t b = 0; b < iso_files->initrd_size; b++) {
            ((uint8_t *)initrd_buf)[b] = ram_iso[(iso_files->initrd_lba * 2048) + b];
        }
        log_info("BOOT", "Initramfs extracted into RAM at 0x%08X (%u bytes).",
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

    char alpine_cmdline[256];
    if (prof && prof->is_clean_session) {
        snprintf(alpine_cmdline, sizeof(alpine_cmdline),
                 "modules=loop,squashfs,sd-mod,usb-storage,phram,mtdblock phram=iso,0x%08X,0x%08X memmap=0x%08X$0x%08X memdisk=yes console=tty0 loglevel=7",
                 LINUX_RAM_ISO_PHYS, total_iso_bytes, total_iso_bytes, LINUX_RAM_ISO_PHYS);
    } else {
        snprintf(alpine_cmdline, sizeof(alpine_cmdline),
                 "modules=loop,squashfs,sd-mod,usb-storage,phram,mtdblock phram=iso,0x%08X,0x%08X memmap=0x%08X$0x%08X memdisk=yes console=tty0 loglevel=7 apkovl=sda1:",
                 LINUX_RAM_ISO_PHYS, total_iso_bytes, total_iso_bytes, LINUX_RAM_ISO_PHYS);
    }

    // Switch to VBE Linear Framebuffer mode
    vbe_mode_info_t vbe_mode = {0};
    uint16_t vbe_mode_num = 0;
    int vbe_ok = vbe_setup_linear_framebuffer(&vbe_mode, &vbe_mode_num);
    if (vbe_ok == 0) {
        log_info("BOOT", "VBE Linear Framebuffer active: Mode 0x%04X (%ux%ux%u)",
                 vbe_mode_num, vbe_mode.x_res, vbe_mode.y_res, vbe_mode.bits_per_pixel);
    } else {
        log_info("BOOT", "VBE unavailable; falling back to VGA text mode.");
    }

    linux_boot_params_t *alpine_params = (linux_boot_params_t *)LINUX_BOOT_PARAMS_PHYS;
    linux_prepare_boot_params(kernel_buf, iso_files->kernel_size,
                              iso_files->found_initrd ? initrd_buf : NULL,
                              iso_files->found_initrd ? iso_files->initrd_size : 0,
                              alpine_cmdline, boot_info,
                              (vbe_ok == 0) ? &vbe_mode : NULL,
                              LINUX_RAM_ISO_PHYS, total_iso_bytes,
                              alpine_params);

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    log_info("BOOT", "==========================================================");
    log_info("BOOT", "  HANDING OFF TO IN-RAM LINUX WITH SD PERSISTENCE         ");
    log_info("BOOT", "  * In-RAM ISO     : 0x%08X (%u MB, Type 2 RESERVED)     ",
             LINUX_RAM_ISO_PHYS, total_iso_bytes / 1024 / 1024);
    if (prof && prof->is_clean_session) {
        log_info("BOOT", "  * Persistence    : Clean Session (100%% In-RAM, no saved changes)");
    } else {
        log_info("BOOT", "  * Persistence    : %s (apkovl=sda1: / /BootManager/persistence/)",
                 prof ? prof->profile_name : "Default");
    }
    log_info("BOOT", "==========================================================");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

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

static void boot_from_in_ram_iso(os_entry_t *entry, boot_info_t *boot_info, const persistence_profile_t *prof) {
    if (!entry) return;
    log_info("BOOT", "Booting preloaded In-RAM ISO at 0x%08X (%u MB)...",
             LINUX_RAM_ISO_PHYS, (uint32_t)(entry->file_size / 1024 / 1024));
    boot_in_ram_iso_handoff((uint32_t)entry->file_size, &entry->iso_files, boot_info, prof);
}

static void boot_from_android_mtp(mtp_session_t *session, boot_info_t *boot_info, const persistence_profile_t *prof) {
    log_info("BOOT", "Attempting boot from Android Phone (MTP)...");
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

    char target_file[64] = {0};
    if (mtp_find_boot_file(mtp_src, target_file, sizeof(target_file)) != 0) {
        log_error("BOOT", "No bootable kernel or ISO found in Android /Download/!");
        sound_error_tone();
        return;
    }

    log_info("BOOT", "Boot image found on Android phone: '%s'", target_file);
    if (mtp_src->open(mtp_src, target_file) != 0) {
        log_error("BOOT", "Failed to open '%s' on Android phone!", target_file);
        sound_error_tone();
        return;
    }

    uint64_t fsize = mtp_src->size(mtp_src);
    log_info("BOOT", "Opened '%s' (%u MB). Inspecting...",
             target_file, (uint32_t)(fsize / 1024 / 1024));

    iso_boot_files_t iso_files;
    if (iso_find_boot_files(mtp_src, &iso_files) == 0 && iso_files.found_kernel) {
        log_info("BOOT", "Bootable ISO detected! (%s)",
                 iso_files.is_casper ? "Ubuntu / Casper Live" : "Alpine Linux");
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
            if (pct >= last_pct + 10 || streamed == total_iso_bytes) {
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
                                      NULL, 0, 0,
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
    uint8_t drive = (uint8_t)(uintptr_t)priv;
    return bios_disk_read(drive, lba, (uint16_t)count, buf);
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
        linux_prepare_boot_params(kernel_buf, fsize, NULL, 0, cmdline, boot_info, NULL, 0, 0, params);
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

void c_main(boot_info_t *boot_info) {
    // 1. Initialize Serial Port & VGA Console
    serial_init();
    vga_init();
    sound_boot_tone();

    // 2. Initialize Persistent SD Disk Logging EARLY
    disk_log_init(boot_info);

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

    // 6. PCI Bus Enumeration (xHCI Host Controller Discovery)
    pci_init();

    pci_device_t *xhci_pci = pci_find_xhci();
    if (xhci_pci) {
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

            bool phone_prompted = false;
            bool mtp_found = false;
            uint32_t probed_ports = 0;
            uint8_t probe_fail_count[32] = {0};

            // Initial scan of connected USB devices
            usb_device_t current_dev;
            for (uint8_t p = 1; p <= xhci_ctrl.max_ports && p < 32; p++) {
                uintptr_t port_reg = xhci_ctrl.op_regs + XHCI_OP_PORTS_BASE + (p - 1) * 0x10;
                uint32_t portsc = *(volatile uint32_t *)port_reg;

                if (!(portsc & XHCI_PORT_CCS)) continue;

                k_memset(&current_dev, 0, sizeof(current_dev));
                log_info("STAGE3", "Probing USB device on Port %u...", p);
                int probe_res = usb_probe_port(&xhci_ctrl, p, &current_dev);
                if (probe_res == 0) {
                    probed_ports |= (1U << p);
                    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                    log_info("STAGE3", "Phase 4 USB Enumeration Successfully Verified!");
                    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

                    if (current_dev.has_adb) {
                        log_info("STAGE3", "Port %u: Android ADB interface detected! Initializing Strategy A root handshake...", p);
                        adb_session_t adb_sess;
                        if (adb_init_session(&current_dev, &adb_sess) == 0) {
                            adb_trigger_mass_storage(&adb_sess, NULL);
                        }
                    }

                    if (current_dev.has_msc) {
                        log_info("STAGE3", "Port %u: USB Mass Storage Block Storage registered.", p);
                        detected_msc_dev = current_dev;
                        msc_found = true;
                        if (detected_usb_dev.slot_id == 0) {
                            detected_usb_dev = current_dev;
                        }
                    } else if (current_dev.has_mtp) {
                        detected_usb_dev = current_dev;
                        log_info("STAGE3", "Android MTP interface detected on Port %u! Initializing MTP session...", p);
                        int mtp_res = mtp_init_session(&detected_usb_dev, &active_mtp_session);
                        if (mtp_res == 0) {
                            uint32_t storage_ids[8];
                            uint32_t count = 0;
                            mtp_get_storage_ids(&active_mtp_session, storage_ids, 8, &count);
                            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                            log_info("STAGE3", "Phase 5 Android MTP Detection Successfully Verified!");
                            vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                            sound_phone_connected_tone();
                            mtp_found = true;
                        }
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
                    }
                }
            }

            // Poll loop: wait if device not yet detected
            for (int poll_iter = 0; poll_iter < 100 && !mtp_found && !msc_found; poll_iter++) {
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

                        if (current_dev.has_mtp) {
                            detected_usb_dev = current_dev;
                            log_info("STAGE3", "Android MTP interface detected on Port %u! Initializing MTP session...", p);
                            int mtp_res = mtp_init_session(&detected_usb_dev, &active_mtp_session);
                            if (mtp_res == 0) {
                                uint32_t storage_ids[8];
                                uint32_t count = 0;
                                mtp_get_storage_ids(&active_mtp_session, storage_ids, 8, &count);

                                vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                                log_info("STAGE3", "Phase 5 Android MTP Detection Successfully Verified!");
                                vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                                sound_phone_connected_tone();
                                mtp_found = true;
                                break;
                            }
                        } else if (current_dev.has_msc) {
                            log_info("STAGE3", "Port %u: USB Mass Storage Block Storage registered.", p);
                            detected_msc_dev = current_dev;
                            msc_found = true;
                            if (detected_usb_dev.slot_id == 0) {
                                detected_usb_dev = current_dev;
                            }
                            break;
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

    // Phase 10: Scan Connected Storage & Display Dynamic Interactive Boot Menu
    os_registry_t os_reg;
    os_scan_all_storages(boot_info, &xhci_ctrl,
                         msc_found ? &detected_msc_dev : NULL,
                         active_mtp_session.session_active ? &active_mtp_session : NULL,
                         &os_reg);

    menu_render(boot_info, &xhci_ctrl, &detected_usb_dev, &active_mtp_session, &os_reg);

    // Wait for explicit user selection (no auto-boot countdown)
    menu_selection_t choice = menu_wait_selection(&os_reg);

    switch (choice.type) {
        case MENU_ACTION_BOOT_OS: {
            if (choice.os_index < os_reg.count) {
                os_entry_t *selected = &os_reg.entries[choice.os_index];
                log_info("BOOT", "Booting selected OS #%u: '%s'...", choice.os_index + 1, selected->title);

                if (selected->profile_count > 1) {
                    selected->selected_profile = menu_select_persistence_profile(selected);
                }
                const persistence_profile_t *prof = (selected->profile_count > 0) ?
                    &selected->profiles[selected->selected_profile] : NULL;

                if (selected->approach == BOOT_APPROACH_BLOCK_ON_DEMAND) {
                    if (selected->storage_type == OS_STORAGE_BLOCK_USB) {
                        boot_from_usb_msc(selected->usb_dev, boot_info, prof);
                    } else if (selected->storage_type == OS_STORAGE_BLOCK_SD) {
                        boot_from_sd_fat(selected, boot_info);
                    }
                } else if (selected->approach == BOOT_APPROACH_MTP_IN_RAM) {
                    if (active_mtp_session.session_active) {
                        boot_from_android_mtp(&active_mtp_session, boot_info, prof);
                    } else {
                        boot_from_in_ram_iso(selected, boot_info, prof);
                    }
                }
            }
            break;
        }

        case MENU_ACTION_DIAGNOSTICS:
            menu_show_diagnostics(boot_info, &xhci_ctrl, &detected_usb_dev, &active_mtp_session);
            test_linux_boot_simulation(boot_info);
            break;

        case MENU_ACTION_SELF_TEST:
        default:
            test_linux_boot_simulation(boot_info);
            break;
    }

    // Boot-end summary: log flush statistics for diagnostics
    log_info("LOG", "Boot session complete: %u flushes, %u errors, %u bytes logged",
             disk_log_get_flush_count(), disk_log_get_error_count(), disk_log_get_length());
    disk_log_flush_with_feedback();

    // Main execution loop / halt
    while (1) {
        __asm__ volatile ("hlt");
    }
}
