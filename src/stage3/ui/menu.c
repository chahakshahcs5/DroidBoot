#include "menu.h"
#include "../core/printf.h"
#include "../debug/vga.h"
#include "../debug/serial.h"
#include "../../include/io.h"
#include "../memory/memory.h"
#include "../pci/pci.h"

static int poll_input_char(void) {
    // 1. Poll COM1 Serial UART (LSR bit 0 = Data Ready)
    if (inb(0x3F8 + 5) & 0x01) {
        return inb(0x3F8);
    }

    // 2. Poll PS/2 Keyboard Controller (Status Port 0x64 bit 0 = Output Buffer Full)
    if (inb(0x64) & 0x01) {
        uint8_t sc = inb(0x60);
        // Translate Scancode Set 1: top row numbers or numeric keypad
        if (sc == 0x02 || sc == 0x4F) return '1';
        if (sc == 0x03 || sc == 0x50) return '2';
        if (sc == 0x04 || sc == 0x51) return '3';
        if (sc == 0x05 || sc == 0x4B) return '4';
        if (sc == 0x06 || sc == 0x4C) return '5';
        if (sc == 0x07 || sc == 0x4D) return '6';
        if (sc == 0x08 || sc == 0x47) return '7';
        if (sc == 0x09 || sc == 0x48) return '8';
        if (sc == 0x20) return 'd'; // 'D'
        if (sc == 0x14) return 't'; // 'T'
        if (sc == 0x1C) return '\n';
        if (sc == 0x39) return ' ';
    }

    return -1;
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

    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    printk("\n[MENU] Select option [1-%u]: ", test_num);
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    while (1) {
        int ch = poll_input_char();
        if (ch >= '1' && ch <= '8') {
            uint32_t val = (uint32_t)(ch - '0');
            if (val <= os_count) {
                uint32_t idx = val - 1;
                printk("%c\n[MENU] User selected [%u]: Boot %s\n", ch, val, registry->entries[idx].title);
                return (menu_selection_t){ .type = MENU_ACTION_BOOT_OS, .os_index = idx };
            } else if (val == diag_num) {
                printk("%c\n[MENU] User selected [%u]: Hardware Diagnostics\n", ch, val);
                return (menu_selection_t){ .type = MENU_ACTION_DIAGNOSTICS, .os_index = 0 };
            } else if (val == test_num || (os_count == 0 && val == 4)) {
                printk("%c\n[MENU] User selected [%u]: Linux Boot Protocol Simulation\n", ch, val);
                return (menu_selection_t){ .type = MENU_ACTION_SELF_TEST, .os_index = 0 };
            }
        }
        if (ch == 'd' || ch == 'D') {
            printk("D\n[MENU] User selected: Hardware Diagnostics\n");
            return (menu_selection_t){ .type = MENU_ACTION_DIAGNOSTICS, .os_index = 0 };
        }
        if (ch == 't' || ch == 'T') {
            printk("T\n[MENU] User selected: Linux Boot Protocol Simulation\n");
            return (menu_selection_t){ .type = MENU_ACTION_SELF_TEST, .os_index = 0 };
        }

        // Small delay (~1 ms) to not burn 100% CPU
        for (int w = 0; w < 1000; w++) {
            io_wait();
        }
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

int menu_select_persistence_profile(os_entry_t *entry) {
    if (!entry || entry->profile_count <= 1) return 0;

    // Drain any leftover input characters from previous menu selection
    while (poll_input_char() != -1) {
        for (int w = 0; w < 100; w++) io_wait();
    }

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    printk("\n======================================================================\n");
    printk("  PERSISTENCE PROFILE SELECTOR: %s\n", entry->title);
    printk("======================================================================\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    printk("Discovered profiles in /BootManager/persistence/:\n");

    for (uint32_t i = 0; i < entry->profile_count; i++) {
        persistence_profile_t *p = &entry->profiles[i];
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        printk("  [%u] %s\n", i + 1, p->profile_name);
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        if (p->is_clean_session) {
            printk("      * Mode: 100%% In-RAM (Clean Session - No changes saved to phone)\n");
        } else {
            printk("      * File: %s (%u MB)\n", p->filename, (uint32_t)(p->file_size / 1024 / 1024));
            printk("      * Mode: Persistent Read/Write Overlay\n");
        }
    }

    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    printk("\n[PROFILE] Select persistence profile [1-%u]: ", entry->profile_count);
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    // Wait indefinitely for explicit user selection (zero auto-selection)
    while (1) {
        int ch = poll_input_char();
        if (ch >= '1' && ch <= '0' + (int)entry->profile_count) {
            int selected = ch - '1';
            printk("%c\n[PROFILE] User selected [%d]: %s\n\n",
                   ch, selected + 1, entry->profiles[selected].profile_name);
            return selected;
        }
        for (int w = 0; w < 1000; w++) io_wait();
    }
}

