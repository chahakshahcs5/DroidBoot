#include "usb.h"
#include "../../include/io.h"
#include "../core/printf.h"
#include "../memory/memory.h"
#include <stddef.h>

#define EP_RING_TRBS 64

static void udelay(uint32_t us) {
    for (uint32_t i = 0; i < us * 2; i++) {
        io_wait();
    }
}

static void mdelay(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
        udelay(1000);
    }
}



int usb_control_transfer(usb_device_t *dev, usb_setup_packet_t *setup, void *data, uint16_t len) {
    if (!dev || !setup) return -1;

    xhci_controller_t *ctrl = dev->ctrl;
    uint8_t slot_id = dev->slot_id;

    // We enqueue Setup TRB, optional Data TRB, and Status TRB
    uint32_t idx = dev->ep0_enqueue_idx;
    xhci_trb_t *trb_setup = &dev->ep0_ring[idx++];
    if (idx >= EP_RING_TRBS - 1) { idx = 0; dev->ep0_cycle_state ^= 1; }

    uint32_t trt = 0; // No data phase
    if (len > 0) {
        trt = (setup->bmRequestType & 0x80) ? (3U << 16) : (2U << 16); // 3=IN, 2=OUT
    }

    uint64_t setup_val = 0;
    uint8_t *s_src = (uint8_t *)setup;
    uint8_t *s_dst = (uint8_t *)&setup_val;
    for (int i = 0; i < 8; i++) s_dst[i] = s_src[i];

    trb_setup->parameter = setup_val;
    trb_setup->status = 8; // Setup packet is always 8 bytes
    trb_setup->control = TRB_TYPE(TRB_SETUP) | TRB_IDT | trt | (dev->ep0_cycle_state ? 1U : 0U);

    xhci_trb_t *trb_data = NULL;
    if (len > 0 && data) {
        trb_data = &dev->ep0_ring[idx++];
        if (idx >= EP_RING_TRBS - 1) { idx = 0; dev->ep0_cycle_state ^= 1; }

        uint32_t data_dir = (setup->bmRequestType & 0x80) ? (1U << 16) : 0U;
        trb_data->parameter = (uintptr_t)data;
        trb_data->status = len;
        trb_data->control = TRB_TYPE(TRB_DATA) | data_dir | (dev->ep0_cycle_state ? 1U : 0U);
    }

    xhci_trb_t *trb_status = &dev->ep0_ring[idx++];
    if (idx >= EP_RING_TRBS - 1) { idx = 0; dev->ep0_cycle_state ^= 1; }

    // Status direction is opposite of data phase
    uint32_t status_dir = (len > 0 && (setup->bmRequestType & 0x80)) ? 0 : (1U << 16);
    trb_status->parameter = 0;
    trb_status->status = 0;
    trb_status->control = TRB_TYPE(TRB_STATUS) | TRB_IOC | status_dir | (dev->ep0_cycle_state ? 1U : 0U);

    dev->ep0_enqueue_idx = idx;

    // Ring Doorbell: Target = 1 (Endpoint 0 Control)
    uintptr_t db_reg = ctrl->db_regs + slot_id * 4;
    xhci_write32(db_reg, 1);

    uintptr_t status_trb_phys = (uintptr_t)trb_status;

    // Poll Event Ring for Transfer Event
    int timeout = 1000;
    while (--timeout > 0) {
        xhci_trb_t *evt = &ctrl->event_ring[ctrl->event_dequeue_idx];
        uint32_t cycle = evt->control & 1U;

        if (cycle == ctrl->event_cycle_state) {
            uint32_t type = (evt->control >> TRB_TYPE_SHIFT) & 0x3F;
            if (type == TRB_TRANSFER_EVENT) {
                if (evt->parameter == status_trb_phys) {
                    ctrl->event_dequeue_idx++;
                    if (ctrl->event_dequeue_idx == XHCI_EVENT_RING_TRBS) {
                        ctrl->event_dequeue_idx = 0;
                        ctrl->event_cycle_state ^= 1;
                    }
                    uintptr_t intr0 = ctrl->rt_regs + 0x20;
                    uintptr_t erdp = (uintptr_t)&ctrl->event_ring[ctrl->event_dequeue_idx];
                    xhci_write64(intr0 + XHCI_INTR_ERDP, erdp | XHCI_ERDP_EHB);

                    uint8_t cc = (uint8_t)((evt->status >> 24) & 0xFF);
                    return (cc == TRB_COMPL_SUCCESS || cc == TRB_COMPL_SHORT_TX) ? 0 : (int)cc;
                }
            }

            ctrl->event_dequeue_idx++;
            if (ctrl->event_dequeue_idx == XHCI_EVENT_RING_TRBS) {
                ctrl->event_dequeue_idx = 0;
                ctrl->event_cycle_state ^= 1;
            }
            uintptr_t intr0 = ctrl->rt_regs + 0x20;
            uintptr_t erdp = (uintptr_t)&ctrl->event_ring[ctrl->event_dequeue_idx];
            xhci_write64(intr0 + XHCI_INTR_ERDP, erdp | XHCI_ERDP_EHB);
        }
        udelay(50);
    }

    log_error("USB", "Control transfer timed out!");
    return -100;
}

