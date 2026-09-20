#include "pci.h"
#include <stddef.h>
#include "../../include/io.h"
#include "../core/printf.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static pci_device_t detected_xhci;
static bool xhci_found = false;

static uint32_t make_pci_address(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    return (uint32_t)((1U << 31) |
                      ((uint32_t)bus << 16) |
                      ((uint32_t)dev << 11) |
                      ((uint32_t)func << 8) |
                      (offset & 0xFC));
}

uint32_t pci_read_config32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, make_pci_address(bus, dev, func, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_write_config32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val) {
    outl(PCI_CONFIG_ADDRESS, make_pci_address(bus, dev, func, offset));
    outl(PCI_CONFIG_DATA, val);
}

uint16_t pci_read_config16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t val = pci_read_config32(bus, dev, func, offset);
    return (uint16_t)((val >> ((offset & 2) * 8)) & 0xFFFF);
}

void pci_write_config16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint16_t val) {
    uint32_t current = pci_read_config32(bus, dev, func, offset);
    uint32_t shift = (offset & 2) * 8;
    uint32_t mask = 0xFFFFU << shift;
    uint32_t new_val = (current & ~mask) | (((uint32_t)val) << shift);
    pci_write_config32(bus, dev, func, offset, new_val);
}

uint8_t pci_read_config8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t val = pci_read_config32(bus, dev, func, offset);
    return (uint8_t)((val >> ((offset & 3) * 8)) & 0xFF);
}

static void check_pci_device(uint8_t bus, uint8_t dev, uint8_t func) {
    uint16_t vendor = pci_read_config16(bus, dev, func, 0x00);
    if (vendor == 0xFFFF || vendor == 0x0000) return;

    uint16_t device = pci_read_config16(bus, dev, func, 0x02);
    uint8_t class_code = pci_read_config8(bus, dev, func, 0x0B);
    uint8_t subclass   = pci_read_config8(bus, dev, func, 0x0A);
    uint8_t prog_if    = pci_read_config8(bus, dev, func, 0x09);

    if (class_code == PCI_CLASS_SERIAL_BUS && subclass == PCI_SUBCLASS_USB) {
        const char *usb_type = "Unknown USB";
        if (prog_if == PCI_PROGIF_UHCI) usb_type = "UHCI (USB 1.1)";
        else if (prog_if == PCI_PROGIF_OHCI) usb_type = "OHCI (USB 1.1)";
        else if (prog_if == PCI_PROGIF_EHCI) usb_type = "EHCI (USB 2.0)";
        else if (prog_if == PCI_PROGIF_XHCI) usb_type = "xHCI (USB 3.0+)";

        uint32_t bar0 = pci_read_config32(bus, dev, func, 0x10);

        log_info("PCI", "USB Controller: %02X:%02X.%u [%04X:%04X] %s (BAR0=0x%08X)",
                 bus, dev, func, vendor, device, usb_type, bar0);

        if (prog_if == PCI_PROGIF_XHCI && !xhci_found) {
            detected_xhci.bus = bus;
            detected_xhci.dev = dev;
            detected_xhci.func = func;
            detected_xhci.vendor_id = vendor;
            detected_xhci.device_id = device;
            detected_xhci.class_code = class_code;
            detected_xhci.subclass = subclass;
            detected_xhci.prog_if = prog_if;
            detected_xhci.bar0 = bar0;
            detected_xhci.bar1 = pci_read_config32(bus, dev, func, 0x14);
            xhci_found = true;
        }
    }
}

void pci_init(void) {
    log_info("PCI", "Scanning PCI bus for host controllers...");
    xhci_found = false;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint16_t vendor = pci_read_config16(bus, dev, 0, 0x00);
            if (vendor == 0xFFFF) continue;

            check_pci_device(bus, dev, 0);

            // Check if multi-function device
            uint8_t header_type = pci_read_config8(bus, dev, 0, 0x0E);
            if (header_type & 0x80) {
                for (uint8_t func = 1; func < 8; func++) {
                    check_pci_device(bus, dev, func);
                }
            }
        }
    }

    if (xhci_found) {
        log_info("PCI", "Primary xHCI Host Controller selected at %02X:%02X.%u",
                 detected_xhci.bus, detected_xhci.dev, detected_xhci.func);
    } else {
        log_info("PCI", "No xHCI Host Controller detected on PCI bus.");
    }
}

pci_device_t *pci_find_xhci(void) {
    return xhci_found ? &detected_xhci : NULL;
}
