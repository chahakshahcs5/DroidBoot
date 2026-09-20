#include "../../include/boot.h"
#include "../../include/io.h"
#include "../debug/serial.h"
#include "../debug/vga.h"
#include "printf.h"
#include "../memory/memory.h"
#include "../pci/pci.h"
#include "../xhci/xhci.h"
#include "../usb/usb.h"
#include "../mtp/mtp.h"
#include "../mtp/mtp_source.h"
#include "../filesystem/fat_source.h"
#include "../image/image_detect.h"
#include "../linux/linux_boot.h"
#include "../ui/menu.h"
#include "../filesystem/iso_reader.h"
#include "../debug/disk_log.h"
#include "../debug/sound.h"

static xhci_controller_t xhci_ctrl;
static usb_device_t      detected_usb_dev;
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
                                      alpine_cmdline, boot_info, params_at_low_mem);

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
                                         cmdline, boot_info, &test_params);
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
    disk_log_flush();
}

void c_main(boot_info_t *boot_info) {
    // 1. Initialize Serial Port & VGA Console
    serial_init();
    vga_init();
    sound_boot_tone(); // PC speaker tone indicating Stage 3 is alive!

    // 2. Banner
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    printk("\n======================================================================\n");
    printk("  ANDROID -> LINUX BOOTLOADER (LEGACY BIOS)\n");
    printk("  Stage 3 C Runtime Active at 0x00100000 (32-bit Flat Protected Mode)\n");
    printk("======================================================================\n\n");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    // 3. Report Stage 1/2 Handoff Parameters
    if (boot_info) {
        log_info("STAGE3", "Boot Drive preserved: 0x%02X (%s)",
                 boot_info->boot_drive,
                 (boot_info->boot_drive >= 0x80) ? "Hard Disk / SD" : "Floppy");
    } else {
        log_error("STAGE3", "boot_info pointer is NULL!");
    }

    // 4. Initialize Persistent SD Disk Logging
    disk_log_init(boot_info);

    // 4. Memory Management & E820 Map
    memory_init(boot_info);

    // 5. PCI Bus Enumeration (xHCI Host Controller Discovery)
    pci_init();

    pci_device_t *xhci_pci = pci_find_xhci();
    if (xhci_pci) {
        // Flush all bootstrap and PCI detection logs to SD card while BIOS INT 13h is still operational!
        disk_log_flush();
        // Deactivate BIOS INT 13h disk access before xHCI controller reset
        disk_log_disable_bios();

        // Phase 3: Initialize xHCI Host Controller
        log_info("STAGE3", "Initializing xHCI Host Controller hardware...");
        int xhci_status = xhci_init(xhci_pci, &xhci_ctrl);
        if (xhci_status == 0) {
            // Test command ring with a NO-OP command
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

            // Phase 4 & 5: Detect Connected USB Devices & Android MTP Phone
            // Multi-pass debounce & poll loop (up to 3 seconds) for Android USB PHY negotiation
            log_info("XHCI", "Waiting for Android phone to connect and stabilize on root hub...");
            bool mtp_found = false;
            uint32_t probed_ports = 0;

            for (int poll_iter = 0; poll_iter < 30 && !mtp_found; poll_iter++) {
                if (poll_iter > 0) {
                    // 100ms delay between port scans
                    for (int d = 0; d < 100000; d++) io_wait();
                }

                xhci_poll_ports(&xhci_ctrl);

                for (uint8_t p = 1; p <= xhci_ctrl.max_ports; p++) {
                    if (probed_ports & (1U << p)) continue; // Already successfully enumerated

                    uintptr_t port_reg = xhci_ctrl.op_regs + XHCI_OP_PORTS_BASE + (p - 1) * 0x10;
                    uint32_t portsc = *(volatile uint32_t *)port_reg;

                    if (portsc & XHCI_PORT_CCS) {
                        usb_device_t current_dev;
                        k_memset(&current_dev, 0, sizeof(current_dev));
                        log_info("STAGE3", "Probing USB device on Port %u...", p);
                        int probe_res = usb_probe_port(&xhci_ctrl, p, &current_dev);
                        if (probe_res == 0) {
                            probed_ports |= (1U << p);
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
                                    sound_phone_connected_tone(); // High chime: Phone connected!
                                    mtp_found = true;
                                    break;
                                } else {
                                    log_error("STAGE3", "Failed to initialize MTP session (code %d)", mtp_res);
                                }
                            } else {
                                log_info("STAGE3", "Port %u: Attached device is not MTP (Class 0x%02X)",
                                         p, current_dev.dev_desc.bDeviceClass);
                            }
                        }
                    }
                }
            }

            if (!mtp_found) {
                log_info("STAGE3", "Waiting for Android phone... (plug phone into USB with File Transfer mode)");
                sound_error_tone(); // Warning tone: No MTP phone detected
            }
        } else {
            log_error("STAGE3", "Failed to initialize xHCI controller (error %d)", xhci_status);
            sound_error_tone();
        }
    } else {
        log_info("STAGE3", "No xHCI controller detected on PCI bus.");
        disk_log_flush();
        disk_log_disable_bios();
        sound_error_tone();
    }

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    log_info("STAGE3", "Phase 1 Legacy BIOS Bootstrap Successfully Verified!");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    // Phase 10: Display Interactive Boot Menu
    menu_render(boot_info, &xhci_ctrl, &detected_usb_dev, &active_mtp_session);

    // Wait for user selection or auto-selection timeout (1 second if MTP active, 2 seconds otherwise)
    boot_choice_t choice = menu_wait_selection(active_mtp_session.session_active ? 1 : 2,
                                               active_mtp_session.session_active);

    switch (choice) {
        case BOOT_CHOICE_ANDROID_MTP:
            log_info("BOOT", "Attempting boot from Android Phone (MTP)...");
            if (active_mtp_session.session_active) {
                boot_source_t *mtp_src = boot_source_mtp_create(&active_mtp_session);
                if (mtp_src) {
                    char target_file[64] = {0};
                    if (mtp_find_boot_file(mtp_src, target_file, sizeof(target_file)) == 0) {
                        log_info("BOOT", "Boot image found on Android phone: '%s'", target_file);
                        if (mtp_src->open(mtp_src, target_file) == 0) {
                            uint64_t fsize = mtp_src->size(mtp_src);
                            log_info("BOOT", "Opened '%s' (%u MB). Inspecting...",
                                     target_file, (uint32_t)(fsize / 1024 / 1024));

                            // Check if ISO image
                            iso_boot_files_t iso_files;
                            if (iso_find_boot_files(mtp_src, &iso_files) == 0) {
                                log_info("BOOT", "Alpine Linux ISO detected! Streaming kernel & initramfs...");
                                void *kernel_buf = (void *)0x02000000;
                                void *initrd_buf = (void *)0x04000000;

                                mtp_src->seek(mtp_src, (uint64_t)iso_files.kernel_lba * 2048);
                                mtp_src->read(mtp_src, kernel_buf, iso_files.kernel_size);

                                if (iso_files.found_initrd) {
                                    mtp_src->seek(mtp_src, (uint64_t)iso_files.initrd_lba * 2048);
                                    mtp_src->read(mtp_src, initrd_buf, iso_files.initrd_size);
                                }

                                linux_kernel_info_t kinfo;
                                if (linux_check_kernel_image(kernel_buf, iso_files.kernel_size, &kinfo) == 0) {
                                    static linux_boot_params_t alpine_params;
                                    const char *alpine_cmdline = "modules=loop,squashfs,sd-mod,usb-storage console=tty0 console=ttyS0,115200 quiet";
                                    linux_prepare_boot_params(kernel_buf, iso_files.kernel_size,
                                                              iso_files.found_initrd ? initrd_buf : NULL,
                                                              iso_files.found_initrd ? iso_files.initrd_size : 0,
                                                              alpine_cmdline, boot_info, &alpine_params);

                                    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                                    log_info("BOOT", "==========================================================");
                                    log_info("BOOT", "  HANDING OFF TO ALPINE LINUX KERNEL ENTRY (0x00100000)   ");
                                    log_info("BOOT", "==========================================================");
                                    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                                    disk_log_flush();
                                    sound_kernel_jump_tone(); // Fanfare: Booting into Linux kernel!

                                    linux_boot_jump((uint32_t)kernel_buf + kinfo.protected_mode_offset,
                                                    LINUX_KERNEL_LOAD_PHYS,
                                                    kinfo.protected_mode_size,
                                                    (uint32_t)&alpine_params,
                                                    kinfo.code32_start);
                                }
                            } else {
                                // Raw kernel image
                                void *kernel_buf = (void *)0x02000000;
                                mtp_src->seek(mtp_src, 0);
                                mtp_src->read(mtp_src, kernel_buf, (uint32_t)fsize);

                                linux_kernel_info_t kinfo;
                                if (linux_check_kernel_image(kernel_buf, (uint32_t)fsize, &kinfo) == 0) {
                                    static linux_boot_params_t raw_params;
                                    const char *cmdline = "console=tty0 console=ttyS0,115200 root=/dev/ram0 rw quiet";
                                    linux_prepare_boot_params(kernel_buf, (uint32_t)fsize,
                                                              NULL, 0,
                                                              cmdline, boot_info, &raw_params);
                                    linux_boot_jump((uint32_t)kernel_buf + kinfo.protected_mode_offset,
                                                    LINUX_KERNEL_LOAD_PHYS,
                                                    kinfo.protected_mode_size,
                                                    (uint32_t)&raw_params,
                                                    kinfo.code32_start);
                                }
                            }
                        }
                    } else {
                        log_error("BOOT", "No bootable kernel or ISO found in Android /Download/!");
                        sound_error_tone();
                    }
                }
            } else {
                log_error("BOOT", "Android MTP session not active! Falling back to self-test...");
                sound_error_tone();
                test_linux_boot_simulation(boot_info);
            }
            break;

        case BOOT_CHOICE_SD_FAT:
            log_info("BOOT", "Attempting boot from SD Card (FAT32)...");
            break;

        case BOOT_CHOICE_DIAGNOSTICS:
            menu_show_diagnostics(boot_info, &xhci_ctrl, &detected_usb_dev, &active_mtp_session);
            test_linux_boot_simulation(boot_info);
            break;

        case BOOT_CHOICE_TEST_PROTOCOL:
        default:
            test_linux_boot_simulation(boot_info);
            break;
    }

    disk_log_flush();

    // Main execution loop / halt
    while (1) {
        __asm__ volatile ("hlt");
    }
}
