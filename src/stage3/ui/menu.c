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
        if (sc == 0x1C) return '\n';
        if (sc == 0x39) return ' ';
    }

    return -1;
}

void menu_render(boot_info_t *boot_info, xhci_controller_t *xhci,
                 usb_device_t *usb_dev, mtp_session_t *mtp_session) {
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    printk("\n+------------------------------------------------------------------------+\n");
    printk("|              ANDROID -> LINUX BOOTLOADER (LEGACY BIOS)                 |\n");
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
    printk("| Boot Menu Selection:                                                   |\n");
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    if (usb_dev && usb_dev->has_msc) {
        printk("|  [1] Boot Linux from USB Block Device / Rooted Phone (On-Demand Direct)|\n");
    } else {
        printk("|  [1] Boot Linux from Android Phone (MTP In-RAM Boot + SD Persistence) |\n");
    }
    printk("|  [2] Boot Linux from SD Card (FAT32 Partition)                         |\n");
    printk("|  [3] Hardware Diagnostics & PCI / USB / Memory Inspection              |\n");
    printk("|  [4] Linux 32-bit Boot Protocol Self-Test & Handoff Simulation         |\n");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    printk("+------------------------------------------------------------------------+\n");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

boot_choice_t menu_wait_selection(bool has_mtp, bool has_msc) {
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    printk("\n[MENU] Select option [1-4]: ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    while (1) {
        int ch = poll_input_char();
        if (ch == '1') {
            if (has_msc) {
                printk("1\n[MENU] User selected [1]: Boot from USB Block Storage (Direct)\n");
                return BOOT_CHOICE_USB_MSC;
            } else if (has_mtp) {
                printk("1\n[MENU] User selected [1]: Boot from Android Phone (MTP In-RAM)\n");
                return BOOT_CHOICE_ANDROID_MTP;
            } else {
                printk("1\n[MENU] User selected [1]: Boot from Android Phone (MTP)\n");
                return BOOT_CHOICE_ANDROID_MTP;
            }
        }
        if (ch == '2') {
            printk("2\n[MENU] User selected [2]: Boot Linux from SD Card (FAT32)\n");
            return BOOT_CHOICE_SD_FAT;
        }
        if (ch == '3') {
            printk("3\n[MENU] User selected [3]: Hardware Diagnostics\n");
            return BOOT_CHOICE_DIAGNOSTICS;
        }
        if (ch == '4') {
            printk("4\n[MENU] User selected [4]: Linux Boot Protocol Simulation\n");
            return BOOT_CHOICE_TEST_PROTOCOL;
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
            printk("  [%u] 0x%08X%08X - 0x%08X%08X (Type %u)\n",
                   i,
                   (uint32_t)(map[i].base >> 32), (uint32_t)map[i].base,
                   (uint32_t)((map[i].base + map[i].length) >> 32),
                   (uint32_t)(map[i].base + map[i].length),
                   map[i].type);
        }
    }

    // 2. xHCI Diagnostics
    if (xhci && xhci->op_regs) {
        uint32_t usbcmd = *(volatile uint32_t *)(xhci->op_regs + XHCI_OP_USBCMD);
        uint32_t usbsts = *(volatile uint32_t *)(xhci->op_regs + XHCI_OP_USBSTS);
        printk("[DIAG] xHCI Controller State: USBCMD=0x%08X | USBSTS=0x%08X\n", usbcmd, usbsts);
        printk("[DIAG] xHCI Max Slots: %u, Max Ports: %u\n", xhci->max_slots, xhci->max_ports);
    }

    // 3. USB Device
    if (usb_dev && usb_dev->slot_id > 0) {
        printk("[DIAG] USB Device: Slot %u, VID=0x%04X, PID=0x%04X, HasMTP=%s\n",
               usb_dev->slot_id, usb_dev->dev_desc.idVendor,
               usb_dev->dev_desc.idProduct, usb_dev->has_mtp ? "YES" : "NO");
    }

    printk("=== END DIAGNOSTICS ===\n\n");
}
