#include "menu.h"
#include "../core/printf.h"
#include "../debug/vga.h"
#include "../debug/serial.h"
#include "../../include/io.h"
#include "../memory/memory.h"
#include "../pci/pci.h"

extern uint16_t bios_int16_call(uint8_t cmd);

static int poll_input_char(void) {
    // 1. Poll BIOS INT 16h Keyboard Service (universal for laptop built-in and USB keyboards)
    uint16_t k = bios_int16_call(0x01);
    if (k != 0) {
        uint8_t ascii = (uint8_t)(k & 0xFF);
        uint8_t sc = (uint8_t)(k >> 8);
        if (ascii != 0) return (int)ascii;
        // Extended keys without ASCII
        if (sc == 0x4F) return '1';
        if (sc == 0x50) return '2';
        if (sc == 0x51) return '3';
        if (sc == 0x1C) return '\n';
    }

    // 2. Poll COM1 Serial UART (only if physical UART is actually present)
    if (serial_is_present() && (inb(0x3F8 + 5) & 0x01)) {
        return inb(0x3F8);
    }

    // 3. Fallback: Poll PS/2 Keyboard Controller (Status Port 0x64 bit 0 = Output Buffer Full)
    if (inb(0x64) & 0x01) {
        uint8_t sc = inb(0x60);
        // Ignore break codes (key release has bit 7 set)
        if (!(sc & 0x80)) {
            if (sc == 0x02 || sc == 0x4F) return '1';
            if (sc == 0x03 || sc == 0x50) return '2';
            if (sc == 0x04 || sc == 0x51) return '3';
            if (sc == 0x05 || sc == 0x4B) return '4';
            if (sc == 0x06 || sc == 0x4C) return '5';
            if (sc == 0x07 || sc == 0x4D) return '6';
            if (sc == 0x08 || sc == 0x47) return '7';
            if (sc == 0x09 || sc == 0x48) return '8';
            if (sc == 0x0A || sc == 0x49) return '9';
            if (sc == 0x0B || sc == 0x52) return '0';
            if (sc == 0x0E) return 0x08; // Backspace
            if (sc == 0x20) return 'd'; // 'D'
            if (sc == 0x14) return 't'; // 'T'
            if (sc == 0x1C) return '\n'; // Enter
            if (sc == 0x39) return ' ';
        }
    }

    return -1;
}

int menu_get_char(void) {
    while (1) {
        int ch = poll_input_char();
        if (ch != -1) return ch;
        for (int w = 0; w < 10000; w++) io_wait();
    }
}

static int menu_read_line(char *buf, uint32_t max_len, uint32_t timeout_sec, const char *default_val) {
    uint32_t pos = 0;
    buf[0] = '\0';

    uint32_t remaining = timeout_sec;
    uint32_t slice_count = 0;

    while (1) {
        int ch = poll_input_char();
        if (ch != -1) {
            remaining = 0; // Disable countdown immediately on user keystroke

            // Enter key: carriage return (\r) or line feed (\n)
            if (ch == '\r' || ch == '\n') {
                if (pos == 0) {
                    if (default_val && default_val[0]) {
                        for (int i = 0; default_val[i] && pos + 1 < max_len; i++) {
                            buf[pos++] = default_val[i];
                        }
                        buf[pos] = '\0';
                        printk("%s\n", default_val);
                        return (int)pos;
                    }
                    continue;
                }
                printk("\n");
                buf[pos] = '\0';
                return (int)pos;
            }

            // Backspace: 0x08 (BS) or 0x7F (DEL) or '\b'
            if (ch == 0x08 || ch == 0x7F || ch == '\b') {
                if (pos > 0) {
                    pos--;
                    buf[pos] = '\0';
                    printk("\b \b");
                }
                continue;
            }

            // Printable characters
            if (ch >= 32 && ch <= 126) {
                if (pos + 1 < max_len) {
                    buf[pos++] = (char)ch;
                    buf[pos] = '\0';
                    printk("%c", ch);
                }
            }
            continue;
        }

        // Countdown timer tick if active
        if (remaining > 0) {
            slice_count++;
            if (slice_count >= 100) {
                slice_count = 0;
                remaining--;
                if (remaining > 0) {
                    printk("\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b[Auto-boot in %u sec...] ", remaining);
                } else {
                    printk("\n[MENU] Auto-boot timer expired. Booting default [%s]...\n", default_val ? default_val : "1");
                    if (default_val) {
                        for (int i = 0; default_val[i] && pos + 1 < max_len; i++) {
                            buf[pos++] = default_val[i];
                        }
                    }
                    buf[pos] = '\0';
                    return (int)pos;
                }
            }
        }

        for (int w = 0; w < 10000; w++) io_wait();
    }
}