int usb_probe_port(xhci_controller_t *ctrl, uint8_t port_num, usb_device_t *out_dev) {
    if (!ctrl || !out_dev) return -1;

    uintptr_t port_reg = ctrl->op_regs + XHCI_OP_PORTS_BASE + (port_num - 1) * 0x10;
    uint32_t portsc = *(volatile uint32_t *)port_reg;

    if (!(portsc & XHCI_PORT_CCS)) {
        return -1; // No device connected
    }

    log_info("USB", "Found connected device on Root Hub Port %u. Performing port reset...", port_num);

    // 1. Reset Port
    int reset_res = xhci_reset_port(ctrl, port_num);
    if (reset_res != 0) {
        log_error("USB", "Port %u reset failed (error %d)!", port_num, reset_res);
        return -2;
    }

    portsc = *(volatile uint32_t *)port_reg;
    uint8_t speed = (uint8_t)((portsc & XHCI_PORT_SPEED_MASK) >> XHCI_PORT_SPEED_SHIFT);
    out_dev->ctrl = ctrl;
    out_dev->port_num = port_num;
    out_dev->speed = speed;

    // 2. Enable Slot
    uint8_t slot_id = 0;
    int slot_res = xhci_enable_slot(ctrl, &slot_id);
    if (slot_res != 0 || slot_id == 0) {
        log_error("USB", "Enable Slot failed for port %u!", port_num);
        return -3;
    }
    out_dev->slot_id = slot_id;

    // 3. Allocate Device Context & Input Context
    void *dev_ctx = kmalloc_aligned(2048, 64);
    ctrl->dcbaa[slot_id] = (uintptr_t)dev_ctx;

    uint8_t *input_ctx = (uint8_t *)kmalloc_aligned(2048, 64);
    for (int i = 0; i < 2048; i++) input_ctx[i] = 0;

    // Input Control Context (first 64 bytes):
    // Add flags: bit 0 (Slot Context), bit 1 (EP0 Context)
    *(uint32_t *)(input_ctx + 4) = (1U << 0) | (1U << 1);

    // Slot Context (offset 0x20 in 32-byte context mode, or 0x40 in 64-byte mode)
    // Default context size is 32 bytes
    uint32_t *slot_ctx = (uint32_t *)(input_ctx + 0x20);
    slot_ctx[0] = ((uint32_t)speed << 20) | (1U << 27); // Speed & 1 Context Entry (EP0)
    slot_ctx[1] = ((uint32_t)port_num << 16);           // Root Hub Port Number

    // Endpoint 0 Context (offset 0x40)
    uint32_t *ep0_ctx = (uint32_t *)(input_ctx + 0x40);
    ep0_ctx[0] = (3U << 1) | (4U << 3); // CErr=3, EP Type = 4 (Control)

    uint16_t max_packet = (speed == 4) ? 512 : ((speed == 2) ? 8 : 64);
    ep0_ctx[1] = ((uint32_t)max_packet << 16);

    // Allocate EP0 Transfer Ring
    uint32_t ep0_bytes = EP_RING_TRBS * sizeof(xhci_trb_t);
    out_dev->ep0_ring = (xhci_trb_t *)kmalloc_aligned(ep0_bytes, 64);
    for (int i = 0; i < EP_RING_TRBS; i++) {
        out_dev->ep0_ring[i].parameter = 0;
        out_dev->ep0_ring[i].status = 0;
        out_dev->ep0_ring[i].control = 0;
    }
    // Link TRB
    out_dev->ep0_ring[EP_RING_TRBS - 1].parameter = (uintptr_t)out_dev->ep0_ring;
    out_dev->ep0_ring[EP_RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE_CYCLE;
    out_dev->ep0_enqueue_idx = 0;
    out_dev->ep0_cycle_state = 1;

    // tr_dequeue_ptr with DCS = 1
    *(uint64_t *)(&ep0_ctx[2]) = (uintptr_t)out_dev->ep0_ring | 1U;
    ep0_ctx[4] = 8; // Average TRB length

    // 4. Issue Address Device Command
    xhci_trb_t addr_cmd;
    addr_cmd.parameter = (uintptr_t)input_ctx;
    addr_cmd.status = 0;
    addr_cmd.control = TRB_TYPE(TRB_ADDRESS_DEV_CMD) | ((uint32_t)slot_id << 24);

    xhci_trb_t addr_evt;
    int addr_res = xhci_send_command(ctrl, &addr_cmd, &addr_evt);
    if (addr_res != 0) {
        log_error("USB", "Address Device command failed (code %d)!", addr_res);
        return -4;
    }
    log_info("USB", "Device on Port %u Addressed Successfully (Slot %u)!", port_num, slot_id);

    // 5. Get Device Descriptor (18 bytes)
    usb_setup_packet_t req_dev;
    req_dev.bmRequestType = 0x80;
    req_dev.bRequest = USB_REQ_GET_DESCRIPTOR;
    req_dev.wValue = (USB_DESC_DEVICE << 8);
    req_dev.wIndex = 0;
    req_dev.wLength = sizeof(usb_device_desc_t);

    int get_desc_res = usb_control_transfer(out_dev, &req_dev, &out_dev->dev_desc, sizeof(usb_device_desc_t));
    if (get_desc_res != 0) {
        log_error("USB", "Failed to retrieve Device Descriptor (error %d)!", get_desc_res);
        return -5;
    }

    log_info("USB", "Device Descriptor: VID=0x%04X, PID=0x%04X, Class=0x%02X, Subclass=0x%02X, Proto=0x%02X, Configs=%u",
             out_dev->dev_desc.idVendor,
             out_dev->dev_desc.idProduct,
             out_dev->dev_desc.bDeviceClass,
             out_dev->dev_desc.bDeviceSubClass,
             out_dev->dev_desc.bDeviceProtocol,
             out_dev->dev_desc.bNumConfigurations);

    // 6. Get Configuration Descriptor (Header first, then full length)
    usb_setup_packet_t req_cfg;
    req_cfg.bmRequestType = 0x80;
    req_cfg.bRequest = USB_REQ_GET_DESCRIPTOR;
    req_cfg.wValue = (USB_DESC_CONFIGURATION << 8);
    req_cfg.wIndex = 0;
    req_cfg.wLength = 9;

    usb_config_desc_t cfg_hdr;
    int cfg_res = usb_control_transfer(out_dev, &req_cfg, &cfg_hdr, 9);
    if (cfg_res != 0) {
        log_error("USB", "Failed to retrieve Configuration Header (error %d)!", cfg_res);
        return -6;
    }

    uint16_t total_len = cfg_hdr.wTotalLength;
    if (total_len > sizeof(out_dev->config_buf)) total_len = sizeof(out_dev->config_buf);
    req_cfg.wLength = total_len;

    cfg_res = usb_control_transfer(out_dev, &req_cfg, out_dev->config_buf, total_len);
    if (cfg_res != 0) {
        log_error("USB", "Failed to retrieve full Configuration Descriptor (error %d)!", cfg_res);
        return -7;
    }
    out_dev->config_len = total_len;

    // 7. Parse Interfaces and Endpoints
    out_dev->has_mtp = false;
    uint8_t *ptr = out_dev->config_buf;
    uint8_t *end = ptr + total_len;

    bool current_is_mtp = false;
    uint8_t cur_iface_num = 0;

    while (ptr + 2 <= end) {
        uint8_t len = ptr[0];
        uint8_t type = ptr[1];
        if (len == 0 || ptr + len > end) break;

        if (type == USB_DESC_INTERFACE) {
            usb_interface_desc_t *iface = (usb_interface_desc_t *)ptr;
            cur_iface_num = iface->bInterfaceNumber;

            // Check if MTP (Class 0x06, Subclass 0x01, Protocol 0x01)
            if (iface->bInterfaceClass == MTP_INTERFACE_CLASS &&
                iface->bInterfaceSubClass == MTP_INTERFACE_SUBCLASS &&
                iface->bInterfaceProtocol == MTP_INTERFACE_PROTOCOL) {
                current_is_mtp = true;
                out_dev->has_mtp = true;
                out_dev->mtp_iface_num = cur_iface_num;
                log_info("USB", "  Interface %u: STANDARD MTP / PTP INTERFACE FOUND!", cur_iface_num);
            } else {
                current_is_mtp = false;
                log_info("USB", "  Interface %u: Class=0x%02X, Subclass=0x%02X, Protocol=0x%02X",
                         cur_iface_num, iface->bInterfaceClass, iface->bInterfaceSubClass, iface->bInterfaceProtocol);
            }
        } else if (type == USB_DESC_ENDPOINT) {
            usb_endpoint_desc_t *ep = (usb_endpoint_desc_t *)ptr;
            uint8_t ep_addr = ep->bEndpointAddress;
            uint8_t ep_type = ep->bmAttributes & 0x03; // 2 = Bulk
            uint16_t ep_max_pkt = ep->wMaxPacketSize;

            if (current_is_mtp && ep_type == 2) {
                if (ep_addr & 0x80) {
                    out_dev->mtp_bulk_in_ep = ep_addr;
                    out_dev->mtp_bulk_in_max_packet = ep_max_pkt;
                    log_info("MTP", "  MTP Bulk IN Endpoint:  0x%02X (MaxPacket: %u)", ep_addr, ep_max_pkt);
                } else {
                    out_dev->mtp_bulk_out_ep = ep_addr;
                    out_dev->mtp_bulk_out_max_packet = ep_max_pkt;
                    log_info("MTP", "  MTP Bulk OUT Endpoint: 0x%02X (MaxPacket: %u)", ep_addr, ep_max_pkt);
                }
            }
        }
        ptr += len;
    }

    // 8. Set Configuration 1
    usb_setup_packet_t req_set_cfg;
    req_set_cfg.bmRequestType = 0x00;
    req_set_cfg.bRequest = USB_REQ_SET_CONFIGURATION;
    req_set_cfg.wValue = 1;
    req_set_cfg.wIndex = 0;
    req_set_cfg.wLength = 0;

    int set_cfg_res = usb_control_transfer(out_dev, &req_set_cfg, NULL, 0);
    if (set_cfg_res == 0) {
        log_info("USB", "Device Configuration 1 Activated.");
    }

    if (out_dev->has_mtp) {
        usb_configure_mtp_endpoints(out_dev);
    }

    return 0;
}

int usb_configure_mtp_endpoints(usb_device_t *dev) {
    if (!dev || !dev->has_mtp) return -1;

    xhci_controller_t *ctrl = dev->ctrl;
    uint8_t slot_id = dev->slot_id;

    uint8_t in_ep_num = dev->mtp_bulk_in_ep & 0x0F;
    uint8_t out_ep_num = dev->mtp_bulk_out_ep & 0x0F;

    uint8_t in_ep_ctx_idx = (in_ep_num * 2) + 1;
    uint8_t out_ep_ctx_idx = (out_ep_num * 2);

    uint8_t max_ep_idx = (in_ep_ctx_idx > out_ep_ctx_idx) ? in_ep_ctx_idx : out_ep_ctx_idx;

    uint8_t *input_ctx = (uint8_t *)kmalloc_aligned(2048, 64);
    for (int i = 0; i < 2048; i++) input_ctx[i] = 0;

    // Input Control Context: Add flags
    *(uint32_t *)(input_ctx + 4) = (1U << 0) | (1U << in_ep_ctx_idx) | (1U << out_ep_ctx_idx);

    // Slot Context
    uint32_t *slot_ctx = (uint32_t *)(input_ctx + 0x20);
    slot_ctx[0] = ((uint32_t)dev->speed << 20) | ((uint32_t)max_ep_idx << 27);
    slot_ctx[1] = ((uint32_t)dev->port_num << 16);

    // Bulk IN Endpoint Context
    uint32_t *ep_in_ctx = (uint32_t *)(input_ctx + (in_ep_ctx_idx + 1) * 32);
    ep_in_ctx[0] = (3U << 1) | (6U << 3); // CErr=3, EP Type = 6 (Bulk IN)
    ep_in_ctx[1] = ((uint32_t)dev->mtp_bulk_in_max_packet << 16);

    // Allocate Bulk IN Ring
    dev->bulk_in_ring = (xhci_trb_t *)kmalloc_aligned(64 * sizeof(xhci_trb_t), 64);
    for (int i = 0; i < 64; i++) {
        dev->bulk_in_ring[i].parameter = 0;
        dev->bulk_in_ring[i].status = 0;
        dev->bulk_in_ring[i].control = 0;
    }
    dev->bulk_in_ring[63].parameter = (uintptr_t)dev->bulk_in_ring;
    dev->bulk_in_ring[63].control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE_CYCLE;
    dev->bulk_in_enqueue_idx = 0;
    dev->bulk_in_cycle_state = 1;
    *(uint64_t *)(&ep_in_ctx[2]) = (uintptr_t)dev->bulk_in_ring | 1U;
    ep_in_ctx[4] = dev->mtp_bulk_in_max_packet;

    // Bulk OUT Endpoint Context
    uint32_t *ep_out_ctx = (uint32_t *)(input_ctx + (out_ep_ctx_idx + 1) * 32);
    ep_out_ctx[0] = (3U << 1) | (2U << 3); // CErr=3, EP Type = 2 (Bulk OUT)
    ep_out_ctx[1] = ((uint32_t)dev->mtp_bulk_out_max_packet << 16);

    // Allocate Bulk OUT Ring
    dev->bulk_out_ring = (xhci_trb_t *)kmalloc_aligned(64 * sizeof(xhci_trb_t), 64);
    for (int i = 0; i < 64; i++) {
        dev->bulk_out_ring[i].parameter = 0;
        dev->bulk_out_ring[i].status = 0;
        dev->bulk_out_ring[i].control = 0;
    }
    dev->bulk_out_ring[63].parameter = (uintptr_t)dev->bulk_out_ring;
    dev->bulk_out_ring[63].control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE_CYCLE;
    dev->bulk_out_enqueue_idx = 0;
    dev->bulk_out_cycle_state = 1;
    *(uint64_t *)(&ep_out_ctx[2]) = (uintptr_t)dev->bulk_out_ring | 1U;
    ep_out_ctx[4] = dev->mtp_bulk_out_max_packet;

    // Send Configure Endpoint Command
    xhci_trb_t cfg_cmd;
    cfg_cmd.parameter = (uintptr_t)input_ctx;
    cfg_cmd.status = 0;
    cfg_cmd.control = TRB_TYPE(TRB_CONFIG_EP_CMD) | ((uint32_t)slot_id << 24);

    xhci_trb_t cfg_evt;
    int res = xhci_send_command(ctrl, &cfg_cmd, &cfg_evt);
    if (res == 0) {
        log_info("MTP", "Bulk Endpoints successfully configured on xHCI controller!");
    } else {
        log_error("MTP", "Failed to configure bulk endpoints (code %d)!", res);
    }
    return res;
}
