#ifndef USB_H
#define USB_H

#include <stdint.h>
#include <stdbool.h>
#include "../../include/usb_defs.h"
#include "../xhci/xhci.h"

#define USB_CLASS_VENDOR_SPECIFIC 0xFF
#define USB_SUBCLASS_ADB          0x42
#define USB_PROTO_ADB             0x01

#define USB_MAX_DEVICES 8

typedef struct usb_device {
    xhci_controller_t *ctrl;
    uint8_t  slot_id;
    uint8_t  port_num;
    uint8_t  speed;

    usb_device_desc_t dev_desc;
    uint8_t           config_buf[512];
    uint16_t          config_len;

    // Default Control Endpoint 0 Transfer Ring
    xhci_trb_t *ep0_ring;
    uint32_t   ep0_enqueue_idx;
    uint8_t    ep0_cycle_state;

    // Detected MTP Interface Parameters
    bool     has_mtp;
    uint8_t  mtp_iface_num;
    uint8_t  mtp_bulk_in_ep;
    uint16_t mtp_bulk_in_max_packet;
    uint8_t  mtp_bulk_out_ep;
    uint16_t mtp_bulk_out_max_packet;

    // Detected Mass Storage Interface Parameters
    bool     has_msc;
    uint8_t  msc_iface_num;
    uint8_t  msc_bulk_in_ep;
    uint16_t msc_bulk_in_max_packet;
    uint8_t  msc_bulk_out_ep;
    uint16_t msc_bulk_out_max_packet;

    // Detected ADB Interface Parameters
    bool     has_adb;
    uint8_t  adb_iface_num;
    uint8_t  adb_bulk_in_ep;
    uint16_t adb_bulk_in_max_packet;
    uint8_t  adb_bulk_out_ep;
    uint16_t adb_bulk_out_max_packet;

    // Per-Endpoint Transfer Rings (up to 32 context indices: 0..31)
    xhci_trb_t *ep_rings[32];
    uint32_t   ep_enqueue_idx[32];
    uint8_t    ep_cycle_state[32];

    // Convenience pointers (points to last active IN/OUT ring)
    xhci_trb_t *bulk_in_ring;
    uint32_t   bulk_in_enqueue_idx;
    uint8_t    bulk_in_cycle_state;

    xhci_trb_t *bulk_out_ring;
    uint32_t   bulk_out_enqueue_idx;
    uint8_t    bulk_out_cycle_state;
} usb_device_t;

int  usb_init_subsystem(void);
int  usb_probe_port(xhci_controller_t *ctrl, uint8_t port_num, usb_device_t *out_dev);
int  usb_reprobe_as_msc(xhci_controller_t *ctrl, uint8_t port_num, usb_device_t *out_msc_dev, int max_wait_sec);
int  usb_control_transfer(usb_device_t *dev, usb_setup_packet_t *setup, void *data, uint16_t len);
int  usb_configure_bulk_endpoints(usb_device_t *dev, uint8_t in_ep, uint16_t in_max_packet, uint8_t out_ep, uint16_t out_max_packet);
int  usb_configure_mtp_endpoints(usb_device_t *dev);
int  usb_configure_adb_endpoints(usb_device_t *dev);
int  usb_bulk_transfer(usb_device_t *dev, uint8_t ep_addr, void *data, uint32_t len, uint32_t *transferred_out);
int  usb_bulk_transfer_wait(usb_device_t *dev, uint8_t ep_addr, void *data, uint32_t len, uint32_t *transferred_out, int max_seconds);
int  usb_clear_endpoint_halt(usb_device_t *dev, uint8_t ep_addr);
int  usb_get_string_descriptor(usb_device_t *dev, uint8_t index, char *out_str, uint16_t max_len);

#endif // USB_H