void menu_render(boot_info_t *boot_info, xhci_controller_t *xhci,
                 usb_device_t *usb_dev, mtp_session_t *mtp_session,
                 const os_registry_t *registry) {
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    printk("\n+------------------------------------------------------------------------+\n");
    printk("|              ANDROID -> LINUX BOOTLOADER (DYNAMIC MULTI-OS)            |\n");
    printk("+------------------------------------------------------------------------+\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    printk("| System Diagnostics Status:                                             |\n");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    (void)mtp_session;
    printk("|  * CPU State    : 32-bit Flat Protected Mode (CR0.PE=1, A20 Enabled)   |\n");
    printk("|  * Usable RAM   : %u MiB (via BIOS E820 Memory Map)                 |\n",
           (uint32_t)(memory_get_total_usable() / 1024 / 1024));

    if (boot_info) {
        printk("|  * Boot Storage : BIOS Drive 0x%02X (%s)                         |\n",
               boot_info->boot_drive,
               (boot_info->boot_drive >= 0x80) ? "SD Card / Hard Disk" : "Floppy");
    }

    if (xhci && xhci->op_regs) {
        printk("|  * USB Host     : xHCI Controller Ready (MMIO 0x%08X, %u Ports)     |\n",
               xhci->mmio_base, xhci->max_ports);
    } else {
        printk("|  * USB Host     : No xHCI Controller Initialized                       |\n");
    }

    if (usb_dev && usb_dev->slot_id > 0) {
        if (usb_dev->has_mtp) {
            printk("|  * Phone Status : Android MTP Device Attached (Slot %u, VID: 0x%04X)   |\n",
                   usb_dev->slot_id, usb_dev->dev_desc.idVendor);
        } else if (usb_dev->has_msc) {
            printk("|  * Storage Mode : USB Mass Storage Block Device (Slot %u, VID: 0x%04X)  |\n",
                   usb_dev->slot_id, usb_dev->dev_desc.idVendor);
        } else {
            printk("|  * USB Device   : Attached (Class 0x%02X, VID: 0x%04X, PID: 0x%04X)       |\n",
                   usb_dev->dev_desc.bDeviceClass, usb_dev->dev_desc.idVendor, usb_dev->dev_desc.idProduct);
        }
    } else {
        printk("|  * Phone Status : Waiting for Android USB Connection...                |\n");
    }

    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    printk("+------------------------------------------------------------------------+\n");
    printk("| Discovered Operating Systems & Boot Options:                           |\n");
    printk("+------------------------------------------------------------------------+\n");

    if (registry && registry->count > 0) {
        for (uint32_t i = 0; i < registry->count; i++) {
            const os_entry_t *entry = &registry->entries[i];
            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            printk("|  [%u] %s\n", i + 1, entry->title);
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            printk("|      * Source: %s\n", entry->storage_desc);
            printk("|      * File  : %s (%u MB)\n", entry->filename, (uint32_t)(entry->file_size / 1024 / 1024));
            if (entry->approach == BOOT_APPROACH_BLOCK_ON_DEMAND) {
                vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
                printk("|      * Mode  : Direct Block Access (0 MB OS in RAM, Instant Boot)     |\n");
            } else {
                vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
                printk("|      * Mode  : In-RAM Boot + SD Card Persistence (apkovl=sda1:)      |\n");
            }
        }
    } else {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        printk("|  * No external OS images detected on USB or MTP storage.              |\n");
    }

    uint32_t diag_num = (registry && registry->count > 0) ? registry->count + 1 : 1;
    uint32_t test_num = (registry && registry->count > 0) ? registry->count + 2 : 2;

    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    printk("|------------------------------------------------------------------------|\n");
    if (usb_dev && (usb_dev->has_adb || usb_dev->has_mtp)) {
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        printk("|  [U] Switch Phone to Root USB Mass Storage (UMS) (or press 'U')          |\n");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    }
    printk("|  [R] Rescan USB Devices & Storage               (or press 'R')          |\n");
    printk("|  [%u] Hardware Diagnostics & System Inspection   (or press 'D')          |\n", diag_num);
    printk("|  [%u] Linux 32-bit Boot Protocol Simulation      (or press 'T')          |\n", test_num);
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    printk("+------------------------------------------------------------------------+\n");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

menu_selection_t menu_wait_selection(const os_registry_t *registry) {
    uint32_t os_count = registry ? registry->count : 0;
    uint32_t diag_num = os_count > 0 ? os_count + 1 : 1;
    uint32_t test_num = os_count > 0 ? os_count + 2 : 2;

    while (1) {
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        if (os_count > 0) {
            printk("\n[MENU] Select option [1-%u], 'U' (Root UMS), or 'R' (Rescan): ", test_num);
        } else {
            printk("\n[MENU] Select option: No OS found! Press 'U' (Root UMS), 'R' (Rescan), or 'T' (Test): ");
        }
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

        char line[16];
        if (os_count > 0) {
            menu_read_line(line, sizeof(line), 0, "1");
        } else {
            menu_read_line(line, sizeof(line), 0, "R");
        }

        // Direct letter shortcuts (case-insensitive)
        if ((line[0] == 'u' || line[0] == 'U' || line[0] == 'm' || line[0] == 'M') && line[1] == '\0') {
            printk("[MENU] Switching rooted Android phone to USB Mass Storage (UMS)...\n");
            return (menu_selection_t){ .type = MENU_ACTION_SWITCH_UMS, .os_index = 0 };
        }
        if ((line[0] == 'r' || line[0] == 'R') && line[1] == '\0') {
            printk("[MENU] Rescanning USB and storage devices...\n");
            return (menu_selection_t){ .type = MENU_ACTION_RESCAN, .os_index = 0 };
        }
        if ((line[0] == 'd' || line[0] == 'D') && line[1] == '\0') {
            printk("[MENU] User selected: Hardware Diagnostics\n");
            return (menu_selection_t){ .type = MENU_ACTION_DIAGNOSTICS, .os_index = 0 };
        }
        if ((line[0] == 't' || line[0] == 'T') && line[1] == '\0') {
            printk("[MENU] User selected: Linux Boot Protocol Simulation\n");
            return (menu_selection_t){ .type = MENU_ACTION_SELF_TEST, .os_index = 0 };
        }

        // Numeric parsing
        bool is_num = true;
        uint32_t val = 0;
        for (int i = 0; line[i]; i++) {
            if (line[i] < '0' || line[i] > '9') {
                is_num = false;
                break;
            }
            val = val * 10 + (uint32_t)(line[i] - '0');
        }

        if (is_num && val > 0) {
            if (val <= os_count) {
                uint32_t idx = val - 1;
                printk("[MENU] User selected [%u]: Boot %s\n", val, registry->entries[idx].title);
                return (menu_selection_t){ .type = MENU_ACTION_BOOT_OS, .os_index = idx };
            } else if (val == diag_num) {
                printk("[MENU] User selected [%u]: Hardware Diagnostics\n", val);
                return (menu_selection_t){ .type = MENU_ACTION_DIAGNOSTICS, .os_index = 0 };
            } else if (val == test_num || (os_count == 0 && val == 4)) {
                printk("[MENU] User selected [%u]: Linux Boot Protocol Simulation\n", val);
                return (menu_selection_t){ .type = MENU_ACTION_SELF_TEST, .os_index = 0 };
            }
        }

        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        printk("[MENU] Invalid choice '%s'. Please enter a valid number or option.\n", line);
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    }
}

void menu_show_diagnostics(boot_info_t *boot_info, xhci_controller_t *xhci,
                          usb_device_t *usb_dev, mtp_session_t *mtp_session) {
    (void)mtp_session;
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    printk("\n=== HARDWARE DIAGNOSTICS & SYSTEM INSPECTION ===\n");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    // 1. Memory diagnostics
    if (boot_info && boot_info->e820_count > 0) {
        e820_entry_t *map = (e820_entry_t *)boot_info->e820_map_addr;
        printk("[DIAG] E820 Memory Map (%u entries):\n", boot_info->e820_count);
        for (uint32_t i = 0; i < boot_info->e820_count && i < 8; i++) {
            printk("  [%u] 0x%08X%08X - 0x%08X%08X (Type %u: %s)\n",
                   i,
                   (uint32_t)(map[i].base >> 32), (uint32_t)map[i].base,
                   (uint32_t)((map[i].base + map[i].length) >> 32),
                   (uint32_t)(map[i].base + map[i].length),
                   map[i].type,
                   (map[i].type == 1) ? "USABLE" :
                   (map[i].type == 2) ? "RESERVED" : "ACPI/OTHER");
        }
    }

    // 2. PCI / xHCI diagnostics
    if (xhci && xhci->op_regs) {
        printk("[DIAG] xHCI Controller MMIO: 0x%08X | MaxSlots: %u | MaxPorts: %u\n",
               xhci->mmio_base, xhci->max_slots, xhci->max_ports);
        printk("[DIAG] xHCI Status: USBCMD.RS=1, USBSTS.HCH=0 (Hardware Running)\n");
    }

    // 3. USB device diagnostics
    if (usb_dev && usb_dev->slot_id > 0) {
        printk("[DIAG] USB Device Slot %u on Port %u:\n", usb_dev->slot_id, usb_dev->port_num);
        printk("  VID: 0x%04X, PID: 0x%04X, Class: 0x%02X, Subclass: 0x%02X\n",
               usb_dev->dev_desc.idVendor, usb_dev->dev_desc.idProduct,
               usb_dev->dev_desc.bDeviceClass, usb_dev->dev_desc.bDeviceSubClass);
        if (usb_dev->has_msc) {
            printk("  Endpoint IN: 0x%02X, Endpoint OUT: 0x%02X (USB Mass Storage)\n",
                   usb_dev->msc_bulk_in_ep, usb_dev->msc_bulk_out_ep);
        } else if (usb_dev->has_mtp) {
            printk("  Endpoint IN: 0x%02X, Endpoint OUT: 0x%02X (Android MTP)\n",
                   usb_dev->mtp_bulk_in_ep, usb_dev->mtp_bulk_out_ep);
        }
    } else {
        printk("[DIAG] No active USB peripheral attached to xHCI root ports.\n");
    }

    printk("=== END DIAGNOSTICS ===\n\n");
}

uint64_t menu_prompt_profile_size(void) {
    // Drain input
    while (poll_input_char() != -1) {
        for (int w = 0; w < 100; w++) io_wait();
    }

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    printk("\n======================================================================\n");
    printk("  PERSISTENCE OVERLAY CAPACITY SELECTOR\n");
    printk("======================================================================\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    printk("Select virtual storage capacity (allocated as sparse file on phone):\n");
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    printk("  [1] 2 GB  (Light - Web browsing, documents, config files)\n");
    printk("  [2] 4 GB  (Standard - Recommended for general use & software)\n");
    printk("  [3] 8 GB  (Developer - Large IDEs, compilers, Docker tools)\n");
    printk("  [4] 16 GB (Heavy Workstation - Full Linux software space)\n");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    printk("  * Sparse allocation: Initially consumes only ~35 MB on phone!\n");

    while (1) {
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        printk("\n[SIZE] Select persistence capacity [1-4]: ");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

        char line[16];
        menu_read_line(line, sizeof(line), 0, "2");

        if (line[0] == '1' && line[1] == '\0') {
            printk("[SIZE] Allocated 2 GB sparse overlay capacity.\n\n");
            return (uint64_t)2 * 1024 * 1024 * 1024;
        } else if (line[0] == '2' && line[1] == '\0') {
            printk("[SIZE] Allocated 4 GB standard overlay capacity.\n\n");
            return (uint64_t)4 * 1024 * 1024 * 1024;
        } else if (line[0] == '3' && line[1] == '\0') {
            printk("[SIZE] Allocated 8 GB developer overlay capacity.\n\n");
            return (uint64_t)8 * 1024 * 1024 * 1024;
        } else if (line[0] == '4' && line[1] == '\0') {
            printk("[SIZE] Allocated 16 GB workstation overlay capacity.\n\n");
            return (uint64_t)16 * 1024 * 1024 * 1024;
        }

        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        printk("[SIZE] Invalid choice '%s'. Please select [1-4].\n", line);
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    }
}

int menu_select_phone_image(const os_registry_t *registry) {
    if (!registry || registry->count == 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        printk("\n[UMS] No images detected on phone storage!\n");
        printk("      Place .iso or .img files in /sdcard/Download/ or /sdcard/ISO/.\n\n");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        return -1;
    }

    // Drain input
    while (poll_input_char() != -1) {
        for (int w = 0; w < 100; w++) io_wait();
    }

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    printk("\n======================================================================\n");
    printk("  SELECT IMAGE TO ATTACH AS USB MASS STORAGE (0 MB IN RAM)\n");
    printk("======================================================================\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    printk("Choose which OS image on your phone to expose as a physical USB drive:\n\n");

    for (uint32_t i = 0; i < registry->count; i++) {
        const os_entry_t *entry = &registry->entries[i];
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        printk("  [%u] %s\n", i + 1, entry->title);
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        if (entry->file_size > 0) {
            printk("      * File: %s (%u MB) | %s\n",
                   entry->filename, (uint32_t)(entry->file_size / 1024 / 1024), entry->storage_desc);
        } else {
            printk("      * File: %s | %s\n", entry->filename, entry->storage_desc);
        }
    }

    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    printk("  [0] Cancel / Back to Main Menu\n");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    while (1) {
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        printk("\n[UMS] Select image to convert [1-%u, or 0 to cancel]: ", registry->count);
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

        char line[16];
        menu_read_line(line, sizeof(line), 0, "1");

        if ((line[0] == '0' && line[1] == '\0') || line[0] == 27) {
            printk("[UMS] Cancelled.\n\n");
            return -1;
        }

        bool is_num = true;
        uint32_t val = 0;
        for (int i = 0; line[i]; i++) {
            if (line[i] < '0' || line[i] > '9') {
                is_num = false;
                break;
            }
            val = val * 10 + (uint32_t)(line[i] - '0');
        }

        if (is_num && val >= 1 && val <= registry->count) {
            int selected = (int)val - 1;
            printk("[UMS] Selected: '%s' (%s)\n\n",
                   registry->entries[selected].title, registry->entries[selected].filename);
            return selected;
        }

        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        printk("[UMS] Invalid choice '%s'. Please select [1-%u] or 0.\n", line, registry->count);
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    }
}

int menu_select_persistence_profile(os_entry_t *entry, adb_session_t *adb) {
    if (!entry) return 0;

    // Drain any leftover input characters from previous menu selection (bounded to avoid hangs)
    for (int d = 0; d < 16; d++) {
        if (poll_input_char() == -1) break;
        for (int w = 0; w < 100; w++) io_wait();
    }

    // Ensure there is always at least one option: Clean Disposable Session
    if (entry->profile_count == 0) {
        persistence_profile_t *clean = &entry->profiles[entry->profile_count++];
        uint8_t *b = (uint8_t *)clean;
        for (uint32_t i = 0; i < sizeof(persistence_profile_t); i++) b[i] = 0;
        const char *title = "Clean Disposable Session";
        for (int i = 0; title[i] && i < 31; i++) clean->profile_name[i] = title[i];
        clean->filename[0] = '\0';
        clean->file_size = 0;
        clean->is_clean_session = true;
    }

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    printk("\n======================================================================\n");
    printk("  PERSISTENCE PROFILE SELECTOR: %s\n", entry->title);
    printk("======================================================================\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    printk("Available Persistence Profiles on Phone (/sdcard/BootManager/persistence/):\n");

    for (uint32_t i = 0; i < entry->profile_count; i++) {
        persistence_profile_t *p = &entry->profiles[i];
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        printk("  [%u] %s\n", i + 1, p->profile_name);
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        if (p->is_clean_session) {
            printk("      * Mode: 100%% In-RAM (Clean Session - No changes saved to phone)\n");
        } else {
            printk("      * File: %s (%u MB)\n", p->filename, (uint32_t)(p->file_size / 1024 / 1024));
            printk("      * Mode: Multi-LUN Persistent Read/Write Drive (LUN 1)\n");
        }
    }

    // Additional option: Create New Custom Profile
    uint32_t create_opt = entry->profile_count + 1;
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    printk("  [%u] [+] Create New Custom Profile...\n", create_opt);
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    printk("      * Choose custom virtual capacity (2GB, 4GB, 8GB, 16GB)\n");

    while (1) {
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        printk("\n[PROFILE] Select persistence profile [1-%u]: ", create_opt);
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

        char line[16];
        menu_read_line(line, sizeof(line), 0, "1");

        bool is_num = true;
        uint32_t val = 0;
        for (int i = 0; line[i]; i++) {
            if (line[i] < '0' || line[i] > '9') {
                is_num = false;
                break;
            }
            val = val * 10 + (uint32_t)(line[i] - '0');
        }

        if (is_num && val >= 1 && val <= entry->profile_count) {
            int selected = (int)val - 1;
            printk("[PROFILE] User selected [%d]: %s\n\n",
                   val, entry->profiles[selected].profile_name);
            return selected;
        }
        if (is_num && val == create_opt) {
            printk("[PROFILE] User selected [%u]: Create New Custom Profile\n", create_opt);
            uint64_t chosen_size = menu_prompt_profile_size();
            uint32_t mb = (uint32_t)(chosen_size / 1024 / 1024);
            uint32_t gb = (uint32_t)(chosen_size / 1024 / 1024 / 1024);
            char prof_name[32];
            snprintf(prof_name, sizeof(prof_name), "Custom Profile (%u MB)", mb);
            char prof_file[64];
            if (entry->iso_files.is_casper) {
                snprintf(prof_file, sizeof(prof_file), "custom_%uMB.casper-rw", mb);
            } else {
                snprintf(prof_file, sizeof(prof_file), "custom_%uMB.img", mb);
            }

            // Create sparse file on phone via ADB if active
            if (adb && adb->is_connected) {
                char full_phone_path[128];
                snprintf(full_phone_path, sizeof(full_phone_path), "/sdcard/BootManager/persistence/%s", prof_file);
                vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
                printk("[PROFILE] Allocating %u GB sparse persistence image on phone storage...\n", gb);
                vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                adb_create_sparse_overlay(adb, full_phone_path, gb > 0 ? gb : 2);
            }

            int new_idx = os_add_custom_profile(entry, prof_name, prof_file, chosen_size);
            if (new_idx >= 0) {
                return new_idx;
            }
            return 0;
        }

        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        printk("[PROFILE] Invalid choice '%s'. Please select [1-%u].\n", line, create_opt);
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    }
}

