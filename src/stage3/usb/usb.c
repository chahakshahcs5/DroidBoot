#include "usb.h"
#include "usb_msc.h"
#include "../debug/disk_log.h"
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

static void __attribute__((unused)) mdelay(uint32_t ms) {
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
    uintptr_t setup_trb_phys = (uintptr_t)trb_setup;
    uintptr_t data_trb_phys = trb_data ? (uintptr_t)trb_data : 0;

    // Poll Event Ring for Transfer Event
    int timeout = 20000;
    while (--timeout > 0) {
        xhci_trb_t *evt = &ctrl->event_ring[ctrl->event_dequeue_idx];
        uint32_t cycle = evt->control & 1U;

        if (cycle == ctrl->event_cycle_state) {
            uint32_t type = (evt->control >> TRB_TYPE_SHIFT) & 0x3F;
            if (type == TRB_TRANSFER_EVENT) {
                uintptr_t p = (uintptr_t)evt->parameter;
                if (p == status_trb_phys || p == setup_trb_phys || p == data_trb_phys) {
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
    void *dev_ctx = kmalloc_aligned(4096, 64);
    ctrl->dcbaa[slot_id] = (uintptr_t)dev_ctx;

    uint8_t *input_ctx = (uint8_t *)kmalloc_aligned(4096, 64);
    for (int i = 0; i < 4096; i++) input_ctx[i] = 0;

    uint8_t ctx_sz = ctrl->context_size ? ctrl->context_size : 32;

    // Input Control Context (first ctx_sz bytes):
    // Add flags: bit 0 (Slot Context), bit 1 (EP0 Context)
    *(uint32_t *)(input_ctx + 4) = (1U << 0) | (1U << 1);

    // Slot Context (offset = 1 * ctx_sz: 0x20 in 32-byte mode, 0x40 in 64-byte mode)
    uint32_t *slot_ctx = (uint32_t *)(input_ctx + ctx_sz);
    slot_ctx[0] = ((uint32_t)speed << 20) | (1U << 27); // Speed & 1 Context Entry (EP0)
    slot_ctx[1] = ((uint32_t)port_num << 16);           // Root Hub Port Number

    // Endpoint 0 Context (offset = 2 * ctx_sz: 0x40 in 32-byte mode, 0x80 in 64-byte mode)
    uint32_t *ep0_ctx = (uint32_t *)(input_ctx + 2 * ctx_sz);
    uint16_t max_packet = (speed == 4) ? 512 : ((speed == 2) ? 8 : 64);
    ep0_ctx[0] = 0;
    ep0_ctx[1] = (3U << 1) | (4U << 3) | ((uint32_t)max_packet << 16); // CErr=3, EP Type = 4 (Control), MaxPacket

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

    // Query String Descriptors (triggers Android gadget notification & logs device name)
    char mfg_str[48] = {0};
    char prod_str[48] = {0};
    if (out_dev->dev_desc.iManufacturer) {
        usb_get_string_descriptor(out_dev, out_dev->dev_desc.iManufacturer, mfg_str, sizeof(mfg_str));
    }
    if (out_dev->dev_desc.iProduct) {
        usb_get_string_descriptor(out_dev, out_dev->dev_desc.iProduct, prod_str, sizeof(prod_str));
    }
    if (mfg_str[0] || prod_str[0]) {
        log_info("USB", "Device Ident: [%s] [%s]", mfg_str, prod_str);
    }

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
    out_dev->has_msc = false;
    uint8_t *ptr = out_dev->config_buf;
    uint8_t *end = ptr + total_len;

    bool current_is_mtp = false;
    bool current_is_msc = false;
    uint8_t cur_iface_num = 0;

    while (ptr + 2 <= end) {
        uint8_t len = ptr[0];
        uint8_t type = ptr[1];
        if (len == 0 || ptr + len > end) break;

        if (type == USB_DESC_INTERFACE) {
            usb_interface_desc_t *iface = (usb_interface_desc_t *)ptr;
            cur_iface_num = iface->bInterfaceNumber;

            // Check if MTP: standard Still Image (0x06/0x01/0x01) or Android Gadget (0xFF/0xFF)
            bool is_std_mtp = (iface->bInterfaceClass == MTP_INTERFACE_CLASS &&
                               iface->bInterfaceSubClass == MTP_INTERFACE_SUBCLASS &&
                               iface->bInterfaceProtocol == MTP_INTERFACE_PROTOCOL);
            bool is_android_mtp = (iface->bInterfaceClass == ANDROID_MTP_CLASS &&
                                   iface->bInterfaceSubClass == ANDROID_MTP_SUBCLASS);

            // Check if Mass Storage: Bulk-Only Transport (0x08 / Subclass / 0x50)
            bool is_msc = (iface->bInterfaceClass == USB_CLASS_MASS_STORAGE &&
                           iface->bInterfaceProtocol == 0x50);

            if (is_std_mtp || is_android_mtp) {
                current_is_mtp = true;
                current_is_msc = false;
                out_dev->has_mtp = true;
                out_dev->mtp_iface_num = cur_iface_num;
                log_info("USB", "  Interface %u: %s MTP INTERFACE (Class=0x%02X, Subclass=0x%02X, Proto=0x%02X)",
                         cur_iface_num, is_android_mtp ? "ANDROID" : "STANDARD",
                         iface->bInterfaceClass, iface->bInterfaceSubClass, iface->bInterfaceProtocol);
            } else if (is_msc) {
                current_is_mtp = false;
                current_is_msc = true;
                out_dev->has_msc = true;
                out_dev->msc_iface_num = cur_iface_num;
                log_info("USB", "  Interface %u: USB MASS STORAGE (Class=0x08, Subclass=0x%02X, Proto=0x50)",
                         cur_iface_num, iface->bInterfaceSubClass);
            } else {
                current_is_mtp = false;
                current_is_msc = false;
                log_info("USB", "  Interface %u: Class=0x%02X, Subclass=0x%02X, Protocol=0x%02X",
                         cur_iface_num, iface->bInterfaceClass, iface->bInterfaceSubClass, iface->bInterfaceProtocol);
            }
        } else if (type == USB_DESC_ENDPOINT) {
            usb_endpoint_desc_t *ep = (usb_endpoint_desc_t *)ptr;
            uint8_t ep_addr = ep->bEndpointAddress;
            uint8_t ep_type = ep->bmAttributes & 0x03; // 2 = Bulk
            uint16_t ep_max_pkt = ep->wMaxPacketSize;

            log_info("USB", "    Endpoint 0x%02X: Type=%u, MaxPacket=%u", ep_addr, ep_type, ep_max_pkt);

            if (current_is_mtp && ep_type == 2) {
                if (ep_addr & 0x80) {
                    out_dev->mtp_bulk_in_ep = ep_addr;
                    out_dev->mtp_bulk_in_max_packet = ep_max_pkt;
                    log_info("MTP", "    -> MTP Bulk IN Endpoint:  0x%02X (MaxPacket: %u)", ep_addr, ep_max_pkt);
                } else {
                    out_dev->mtp_bulk_out_ep = ep_addr;
                    out_dev->mtp_bulk_out_max_packet = ep_max_pkt;
                    log_info("MTP", "    -> MTP Bulk OUT Endpoint: 0x%02X (MaxPacket: %u)", ep_addr, ep_max_pkt);
                }
            } else if (current_is_msc && ep_type == 2) {
                if (ep_addr & 0x80) {
                    out_dev->msc_bulk_in_ep = ep_addr;
                    out_dev->msc_bulk_in_max_packet = ep_max_pkt;
                    log_info("MSC", "    -> MSC Bulk IN Endpoint:  0x%02X (MaxPacket: %u)", ep_addr, ep_max_pkt);
                } else {
                    out_dev->msc_bulk_out_ep = ep_addr;
                    out_dev->msc_bulk_out_max_packet = ep_max_pkt;
                    log_info("MSC", "    -> MSC Bulk OUT Endpoint: 0x%02X (MaxPacket: %u)", ep_addr, ep_max_pkt);
                }
            }
        }
        ptr += len;
    }

    // 8. Set Configuration 1 (Required by Android to transition to CONFIGURED state and show USB prompt)
    usb_setup_packet_t req_set_cfg;
    req_set_cfg.bmRequestType = 0x00;
    req_set_cfg.bRequest = USB_REQ_SET_CONFIGURATION;
    req_set_cfg.wValue = 1;
    req_set_cfg.wIndex = 0;
    req_set_cfg.wLength = 0;

    int set_cfg_res = usb_control_transfer(out_dev, &req_set_cfg, NULL, 0);
    if (set_cfg_res == 0) {
        log_info("USB", "Device Configuration 1 Activated.");
    } else {
        log_info("USB", "Set Configuration 1 status: %d (device already active). Continuing...", set_cfg_res);
    }

    if (out_dev->has_mtp) {
        usb_configure_mtp_endpoints(out_dev);
    } else if (out_dev->has_msc) {
        int msc_cfg = usb_configure_bulk_endpoints(out_dev,
                                                   out_dev->msc_bulk_in_ep, out_dev->msc_bulk_in_max_packet,
                                                   out_dev->msc_bulk_out_ep, out_dev->msc_bulk_out_max_packet);
        if (msc_cfg == 0) {
            usb_msc_init_device(out_dev);
            disk_log_register_usb_msc(out_dev);
        }
    }

    return 0;
}

int usb_configure_bulk_endpoints(usb_device_t *dev, uint8_t in_ep, uint16_t in_max_packet,
                                 uint8_t out_ep, uint16_t out_max_packet) {
    if (!dev) return -1;

    xhci_controller_t *ctrl = dev->ctrl;
    uint8_t slot_id = dev->slot_id;

    uint8_t in_ep_num = in_ep & 0x0F;
    uint8_t out_ep_num = out_ep & 0x0F;

    uint8_t in_ep_ctx_idx = (in_ep_num * 2) + 1;
    uint8_t out_ep_ctx_idx = (out_ep_num * 2);

    uint8_t max_ep_idx = (in_ep_ctx_idx > out_ep_ctx_idx) ? in_ep_ctx_idx : out_ep_ctx_idx;

    if (in_max_packet == 0) in_max_packet = (dev->speed == 4) ? 1024 : 512;
    if (out_max_packet == 0) out_max_packet = (dev->speed == 4) ? 1024 : 512;

    uint8_t *input_ctx = (uint8_t *)kmalloc_aligned(4096, 64);
    for (int i = 0; i < 4096; i++) input_ctx[i] = 0;

    uint8_t ctx_sz = ctrl->context_size ? ctrl->context_size : 32;

    // Input Control Context (offset 0):
    // Drop flags = 0, Add flags = Slot Context (bit 0), EP IN, EP OUT
    *(uint32_t *)(input_ctx + 0) = 0;
    *(uint32_t *)(input_ctx + 4) = (1U << 0) | (1U << in_ep_ctx_idx) | (1U << out_ep_ctx_idx);

    // Slot Context (offset = 1 * ctx_sz):
    // Per xHCI Spec 4.6.6: Software MUST copy the existing Slot Context from the Device Context
    // (ctrl->dcbaa[slot_id]), and only update the Context Entries field to max_ep_idx!
    void *dev_ctx = (void *)(uintptr_t)ctrl->dcbaa[slot_id];
    uint8_t *slot_ctx_bytes = input_ctx + ctx_sz;
    if (dev_ctx) {
        for (uint32_t b = 0; b < ctx_sz; b++) {
            slot_ctx_bytes[b] = ((uint8_t *)dev_ctx)[b];
        }
    }
    uint32_t *slot_ctx = (uint32_t *)slot_ctx_bytes;
    slot_ctx[0] = (slot_ctx[0] & ~(0x1FU << 27)) | ((uint32_t)max_ep_idx << 27);

    // Bulk IN Endpoint Context
    uint32_t *ep_in_ctx = (uint32_t *)(input_ctx + (in_ep_ctx_idx + 1) * ctx_sz);
    ep_in_ctx[0] = 0;
    ep_in_ctx[1] = (3U << 1) | (6U << 3) | ((uint32_t)in_max_packet << 16); // CErr=3, EP Type = 6 (Bulk IN)

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
    ep_in_ctx[4] = in_max_packet;

    // Bulk OUT Endpoint Context
    uint32_t *ep_out_ctx = (uint32_t *)(input_ctx + (out_ep_ctx_idx + 1) * ctx_sz);
    ep_out_ctx[0] = 0;
    ep_out_ctx[1] = (3U << 1) | (2U << 3) | ((uint32_t)out_max_packet << 16); // CErr=3, EP Type = 2 (Bulk OUT)

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
    ep_out_ctx[4] = out_max_packet;

    // Send Configure Endpoint Command
    xhci_trb_t cfg_cmd;
    cfg_cmd.parameter = (uintptr_t)input_ctx;
    cfg_cmd.status = 0;
    cfg_cmd.control = TRB_TYPE(TRB_CONFIG_EP_CMD) | ((uint32_t)slot_id << 24);

    xhci_trb_t cfg_evt;
    int res = xhci_send_command(ctrl, &cfg_cmd, &cfg_evt);
    if (res == 0) {
        log_info("USB", "Bulk Endpoints successfully configured on xHCI (Slot %u)!", slot_id);
    } else {
        log_error("USB", "Failed to configure bulk endpoints on Slot %u (code %d)!", slot_id, res);
    }
    return res;
}

int usb_configure_mtp_endpoints(usb_device_t *dev) {
    if (!dev || !dev->has_mtp) return -1;
    return usb_configure_bulk_endpoints(dev,
                                        dev->mtp_bulk_in_ep, dev->mtp_bulk_in_max_packet,
                                        dev->mtp_bulk_out_ep, dev->mtp_bulk_out_max_packet);
}

int usb_bulk_transfer(usb_device_t *dev, uint8_t ep_addr, void *data, uint32_t len, uint32_t *transferred_out) {
    if (!dev || !data || len == 0) return -1;

    xhci_controller_t *ctrl = dev->ctrl;
    uint8_t slot_id = dev->slot_id;

    uint8_t ep_num = ep_addr & 0x0F;
    bool is_in = (ep_addr & 0x80) != 0;
    uint32_t doorbell_target = (ep_num * 2) + (is_in ? 1 : 0);

    xhci_trb_t *ring = is_in ? dev->bulk_in_ring : dev->bulk_out_ring;
    uint32_t *enqueue_idx_ptr = is_in ? &dev->bulk_in_enqueue_idx : &dev->bulk_out_enqueue_idx;
    uint8_t *cycle_ptr = is_in ? &dev->bulk_in_cycle_state : &dev->bulk_out_cycle_state;

    if (!ring) {
        log_error("USB", "Bulk endpoint 0x%02X ring not initialized!", ep_addr);
        return -2;
    }

    uint32_t idx = *enqueue_idx_ptr;
    uint8_t cur_cycle = *cycle_ptr;
    xhci_trb_t *trb = &ring[idx];

    trb->parameter = (uintptr_t)data;
    trb->status = len;
    trb->control = TRB_TYPE(TRB_NORMAL) | TRB_IOC | (is_in ? TRB_ISP : 0U) | (cur_cycle ? 1U : 0U);

    idx++;
    if (idx >= 64 - 1) {
        ring[63].parameter = (uintptr_t)ring;
        ring[63].control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE_CYCLE | (cur_cycle ? 1U : 0U);
        idx = 0;
        *cycle_ptr ^= 1;
    }
    *enqueue_idx_ptr = idx;

    uintptr_t trb_phys = (uintptr_t)trb;

    // Ring endpoint doorbell
    uintptr_t db_reg = ctrl->db_regs + slot_id * 4;
    xhci_write32(db_reg, doorbell_target);

    // Poll Event Ring for Transfer Event
    int timeout = 100000;
    while (--timeout > 0) {
        xhci_trb_t *evt = &ctrl->event_ring[ctrl->event_dequeue_idx];
        uint32_t cycle = evt->control & 1U;

        if (cycle == ctrl->event_cycle_state) {
            uint32_t type = (evt->control >> TRB_TYPE_SHIFT) & 0x3F;
            if (type == TRB_TRANSFER_EVENT) {
                if (evt->parameter == trb_phys) {
                    ctrl->event_dequeue_idx++;
                    if (ctrl->event_dequeue_idx == XHCI_EVENT_RING_TRBS) {
                        ctrl->event_dequeue_idx = 0;
                        ctrl->event_cycle_state ^= 1;
                    }
                    uintptr_t intr0 = ctrl->rt_regs + 0x20;
                    uintptr_t erdp = (uintptr_t)&ctrl->event_ring[ctrl->event_dequeue_idx];
                    xhci_write64(intr0 + XHCI_INTR_ERDP, erdp | XHCI_ERDP_EHB);

                    uint8_t cc = (uint8_t)((evt->status >> 24) & 0xFF);
                    uint32_t rem = evt->status & 0xFFFFFF;
                    if (transferred_out) {
                        *transferred_out = len - rem;
                    }
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
        for (int w = 0; w < 50; w++) io_wait();
    }

    log_error("USB", "Bulk transfer timed out on EP 0x%02X!", ep_addr);
    return -100;
}

int usb_get_string_descriptor(usb_device_t *dev, uint8_t index, char *out_str, uint16_t max_len) {
    if (!dev || index == 0 || !out_str || max_len == 0) return -1;
    out_str[0] = '\0';

    static uint8_t str_buf[256];
    usb_setup_packet_t req;
    req.bmRequestType = 0x80;
    req.bRequest = USB_REQ_GET_DESCRIPTOR;
    req.wValue = (USB_DESC_STRING << 8) | index;
    req.wIndex = 0x0409; // English (US)
    req.wLength = sizeof(str_buf);

    int res = usb_control_transfer(dev, &req, str_buf, sizeof(str_buf));
    if (res != 0) return res;

    uint8_t len = str_buf[0];
    if (len < 2 || str_buf[1] != USB_DESC_STRING) return -2;

    uint16_t char_count = (len - 2) / 2;
    uint16_t out_idx = 0;
    for (uint16_t i = 0; i < char_count && out_idx + 1 < max_len; i++) {
        uint16_t uc = str_buf[2 + i * 2] | ((uint16_t)str_buf[3 + i * 2] << 8);
        if (uc >= 32 && uc <= 126) {
            out_str[out_idx++] = (char)uc;
        } else {
            out_str[out_idx++] = '?';
        }
    }
    out_str[out_idx] = '\0';
    return 0;
}
