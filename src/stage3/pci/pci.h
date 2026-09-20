#ifndef PCI_H
#define PCI_H

#include <stdint.h>
#include <stdbool.h>

#define PCI_CLASS_SERIAL_BUS        0x0C
#define PCI_SUBCLASS_USB            0x03
#define PCI_PROGIF_UHCI             0x00
#define PCI_PROGIF_OHCI             0x10
#define PCI_PROGIF_EHCI             0x20
#define PCI_PROGIF_XHCI             0x30

typedef struct pci_device {
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint32_t bar0;
    uint32_t bar1;
} pci_device_t;

uint32_t pci_read_config32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
void     pci_write_config32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val);
uint16_t pci_read_config16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
void     pci_write_config16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint16_t val);
uint8_t  pci_read_config8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);

void pci_init(void);
pci_device_t *pci_find_xhci(void);

#endif // PCI_H
