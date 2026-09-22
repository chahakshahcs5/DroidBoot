#include "usb.h"
#include "usb_msc.h"
#include "../debug/disk_log.h"
#include "../../include/io.h"
#include "../core/printf.h"
#include "../memory/memory.h"
#include <stddef.h>

#include "../core/timer.h"

#define EP_RING_TRBS 64

static inline void udelay(uint32_t us) {
    timer_udelay(us);
}

static inline void mdelay(uint32_t ms) {
    timer_mdelay(ms);
}

void usb_abort_control_endpoint(usb_device_t *dev) {
    if (!dev || !dev->ctrl || dev->slot_id == 0 || !dev->ep0_ring) return;

    // 1. Issue Stop Endpoint Command on EP 1 (EP0 Control)
    xhci_trb_t stop_cmd;
    stop_cmd.parameter = 0;
    stop_cmd.status = 0;
    stop_cmd.control = TRB_TYPE(TRB_STOP_ENDPOINT_CMD) | ((uint32_t)dev->slot_id << 24) | (1U << 16);
    xhci_trb_t evt;
    int stop_res = xhci_send_command(dev->ctrl, &stop_cmd, &evt);
    if (stop_res == -100) {
        dev->is_disconnected = true;
        return;
    }

    // 2. Drain any pending transfer events for this slot/EP0 from the event ring
    for (int i = 0; i < XHCI_EVENT_RING_TRBS; i++) {
        xhci_trb_t *ev = &dev->ctrl->event_ring[dev->ctrl->event_dequeue_idx];
        if ((ev->control & 1U) == dev->ctrl->event_cycle_state) {
            uint32_t type = (ev->control >> TRB_TYPE_SHIFT) & 0x3F;
            if (type == TRB_TRANSFER_EVENT) {
                dev->ctrl->event_dequeue_idx++;
                if (dev->ctrl->event_dequeue_idx == XHCI_EVENT_RING_TRBS) {
                    dev->ctrl->event_dequeue_idx = 0;
                    dev->ctrl->event_cycle_state ^= 1;
                }
                uintptr_t intr0 = dev->ctrl->rt_regs + 0x20;
                uintptr_t erdp = (uintptr_t)&dev->ctrl->event_ring[dev->ctrl->event_dequeue_idx];
                xhci_write64(intr0 + XHCI_INTR_ERDP, erdp | XHCI_ERDP_EHB);
            }
        } else {
            break;
        }
    }

    // 3. Reset EP0 transfer ring
    for (int i = 0; i < EP_RING_TRBS - 1; i++) {
        dev->ep0_ring[i].parameter = 0;
        dev->ep0_ring[i].status = 0;
        dev->ep0_ring[i].control = 0;
    }
    dev->ep0_ring[EP_RING_TRBS - 1].parameter = (uintptr_t)dev->ep0_ring;
    dev->ep0_ring[EP_RING_TRBS - 1].status = 0;
    dev->ep0_ring[EP_RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE_CYCLE;
    dev->ep0_enqueue_idx = 0;
    dev->ep0_cycle_state = 1;

    // 4. Set TR Dequeue Pointer to EP0 ring[0] with DCS = 1
    xhci_trb_t deq_cmd;
    deq_cmd.parameter = (uintptr_t)dev->ep0_ring | 1U;
    deq_cmd.status = 0;
    deq_cmd.control = TRB_TYPE(TRB_SET_TR_DEQ_CMD) | ((uint32_t)dev->slot_id << 24) | (1U << 16);
    int deq_res = xhci_send_command(dev->ctrl, &deq_cmd, &evt);
    if (deq_res == -100) dev->is_disconnected = true;
}

int usb_control_transfer(usb_device_t *dev, usb_setup_packet_t *setup, void *data, uint16_t len) {
    if (!dev || !setup || dev->is_disconnected) return -1;

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
    if (len > 0) {
        trb_data = &dev->ep0_ring[idx++];
        if (idx >= EP_RING_TRBS - 1) { idx = 0; dev->ep0_cycle_state ^= 1; }
        trb_data->parameter = (uintptr_t)data;
        trb_data->status = len;
        uint32_t dir_in = (setup->bmRequestType & 0x80) ? (1U << 16) : 0;
        trb_data->control = TRB_TYPE(TRB_DATA) | dir_in | (dev->ep0_cycle_state ? 1U : 0U);
    }

    xhci_trb_t *trb_status = &dev->ep0_ring[idx++];
    if (idx >= EP_RING_TRBS - 1) { idx = 0; dev->ep0_cycle_state ^= 1; }
    trb_status->parameter = 0;
    trb_status->status = 0;
    uint32_t status_dir = (len > 0 && !(setup->bmRequestType & 0x80)) ? (1U << 16) : 0; // Status IN for OUT transfers
    trb_status->control = TRB_TYPE(TRB_STATUS) | TRB_IOC | status_dir | (dev->ep0_cycle_state ? 1U : 0U);

    dev->ep0_enqueue_idx = idx;

    // Ring Doorbell: Target = 1 (Endpoint 0 Control)
    uintptr_t db_reg = ctrl->db_regs + slot_id * 4;
    xhci_write32(db_reg, 1);

    uintptr_t status_trb_phys = (uintptr_t)trb_status;
    uintptr_t setup_trb_phys = (uintptr_t)trb_setup;
    uintptr_t data_trb_phys = trb_data ? (uintptr_t)trb_data : 0;

    // Poll Event Ring for Transfer Event (up to 200ms: 4000 * 50us)
    int timeout = 4000;
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
                    if (cc != TRB_COMPL_SUCCESS && cc != TRB_COMPL_SHORT_TX) {
                        log_error("USB", "Control transfer failed: bmReq=0x%02X bReq=0x%02X wVal=0x%04X wIdx=0x%04X CC=%u (%s)",
                                  setup->bmRequestType, setup->bRequest, setup->wValue, setup->wIndex, cc, xhci_cc_to_string(cc));
                        return (int)cc;
                    }
                    return 0;
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
    usb_abort_control_endpoint(dev);
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
    for (int i = 0; i < 32; i++) {
        out_dev->ep_rings[i] = NULL;
        out_dev->ep_enqueue_idx[i] = 0;
        out_dev->ep_cycle_state[i] = 0;
    }
    out_dev->bulk_in_ring = NULL;
    out_dev->bulk_out_ring = NULL;
    out_dev->is_disconnected = false;

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
    // Per xHCI Spec 4.3.3: SuperSpeed=512, HighSpeed=64, FullSpeed/LowSpeed=8 initially
    uint16_t max_packet = (speed >= 4) ? 512 : ((speed == 3) ? 64 : 8);
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
        log_error("USB", "Address Device failed (code %d) on Port %u. Retrying after reset...", addr_res, port_num);
        for (int w = 0; w < 50000; w++) io_wait();
        xhci_reset_port(ctrl, port_num);
        for (int w = 0; w < 50000; w++) io_wait();
        addr_res = xhci_send_command(ctrl, &addr_cmd, &addr_evt);
    }
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

    // For Full-Speed (speed 1) or Low-Speed (speed 2), EP0 max packet size is initially 8 bytes.
    // To prevent xHCI Babble Error (CC=3) caused by devices returning packets > 8 bytes,
    // read 8 bytes first, evaluate EP0 context with real bMaxPacketSize0, then read full 18 bytes.
    if (speed < 3) {
        req_dev.wLength = 8;
        int res8 = usb_control_transfer(out_dev, &req_dev, &out_dev->dev_desc, 8);
        if (res8 != 0) {
            log_error("USB", "Failed initial 8-byte Device Descriptor on Port %u (error %d)!", port_num, res8);
            return -5;
        }
        uint16_t dev_ep0_max = out_dev->dev_desc.bMaxPacketSize0;
        if (dev_ep0_max > 0 && dev_ep0_max != max_packet) {
            log_info("USB", "Updating EP0 MaxPacketSize to %u via Evaluate Context...", dev_ep0_max);
            *(uint32_t *)(input_ctx + 0) = 0;
            *(uint32_t *)(input_ctx + 4) = (1U << 1); // Add EP0 Context
            ep0_ctx[1] = (ep0_ctx[1] & ~0xFFFF0000U) | ((uint32_t)dev_ep0_max << 16);
            xhci_trb_t eval_cmd;
            eval_cmd.parameter = (uintptr_t)input_ctx;
            eval_cmd.status = 0;
            eval_cmd.control = TRB_TYPE(TRB_EVAL_CTX_CMD) | ((uint32_t)slot_id << 24);
            xhci_send_command(ctrl, &eval_cmd, &addr_evt);
            max_packet = dev_ep0_max;
        }
    }

    req_dev.wLength = sizeof(usb_device_desc_t);
    int get_desc_res = usb_control_transfer(out_dev, &req_dev, &out_dev->dev_desc, sizeof(usb_device_desc_t));
    if (get_desc_res != 0) {
        log_error("USB", "Failed to retrieve Device Descriptor (error %d)!", get_desc_res);
        return -5;
    }

    uint16_t dev_ep0_max = out_dev->dev_desc.bMaxPacketSize0;
    if (speed >= 4) {
        // Per USB 3.0 Spec 9.6.1: bMaxPacketSize0 is exponent (2^9 = 512 bytes)
        dev_ep0_max = (1U << out_dev->dev_desc.bMaxPacketSize0);
    }

    if (dev_ep0_max > 0 && dev_ep0_max != max_packet) {
        log_info("USB", "Updating EP0 MaxPacketSize to %u via Evaluate Context...", dev_ep0_max);
        *(uint32_t *)(input_ctx + 0) = 0;
        *(uint32_t *)(input_ctx + 4) = (1U << 1); // Add EP0 Context
        ep0_ctx[1] = (ep0_ctx[1] & ~0xFFFF0000U) | ((uint32_t)dev_ep0_max << 16);
        xhci_trb_t eval_cmd;
        eval_cmd.parameter = (uintptr_t)input_ctx;
        eval_cmd.status = 0;
        eval_cmd.control = TRB_TYPE(TRB_EVAL_CTX_CMD) | ((uint32_t)slot_id << 24);
        xhci_send_command(ctrl, &eval_cmd, &addr_evt);
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
    out_dev->has_adb = false;
    uint8_t *ptr = out_dev->config_buf;
    uint8_t *end = ptr + total_len;

    bool current_is_mtp = false;
    bool current_is_msc = false;
    bool current_is_adb = false;
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

            // Check if ADB: Vendor-Specific (0xFF / 0x42 / 0x01)
            bool is_adb = (iface->bInterfaceClass == USB_CLASS_VENDOR_SPECIFIC &&
                           iface->bInterfaceSubClass == USB_SUBCLASS_ADB &&
                           iface->bInterfaceProtocol == USB_PROTO_ADB);

            if (is_std_mtp || is_android_mtp) {
                current_is_mtp = true;
                current_is_msc = false;
                current_is_adb = false;
                out_dev->has_mtp = true;
                out_dev->mtp_iface_num = cur_iface_num;
                log_info("USB", "  Interface %u: %s MTP INTERFACE (Class=0x%02X, Subclass=0x%02X, Proto=0x%02X)",
                         cur_iface_num, is_android_mtp ? "ANDROID" : "STANDARD",
                         iface->bInterfaceClass, iface->bInterfaceSubClass, iface->bInterfaceProtocol);
            } else if (is_msc) {
                current_is_mtp = false;
                current_is_msc = true;
                current_is_adb = false;
                out_dev->has_msc = true;
                out_dev->msc_iface_num = cur_iface_num;
                log_info("USB", "  Interface %u: USB MASS STORAGE (Class=0x08, Subclass=0x%02X, Proto=0x50)",
                         cur_iface_num, iface->bInterfaceSubClass);
            } else if (is_adb) {
                current_is_mtp = false;
                current_is_msc = false;
                current_is_adb = true;
                out_dev->has_adb = true;
                out_dev->adb_iface_num = cur_iface_num;
                log_info("USB", "  Interface %u: ANDROID ADB INTERFACE (Class=0xFF, Subclass=0x42, Proto=0x01)",
                         cur_iface_num);
            } else {
                current_is_mtp = false;
                current_is_msc = false;
                current_is_adb = false;
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
            } else if (current_is_adb && ep_type == 2) {
                if (ep_addr & 0x80) {
                    out_dev->adb_bulk_in_ep = ep_addr;
                    out_dev->adb_bulk_in_max_packet = ep_max_pkt;
                    log_info("ADB", "    -> ADB Bulk IN Endpoint:  0x%02X (MaxPacket: %u)", ep_addr, ep_max_pkt);
                } else {
                    out_dev->adb_bulk_out_ep = ep_addr;
                    out_dev->adb_bulk_out_max_packet = ep_max_pkt;
                    log_info("ADB", "    -> ADB Bulk OUT Endpoint: 0x%02X (MaxPacket: %u)", ep_addr, ep_max_pkt);
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

    if (out_dev->has_msc && out_dev->has_adb) {
        log_info("USB", "Composite device detected (MSC + ADB). Configuring all endpoints concurrently...");
        if (usb_configure_composite_msc_adb(out_dev) == 0) {
            usb_msc_init_device(out_dev);
        }
    } else if (out_dev->has_msc) {
        int msc_cfg = usb_configure_bulk_endpoints(out_dev,
                                                   out_dev->msc_bulk_in_ep, out_dev->msc_bulk_in_max_packet,
                                                   out_dev->msc_bulk_out_ep, out_dev->msc_bulk_out_max_packet);
        if (msc_cfg == 0) {
            usb_msc_init_device(out_dev);
        }
    } else if (out_dev->has_adb) {
        usb_configure_adb_endpoints(out_dev);
    } else if (out_dev->has_mtp) {
        usb_configure_mtp_endpoints(out_dev);
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

    if (in_ep_ctx_idx < 32) {
        dev->ep_rings[in_ep_ctx_idx] = dev->bulk_in_ring;
        dev->ep_enqueue_idx[in_ep_ctx_idx] = 0;
        dev->ep_cycle_state[in_ep_ctx_idx] = 1;
    }

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

    if (out_ep_ctx_idx < 32) {
        dev->ep_rings[out_ep_ctx_idx] = dev->bulk_out_ring;
        dev->ep_enqueue_idx[out_ep_ctx_idx] = 0;
        dev->ep_cycle_state[out_ep_ctx_idx] = 1;
    }

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

int usb_configure_composite_msc_adb(usb_device_t *dev) {
    if (!dev) return -1;

    xhci_controller_t *ctrl = dev->ctrl;
    uint8_t slot_id = dev->slot_id;

    uint8_t msc_in_num = dev->msc_bulk_in_ep & 0x0F;
    uint8_t msc_out_num = dev->msc_bulk_out_ep & 0x0F;
    uint8_t adb_in_num = dev->adb_bulk_in_ep & 0x0F;
    uint8_t adb_out_num = dev->adb_bulk_out_ep & 0x0F;

    uint8_t msc_in_ctx_idx = (msc_in_num * 2) + 1;
    uint8_t msc_out_ctx_idx = (msc_out_num * 2);
    uint8_t adb_in_ctx_idx = (adb_in_num * 2) + 1;
    uint8_t adb_out_ctx_idx = (adb_out_num * 2);

    uint8_t max_ep_idx = msc_in_ctx_idx;
    if (msc_out_ctx_idx > max_ep_idx) max_ep_idx = msc_out_ctx_idx;
    if (adb_in_ctx_idx > max_ep_idx) max_ep_idx = adb_in_ctx_idx;
    if (adb_out_ctx_idx > max_ep_idx) max_ep_idx = adb_out_ctx_idx;

    uint16_t msc_in_pkt = dev->msc_bulk_in_max_packet ? dev->msc_bulk_in_max_packet : 512;
    uint16_t msc_out_pkt = dev->msc_bulk_out_max_packet ? dev->msc_bulk_out_max_packet : 512;
    uint16_t adb_in_pkt = dev->adb_bulk_in_max_packet ? dev->adb_bulk_in_max_packet : 512;
    uint16_t adb_out_pkt = dev->adb_bulk_out_max_packet ? dev->adb_bulk_out_max_packet : 512;

    uint8_t *input_ctx = (uint8_t *)kmalloc_aligned(4096, 64);
    for (int i = 0; i < 4096; i++) input_ctx[i] = 0;

    uint8_t ctx_sz = ctrl->context_size ? ctrl->context_size : 32;

    // Add flags: Slot (bit 0) + all 4 endpoints!
    *(uint32_t *)(input_ctx + 0) = 0;
    *(uint32_t *)(input_ctx + 4) = (1U << 0) |
                                   (1U << msc_in_ctx_idx) | (1U << msc_out_ctx_idx) |
                                   (1U << adb_in_ctx_idx) | (1U << adb_out_ctx_idx);

    // Slot Context
    void *dev_ctx = (void *)(uintptr_t)ctrl->dcbaa[slot_id];
    uint8_t *slot_ctx_bytes = input_ctx + ctx_sz;
    if (dev_ctx) {
        for (uint32_t b = 0; b < ctx_sz; b++) {
            slot_ctx_bytes[b] = ((uint8_t *)dev_ctx)[b];
        }
    }
    uint32_t *slot_ctx = (uint32_t *)slot_ctx_bytes;
    slot_ctx[0] = (slot_ctx[0] & ~(0x1FU << 27)) | ((uint32_t)max_ep_idx << 27);

    // 1. MSC Bulk IN
    uint32_t *ep1 = (uint32_t *)(input_ctx + (msc_in_ctx_idx + 1) * ctx_sz);
    ep1[0] = 0;
    ep1[1] = (3U << 1) | (6U << 3) | ((uint32_t)msc_in_pkt << 16);
    dev->bulk_in_ring = (xhci_trb_t *)kmalloc_aligned(64 * sizeof(xhci_trb_t), 64);
    for (int i = 0; i < 64; i++) { dev->bulk_in_ring[i].parameter = 0; dev->bulk_in_ring[i].status = 0; dev->bulk_in_ring[i].control = 0; }
    dev->bulk_in_ring[63].parameter = (uintptr_t)dev->bulk_in_ring;
    dev->bulk_in_ring[63].control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE_CYCLE;
    dev->bulk_in_enqueue_idx = 0;
    dev->bulk_in_cycle_state = 1;
    *(uint64_t *)(&ep1[2]) = (uintptr_t)dev->bulk_in_ring | 1U;
    ep1[4] = msc_in_pkt;
    if (msc_in_ctx_idx < 32) {
        dev->ep_rings[msc_in_ctx_idx] = dev->bulk_in_ring;
        dev->ep_enqueue_idx[msc_in_ctx_idx] = 0;
        dev->ep_cycle_state[msc_in_ctx_idx] = 1;
    }

    // 2. MSC Bulk OUT
    uint32_t *ep2 = (uint32_t *)(input_ctx + (msc_out_ctx_idx + 1) * ctx_sz);
    ep2[0] = 0;
    ep2[1] = (3U << 1) | (2U << 3) | ((uint32_t)msc_out_pkt << 16);
    dev->bulk_out_ring = (xhci_trb_t *)kmalloc_aligned(64 * sizeof(xhci_trb_t), 64);
    for (int i = 0; i < 64; i++) { dev->bulk_out_ring[i].parameter = 0; dev->bulk_out_ring[i].status = 0; dev->bulk_out_ring[i].control = 0; }
    dev->bulk_out_ring[63].parameter = (uintptr_t)dev->bulk_out_ring;
    dev->bulk_out_ring[63].control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE_CYCLE;
    dev->bulk_out_enqueue_idx = 0;
    dev->bulk_out_cycle_state = 1;
    *(uint64_t *)(&ep2[2]) = (uintptr_t)dev->bulk_out_ring | 1U;
    ep2[4] = msc_out_pkt;
    if (msc_out_ctx_idx < 32) {
        dev->ep_rings[msc_out_ctx_idx] = dev->bulk_out_ring;
        dev->ep_enqueue_idx[msc_out_ctx_idx] = 0;
        dev->ep_cycle_state[msc_out_ctx_idx] = 1;
    }

    // 3. ADB Bulk IN
    uint32_t *ep3 = (uint32_t *)(input_ctx + (adb_in_ctx_idx + 1) * ctx_sz);
    ep3[0] = 0;
    ep3[1] = (3U << 1) | (6U << 3) | ((uint32_t)adb_in_pkt << 16);
    xhci_trb_t *adb_in_r = (xhci_trb_t *)kmalloc_aligned(64 * sizeof(xhci_trb_t), 64);
    for (int i = 0; i < 64; i++) { adb_in_r[i].parameter = 0; adb_in_r[i].status = 0; adb_in_r[i].control = 0; }
    adb_in_r[63].parameter = (uintptr_t)adb_in_r;
    adb_in_r[63].control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE_CYCLE;
    *(uint64_t *)(&ep3[2]) = (uintptr_t)adb_in_r | 1U;
    ep3[4] = adb_in_pkt;
    if (adb_in_ctx_idx < 32) {
        dev->ep_rings[adb_in_ctx_idx] = adb_in_r;
        dev->ep_enqueue_idx[adb_in_ctx_idx] = 0;
        dev->ep_cycle_state[adb_in_ctx_idx] = 1;
    }

    // 4. ADB Bulk OUT
    uint32_t *ep4 = (uint32_t *)(input_ctx + (adb_out_ctx_idx + 1) * ctx_sz);
    ep4[0] = 0;
    ep4[1] = (3U << 1) | (2U << 3) | ((uint32_t)adb_out_pkt << 16);
    xhci_trb_t *adb_out_r = (xhci_trb_t *)kmalloc_aligned(64 * sizeof(xhci_trb_t), 64);
    for (int i = 0; i < 64; i++) { adb_out_r[i].parameter = 0; adb_out_r[i].status = 0; adb_out_r[i].control = 0; }
    adb_out_r[63].parameter = (uintptr_t)adb_out_r;
    adb_out_r[63].control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE_CYCLE;
    *(uint64_t *)(&ep4[2]) = (uintptr_t)adb_out_r | 1U;
    ep4[4] = adb_out_pkt;
    if (adb_out_ctx_idx < 32) {
        dev->ep_rings[adb_out_ctx_idx] = adb_out_r;
        dev->ep_enqueue_idx[adb_out_ctx_idx] = 0;
        dev->ep_cycle_state[adb_out_ctx_idx] = 1;
    }

    xhci_trb_t cfg_cmd;
    cfg_cmd.parameter = (uintptr_t)input_ctx;
    cfg_cmd.status = 0;
    cfg_cmd.control = TRB_TYPE(TRB_CONFIG_EP_CMD) | ((uint32_t)slot_id << 24);

    xhci_trb_t cfg_evt;
    int res = xhci_send_command(ctrl, &cfg_cmd, &cfg_evt);
    if (res == 0) {
        log_info("USB", "Composite MSC+ADB endpoints successfully configured on xHCI (Slot %u)!", slot_id);
    } else {
        log_error("USB", "Failed to configure composite endpoints on Slot %u (code %d)!", slot_id, res);
    }
    return res;
}

int usb_configure_mtp_endpoints(usb_device_t *dev) {
    if (!dev || !dev->has_mtp) return -1;
    return usb_configure_bulk_endpoints(dev,
                                        dev->mtp_bulk_in_ep, dev->mtp_bulk_in_max_packet,
                                        dev->mtp_bulk_out_ep, dev->mtp_bulk_out_max_packet);
}

int usb_configure_adb_endpoints(usb_device_t *dev) {
    if (!dev || !dev->has_adb) return -1;
    log_info("USB", "Configuring ADB Bulk Endpoints: IN=0x%02X, OUT=0x%02X...",
             dev->adb_bulk_in_ep, dev->adb_bulk_out_ep);
    return usb_configure_bulk_endpoints(dev,
                                        dev->adb_bulk_in_ep, dev->adb_bulk_in_max_packet,
                                        dev->adb_bulk_out_ep, dev->adb_bulk_out_max_packet);
}

int usb_clear_endpoint_halt(usb_device_t *dev, uint8_t ep_addr) {
    if (!dev || !dev->ctrl) return -1;

    // 1. Send CLEAR_FEATURE(ENDPOINT_HALT) control transfer to device
    usb_setup_packet_t req;
    req.bmRequestType = 0x02; // Endpoint recipient
    req.bRequest = USB_REQ_CLEAR_FEATURE;
    req.wValue = 0; // ENDPOINT_HALT
    req.wIndex = ep_addr;
    req.wLength = 0;
    int res = usb_control_transfer(dev, &req, NULL, 0);

    // 2. Query xHCI Endpoint State from Device Context
    uint8_t ep_num = ep_addr & 0x0F;
    bool is_in = (ep_addr & 0x80) != 0;
    uint8_t ep_ctx_idx = (ep_num * 2) + (is_in ? 1 : 0);

    uint8_t ctx_sz = dev->ctrl->context_size ? dev->ctrl->context_size : 32;
    void *dev_ctx = (void *)(uintptr_t)dev->ctrl->dcbaa[dev->slot_id];
    uint32_t *ep_ctx = dev_ctx ? (uint32_t *)((uint8_t *)dev_ctx + (ep_ctx_idx + 1) * ctx_sz) : NULL;
    uint8_t ep_state = ep_ctx ? (ep_ctx[0] & 0x07) : 0;

    // xHCI Spec 4.6.8: Reset Endpoint Command is ONLY valid if Endpoint is in HALTED state (state 2).
    // If not halted (e.g. running or stopped), issuing Reset Endpoint causes CC 19 (Context State Error).
    if (ep_state == 2) { // EP_STATE_HALTED
        log_info("USB", "Clearing stall/halt condition on EP 0x%02X...", ep_addr);
        xhci_trb_t cmd;
        cmd.parameter = 0;
        cmd.status = 0;
        cmd.control = TRB_TYPE(TRB_RESET_EP_CMD) | ((uint32_t)dev->slot_id << 24) | ((uint32_t)ep_ctx_idx << 16);
        xhci_trb_t evt;
        xhci_send_command(dev->ctrl, &cmd, &evt);

        // xHCI Spec 4.6.10: Set TR Dequeue Pointer is valid once endpoint is in Stopped state
        xhci_trb_t *ring = (ep_ctx_idx < 32 && dev->ep_rings[ep_ctx_idx]) ? dev->ep_rings[ep_ctx_idx] : (is_in ? dev->bulk_in_ring : dev->bulk_out_ring);
        uint32_t deq_idx = (ep_ctx_idx < 32) ? dev->ep_enqueue_idx[ep_ctx_idx] : (is_in ? dev->bulk_in_enqueue_idx : dev->bulk_out_enqueue_idx);
        uint8_t cycle = (ep_ctx_idx < 32) ? dev->ep_cycle_state[ep_ctx_idx] : (is_in ? dev->bulk_in_cycle_state : dev->bulk_out_cycle_state);

        if (ring) {
            xhci_trb_t deq_cmd;
            deq_cmd.parameter = (uintptr_t)&ring[deq_idx] | (cycle ? 1U : 0U);
            deq_cmd.status = 0;
            deq_cmd.control = TRB_TYPE(TRB_SET_TR_DEQ_CMD) | ((uint32_t)dev->slot_id << 24) | ((uint32_t)ep_ctx_idx << 16);
            xhci_send_command(dev->ctrl, &deq_cmd, &evt);
        }
        log_info("USB", "EP 0x%02X stall cleared, endpoint reset.", ep_addr);
    }

    return res;
}

int usb_abort_bulk_endpoint(usb_device_t *dev, uint8_t ep_addr) {
    if (!dev || !dev->ctrl || dev->is_disconnected) return -1;

    // Check physical port connection if applicable
    if (dev->port_num > 0 && dev->port_num <= dev->ctrl->max_ports) {
        uintptr_t port_reg = dev->ctrl->op_regs + XHCI_OP_PORTS_BASE + (dev->port_num - 1) * 0x10;
        uint32_t portsc = *(volatile uint32_t *)port_reg;
        if (!(portsc & XHCI_PORT_CCS)) {
            dev->is_disconnected = true;
            log_info("USB", "Device on port %u physically disconnected. Skipping endpoint abort.", dev->port_num);
            return -1;
        }
    }

    log_info("USB", "Aborting timed out/halted transfer on EP 0x%02X...", ep_addr);

    uint8_t ep_num = ep_addr & 0x0F;
    bool is_in = (ep_addr & 0x80) != 0;
    uint8_t ep_ctx_idx = (ep_num * 2) + (is_in ? 1 : 0);

    xhci_trb_t evt;

    // 1. Transition endpoint to Stopped state:
    // If endpoint was Halted (e.g. CC=4 USB Transaction Error or CC=6 Stall),
    // Reset Endpoint Command (Type 14) is required per xHCI spec 4.6.8.
    // If endpoint was Running (e.g. host timeout), Stop Endpoint Command (Type 15) is required.
    xhci_trb_t reset_cmd;
    reset_cmd.parameter = 0;
    reset_cmd.status = 0;
    reset_cmd.control = TRB_TYPE(TRB_RESET_EP_CMD) | ((uint32_t)dev->slot_id << 24) | ((uint32_t)ep_ctx_idx << 16);
    int rst_res = xhci_send_command(dev->ctrl, &reset_cmd, &evt);
    if (rst_res == -100) {
        dev->is_disconnected = true;
        return -100;
    }
    if (rst_res != 0) {
        // Endpoint was not Halted (returned CC=19 Context State Error); issue Stop Endpoint
        xhci_trb_t stop_cmd;
        stop_cmd.parameter = 0;
        stop_cmd.status = 0;
        stop_cmd.control = TRB_TYPE(TRB_STOP_ENDPOINT_CMD) | ((uint32_t)dev->slot_id << 24) | ((uint32_t)ep_ctx_idx << 16);
        int stop_res = xhci_send_command(dev->ctrl, &stop_cmd, &evt);
        if (stop_res == -100) {
            dev->is_disconnected = true;
            return -100;
        }
    }

    // 2. Drain any pending transfer events for this device/endpoint from the event ring
    for (int i = 0; i < XHCI_EVENT_RING_TRBS; i++) {
        xhci_trb_t *ev = &dev->ctrl->event_ring[dev->ctrl->event_dequeue_idx];
        if ((ev->control & 1U) == dev->ctrl->event_cycle_state) {
            uint32_t type = (ev->control >> TRB_TYPE_SHIFT) & 0x3F;
            if (type == TRB_TRANSFER_EVENT) {
                dev->ctrl->event_dequeue_idx++;
                if (dev->ctrl->event_dequeue_idx == XHCI_EVENT_RING_TRBS) {
                    dev->ctrl->event_dequeue_idx = 0;
                    dev->ctrl->event_cycle_state ^= 1;
                }
                uintptr_t intr0 = dev->ctrl->rt_regs + 0x20;
                uintptr_t erdp = (uintptr_t)&dev->ctrl->event_ring[dev->ctrl->event_dequeue_idx];
                xhci_write64(intr0 + XHCI_INTR_ERDP, erdp | XHCI_ERDP_EHB);
            }
        } else {
            break;
        }
    }

    // 3. Reset the transfer ring pointers and clear the ring
    xhci_trb_t *ring = (ep_ctx_idx < 32 && dev->ep_rings[ep_ctx_idx]) ? dev->ep_rings[ep_ctx_idx] : (is_in ? dev->bulk_in_ring : dev->bulk_out_ring);
    if (ring) {
        for (int i = 0; i < EP_RING_TRBS - 1; i++) {
            ring[i].parameter = 0;
            ring[i].status = 0;
            ring[i].control = 0;
        }
        ring[EP_RING_TRBS - 1].parameter = (uintptr_t)ring;
        ring[EP_RING_TRBS - 1].status = 0;
        ring[EP_RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE_CYCLE;

        if (ep_ctx_idx < 32) {
            dev->ep_enqueue_idx[ep_ctx_idx] = 0;
            dev->ep_cycle_state[ep_ctx_idx] = 1;
        }
        if (is_in) {
            dev->bulk_in_enqueue_idx = 0;
            dev->bulk_in_cycle_state = 1;
        } else {
            dev->bulk_out_enqueue_idx = 0;
            dev->bulk_out_cycle_state = 1;
        }

        // 4. Issue Set TR Dequeue Pointer Command to point back to ring[0] with DCS = 1
        xhci_trb_t deq_cmd;
        deq_cmd.parameter = (uintptr_t)ring | 1U; // DCS = 1
        deq_cmd.status = 0;
        deq_cmd.control = TRB_TYPE(TRB_SET_TR_DEQ_CMD) | ((uint32_t)dev->slot_id << 24) | ((uint32_t)ep_ctx_idx << 16);
        int deq_res = xhci_send_command(dev->ctrl, &deq_cmd, &evt);
        if (deq_res == -100) {
            dev->is_disconnected = true;
            return -100;
        }
    }

    // 5. Send CLEAR_FEATURE(ENDPOINT_HALT) to USB device only if slot is still responsive
    if (!dev->is_disconnected) {
        usb_setup_packet_t req;
        req.bmRequestType = 0x02;
        req.bRequest = USB_REQ_CLEAR_FEATURE;
        req.wValue = 0;
        req.wIndex = ep_addr;
        req.wLength = 0;
        usb_control_transfer(dev, &req, NULL, 0);
    }

    log_info("USB", "EP 0x%02X transfer ring aborted and re-synchronized successfully.", ep_addr);
    return 0;
}

int usb_bulk_transfer(usb_device_t *dev, uint8_t ep_addr, void *data, uint32_t len, uint32_t *transferred_out) {
    if (!dev || !data || len == 0 || dev->is_disconnected) return -1;

    xhci_controller_t *ctrl = dev->ctrl;
    uint8_t slot_id = dev->slot_id;

    uint8_t ep_num = ep_addr & 0x0F;
    bool is_in = (ep_addr & 0x80) != 0;
    uint8_t ep_ctx_idx = (ep_num * 2) + (is_in ? 1 : 0);
    uint32_t doorbell_target = ep_ctx_idx;

    xhci_trb_t *ring = NULL;
    uint32_t *enqueue_idx_ptr = NULL;
    uint8_t *cycle_ptr = NULL;

    if (ep_ctx_idx < 32 && dev->ep_rings[ep_ctx_idx]) {
        ring = dev->ep_rings[ep_ctx_idx];
        enqueue_idx_ptr = &dev->ep_enqueue_idx[ep_ctx_idx];
        cycle_ptr = &dev->ep_cycle_state[ep_ctx_idx];
    } else {
        ring = is_in ? dev->bulk_in_ring : dev->bulk_out_ring;
        enqueue_idx_ptr = is_in ? &dev->bulk_in_enqueue_idx : &dev->bulk_out_enqueue_idx;
        cycle_ptr = is_in ? &dev->bulk_in_cycle_state : &dev->bulk_out_cycle_state;
    }

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
                    if (cc == TRB_COMPL_STALL_ERR || cc == TRB_COMPL_BABBLE_ERR) {
                        log_error("USB", "Bulk transfer STALL/BABBLE on EP 0x%02X (CC=%u: %s)", ep_addr, cc, xhci_cc_to_string(cc));
                        usb_clear_endpoint_halt(dev, ep_addr);
                        return (int)cc;
                    }
                    if (cc != TRB_COMPL_SUCCESS && cc != TRB_COMPL_SHORT_TX) {
                        log_error("USB", "Bulk transfer failed on EP 0x%02X: CC=%u (%s), transferred %u / %u B",
                                  ep_addr, cc, xhci_cc_to_string(cc), transferred_out ? *transferred_out : 0, len);
                        return (int)cc;
                    }
                    return 0;
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
    usb_abort_bulk_endpoint(dev, ep_addr);
    return -100;
}

int usb_bulk_transfer_wait(usb_device_t *dev, uint8_t ep_addr, void *data, uint32_t len, uint32_t *transferred_out, int max_seconds) {
    if (!dev || !data || len == 0 || dev->is_disconnected) return -1;

    xhci_controller_t *ctrl = dev->ctrl;
    uint8_t slot_id = dev->slot_id;

    uint8_t ep_num = ep_addr & 0x0F;
    bool is_in = (ep_addr & 0x80) != 0;
    uint8_t ep_ctx_idx = (ep_num * 2) + (is_in ? 1 : 0);
    uint32_t doorbell_target = ep_ctx_idx;

    xhci_trb_t *ring = NULL;
    uint32_t *enqueue_idx_ptr = NULL;
    uint8_t *cycle_ptr = NULL;

    if (ep_ctx_idx < 32 && dev->ep_rings[ep_ctx_idx]) {
        ring = dev->ep_rings[ep_ctx_idx];
        enqueue_idx_ptr = &dev->ep_enqueue_idx[ep_ctx_idx];
        cycle_ptr = &dev->ep_cycle_state[ep_ctx_idx];
    } else {
        ring = is_in ? dev->bulk_in_ring : dev->bulk_out_ring;
        enqueue_idx_ptr = is_in ? &dev->bulk_in_enqueue_idx : &dev->bulk_out_enqueue_idx;
        cycle_ptr = is_in ? &dev->bulk_in_cycle_state : &dev->bulk_out_cycle_state;
    }

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

    // Ring endpoint doorbell ONCE
    uintptr_t db_reg = ctrl->db_regs + slot_id * 4;
    xhci_write32(db_reg, doorbell_target);

    // Poll Event Ring for Transfer Event across max_seconds
    for (int sec = max_seconds; sec > 0; sec--) {
        if (max_seconds >= 5 && sec % 5 == 0) {
            log_info("USB", "Awaiting USB response... %d sec remaining", sec);
            disk_log_flush();
        }

        // 100 slices of 10,000 io_waits = ~1 second total per second iteration
        for (int slice = 0; slice < 100; slice++) {
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
                        if (cc != TRB_COMPL_SUCCESS && cc != TRB_COMPL_SHORT_TX) {
                            log_error("USB", "Bulk transfer wait failed on EP 0x%02X: CC=%u (%s), transferred %u / %u B",
                                      ep_addr, cc, xhci_cc_to_string(cc), transferred_out ? *transferred_out : 0, len);
                            usb_abort_bulk_endpoint(dev, ep_addr);
                            return (int)cc;
                        }
                        return 0;
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

            for (int w = 0; w < 10000; w++) io_wait();
        }
    }

    log_error("USB", "Bulk transfer timed out on EP 0x%02X after %d sec!", ep_addr, max_seconds);
    usb_abort_bulk_endpoint(dev, ep_addr);
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

int usb_reprobe_as_msc(xhci_controller_t *ctrl, uint8_t port_num, usb_device_t *out_msc_dev, int max_wait_sec) {
    if (!ctrl || !out_msc_dev || port_num == 0) return -1;

    uintptr_t port_reg = ctrl->op_regs + XHCI_OP_PORTS_BASE + (port_num - 1) * 0x10;
    if (max_wait_sec < 15) max_wait_sec = 15;

    log_info("USB", "Awaiting USB Mass Storage re-enumeration on Port %u (timeout %d sec)...",
             port_num, max_wait_sec);

    // Disable previous slot if active
    if (out_msc_dev->slot_id > 0) {
        xhci_disable_slot(ctrl, out_msc_dev->slot_id);
        out_msc_dev->slot_id = 0;
    }

    // Wait 2.5 seconds for phone UDC unbind & ConfigFS rebind to execute
    for (int w = 0; w < 2500000; w++) io_wait();

    int iterations = max_wait_sec * 10; // 100ms per iteration
    for (int iter = 0; iter < iterations; iter++) {
        uint32_t portsc = xhci_read32(port_reg);

        if (portsc & XHCI_PORT_CCS) {
            // Stabilize connection
            for (int w = 0; w < 250000; w++) io_wait();

            uint8_t *dev_bytes = (uint8_t *)out_msc_dev;
            for (size_t b = 0; b < sizeof(usb_device_t); b++) dev_bytes[b] = 0;

            int res = usb_probe_port(ctrl, port_num, out_msc_dev);
            if (res == 0) {
                if (out_msc_dev->has_msc) {
                    log_info("USB", "Port %u successfully enumerated with USB Mass Storage interface!", port_num);
                    if (usb_msc_init_device(out_msc_dev) == 0) {
                        return 0; // Ready for SCSI I/O!
                    }
                } else {
                    log_info("USB", "Port %u enumerated (VID 0x%04X, PID 0x%04X), awaiting MSC function...",
                             port_num, out_msc_dev->dev_desc.idVendor, out_msc_dev->dev_desc.idProduct);
                    if (out_msc_dev->slot_id > 0) {
                        xhci_disable_slot(ctrl, out_msc_dev->slot_id);
                        out_msc_dev->slot_id = 0;
                    }
                }
            }
        }

        for (int w = 0; w < 100000; w++) io_wait();
    }

    log_error("USB", "Timed out waiting for USB Mass Storage re-enumeration on Port %u!", port_num);
    return -1;
}

static void usb_local_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static void usb_local_memset(void *dst, uint8_t val, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = val;
}

static usb_msc_callback_fn g_hub_msc_callback = NULL;

void usb_set_hub_msc_callback(usb_msc_callback_fn cb) {
    g_hub_msc_callback = cb;
}

int usb_probe_hub_downstream(xhci_controller_t *ctrl, usb_device_t *hub_dev, usb_device_t *out_msc_dev) {
    if (!ctrl || !hub_dev) return -1;
    if (hub_dev->dev_desc.bDeviceClass != USB_CLASS_HUB) return -1;

    bool found_any_msc = false;

    // 1. Retrieve Hub Descriptor
    uint8_t hub_desc_buf[16];
    usb_local_memset(hub_desc_buf, 0, sizeof(hub_desc_buf));

    usb_setup_packet_t req_hub;
    req_hub.bmRequestType = 0xA0; // Class-specific, Device recipient, IN
    req_hub.bRequest = USB_REQ_GET_DESCRIPTOR;
    req_hub.wIndex = 0;

    uint8_t num_ports = 0;
    uint8_t pwr_good = 0;

    if (hub_dev->speed >= 4) {
        // SuperSpeed Hub (USB 3.0+)
        req_hub.wValue = (USB_DT_SS_HUB << 8);
        req_hub.wLength = 12;
        int desc_res = usb_control_transfer(hub_dev, &req_hub, hub_desc_buf, 12);
        if (desc_res != 0) {
            log_error("HUB", "Failed to read SuperSpeed Hub Descriptor on Port %u (error %d)",
                      hub_dev->port_num, desc_res);
            return -2;
        }
        num_ports = hub_desc_buf[2];
        pwr_good = hub_desc_buf[5];
    } else {
        // USB 2.0 Hub
        req_hub.wValue = (USB_DT_HUB << 8);
        req_hub.wLength = 9;
        int desc_res = usb_control_transfer(hub_dev, &req_hub, hub_desc_buf, 9);
        if (desc_res != 0) {
            req_hub.wLength = 7;
            desc_res = usb_control_transfer(hub_dev, &req_hub, hub_desc_buf, 7);
        }
        if (desc_res != 0) {
            log_error("HUB", "Failed to read USB 2.0 Hub Descriptor on Port %u (error %d)",
                      hub_dev->port_num, desc_res);
            return -2;
        }
        num_ports = hub_desc_buf[2];
        pwr_good = hub_desc_buf[5];
    }

    log_info("HUB", "Hub on Root Port %u (Slot %u): Found %u downstream ports (PwrOn2PwrGood: %u ms)",
             hub_dev->port_num, hub_dev->slot_id, num_ports, (uint32_t)pwr_good * 2);

    if (num_ports == 0 || num_ports > 15) {
        num_ports = (num_ports > 15) ? 15 : 4;
    }

    // 2. Evaluate Context on xHC to inform hardware that this slot is a Hub
    uint8_t *hub_input_ctx = (uint8_t *)kmalloc_aligned(4096, 64);
    usb_local_memset(hub_input_ctx, 0, 4096);
    uint8_t ctx_sz = ctrl->context_size ? ctrl->context_size : 32;

    // Add bit 0 (Slot Context)
    *(uint32_t *)(hub_input_ctx + 4) = (1U << 0);

    void *hub_dev_ctx = (void *)(uintptr_t)ctrl->dcbaa[hub_dev->slot_id];
    uint8_t *slot_ctx_bytes = hub_input_ctx + ctx_sz;
    if (hub_dev_ctx) {
        usb_local_memcpy(slot_ctx_bytes, hub_dev_ctx, ctx_sz);
    }
    uint32_t *hub_slot_ctx = (uint32_t *)slot_ctx_bytes;
    hub_slot_ctx[0] = (hub_slot_ctx[0] & ~(0x0FU << 20)) | ((uint32_t)hub_dev->speed << 20);
    hub_slot_ctx[0] |= (1U << 26); // Set Hub flag (bit 26)
    if ((hub_slot_ctx[0] >> 27) == 0) hub_slot_ctx[0] |= (1U << 27); // Ensure Context Entries >= 1
    hub_slot_ctx[1] = (hub_slot_ctx[1] & 0x0000FFFFU) | ((uint32_t)hub_dev->port_num << 16) | ((uint32_t)num_ports << 24);

    xhci_trb_t eval_cmd;
    eval_cmd.parameter = (uintptr_t)hub_input_ctx;
    eval_cmd.status = 0;
    eval_cmd.control = TRB_TYPE(TRB_EVAL_CTX_CMD) | ((uint32_t)hub_dev->slot_id << 24);
    xhci_trb_t eval_evt;
    int eval_res = xhci_send_command(ctrl, &eval_cmd, &eval_evt);
    if (eval_res != 0) {
        log_info("HUB", "Evaluate Context for Hub Slot %u returned code %d (proceeding...)",
                 hub_dev->slot_id, eval_res);
    } else {
        log_info("HUB", "Hub Slot %u successfully registered with xHCI controller.", hub_dev->slot_id);
    }

    // 3. SuperSpeed Hub Depth (required by USB 3.0 spec 10.14.2.9)
    if (hub_dev->speed >= 4) {
        usb_setup_packet_t req_depth;
        req_depth.bmRequestType = 0x20; // Host-to-Device, Class, Device
        req_depth.bRequest = HUB_SET_DEPTH;
        req_depth.wValue = 0; // Tier 1 hub depth is 0
        req_depth.wIndex = 0;
        req_depth.wLength = 0;
        usb_control_transfer(hub_dev, &req_depth, NULL, 0);
    }

    // 4. Power on all downstream ports
    for (uint8_t p = 1; p <= num_ports; p++) {
        usb_setup_packet_t req_pwr;
        req_pwr.bmRequestType = 0x23; // Host-to-Device, Class, Other/Port
        req_pwr.bRequest = USB_REQ_SET_FEATURE;
        req_pwr.wValue = HUB_FEATURE_PORT_POWER;
        req_pwr.wIndex = p;
        req_pwr.wLength = 0;
        usb_control_transfer(hub_dev, &req_pwr, NULL, 0);
    }

    // Wait for power stabilization
    uint32_t settle_ms = (pwr_good > 0) ? (uint32_t)pwr_good * 2 : 50;
    if (settle_ms < 50) settle_ms = 50;
    mdelay(settle_ms);

    // 5. Probe each downstream port
    for (uint8_t p = 1; p <= num_ports; p++) {
        usb_setup_packet_t req_st;
        req_st.bmRequestType = 0xA3; // Device-to-Host, Class, Other/Port
        req_st.bRequest = USB_REQ_GET_STATUS;
        req_st.wValue = 0;
        req_st.wIndex = p;
        req_st.wLength = 4;
        uint32_t port_st = 0;
        int st_res = usb_control_transfer(hub_dev, &req_st, &port_st, 4);
        if (st_res != 0) {
            mdelay(25);
            st_res = usb_control_transfer(hub_dev, &req_st, &port_st, 4);
            if (st_res != 0) {
                continue;
            }
        }

        uint16_t status = (uint16_t)(port_st & 0xFFFF);

        // Bit 0: PORT_CONNECTION
        if (!(status & (1U << HUB_FEATURE_PORT_CONNECTION))) {
            continue; // Port is unoccupied
        }

        log_info("HUB", "Downstream Port %u: Connected device detected! Status: 0x%04X", p, status);

        uint8_t child_speed = 1; // Default Full-Speed

        if (hub_dev->speed >= 4) {
            // SuperSpeed Hub downstream port
            child_speed = 4; // SuperSpeed (5 Gbps)
            // If port is not yet enabled/in U0, trigger PORT_RESET
            if (!(status & (1U << HUB_FEATURE_PORT_ENABLE))) {
                usb_setup_packet_t req_rst;
                req_rst.bmRequestType = 0x23;
                req_rst.bRequest = USB_REQ_SET_FEATURE;
                req_rst.wValue = HUB_FEATURE_PORT_RESET;
                req_rst.wIndex = p;
                req_rst.wLength = 0;
                usb_control_transfer(hub_dev, &req_rst, NULL, 0);

                for (int w = 0; w < 20; w++) {
                    mdelay(5);
                    usb_control_transfer(hub_dev, &req_st, &port_st, 4);
                    if (port_st & (1U << HUB_FEATURE_PORT_ENABLE)) break;
                }

                usb_setup_packet_t req_clr;
                req_clr.bmRequestType = 0x23;
                req_clr.bRequest = USB_REQ_CLEAR_FEATURE;
                req_clr.wValue = HUB_FEATURE_C_PORT_RESET;
                req_clr.wIndex = p;
                req_clr.wLength = 0;
                usb_control_transfer(hub_dev, &req_clr, NULL, 0);
            }
        } else {
            // USB 2.0 Hub downstream port: trigger Port Reset to enable & determine speed
            usb_setup_packet_t req_rst;
            req_rst.bmRequestType = 0x23;
            req_rst.bRequest = USB_REQ_SET_FEATURE;
            req_rst.wValue = HUB_FEATURE_PORT_RESET;
            req_rst.wIndex = p;
            req_rst.wLength = 0;
            usb_control_transfer(hub_dev, &req_rst, NULL, 0);

            // Wait for port reset to complete (PORT_RESET bit 4 clears)
            uint32_t st2 = 0;
            for (int w = 0; w < 20; w++) {
                mdelay(10);
                usb_control_transfer(hub_dev, &req_st, &st2, 4);
                if (!(st2 & (1U << 4))) break;
            }

            // Acknowledge reset change
            usb_setup_packet_t req_clr;
            req_clr.bmRequestType = 0x23;
            req_clr.bRequest = USB_REQ_CLEAR_FEATURE;
            req_clr.wValue = HUB_FEATURE_C_PORT_RESET;
            req_clr.wIndex = p;
            req_clr.wLength = 0;
            usb_control_transfer(hub_dev, &req_clr, NULL, 0);

            // Query speed from status
            uint16_t status2 = (uint16_t)(st2 & 0xFFFF);
            if (status2 & (1U << 10)) {
                child_speed = 3; // High-Speed (480 Mbps)
            } else if (status2 & (1U << 9)) {
                child_speed = 2; // Low-Speed (1.5 Mbps)
            } else {
                child_speed = 1; // Full-Speed (12 Mbps)
            }
        }

        const char *sp_str = (child_speed == 4) ? "SuperSpeed" :
                             ((child_speed == 3) ? "High-Speed" :
                             ((child_speed == 1) ? "Full-Speed" : "Low-Speed"));
        log_info("HUB", "Downstream Port %u: Port reset complete. Link speed: %s (%u)",
                 p, sp_str, child_speed);

        // 6. Enable Slot in xHCI
        uint8_t child_slot = 0;
        int slot_res = xhci_enable_slot(ctrl, &child_slot);
        if (slot_res != 0 || child_slot == 0) {
            log_error("HUB", "Enable Slot failed for downstream device on Hub Port %u!", p);
            continue;
        }

        // Allocate Device Context
        void *child_dev_ctx = kmalloc_aligned(4096, 64);
        ctrl->dcbaa[child_slot] = (uintptr_t)child_dev_ctx;

        // Allocate Input Context
        uint8_t *child_in_ctx = (uint8_t *)kmalloc_aligned(4096, 64);
        usb_local_memset(child_in_ctx, 0, 4096);

        // Add Slot (bit 0) and EP0 (bit 1)
        *(uint32_t *)(child_in_ctx + 4) = (1U << 0) | (1U << 1);

        // Slot Context
        uint32_t *c_slot_ctx = (uint32_t *)(child_in_ctx + ctx_sz);
        uint32_t route_string = (p & 0x0FU); // Tier-1 downstream port
        c_slot_ctx[0] = route_string | ((uint32_t)child_speed << 20) | (1U << 27);
        c_slot_ctx[1] = ((uint32_t)hub_dev->port_num << 16); // Root Hub Port Number
        // Per xHCI Spec 6.2.2: Parent Hub Slot ID & Port in DWORD 2 (tt_info)
        // MUST only be set if the device is Low/Full-speed attached to a High-speed hub (Transaction Translator).
        // For High-Speed (speed 3) and SuperSpeed (speed 4), this field MUST be 0 to prevent Parameter Error (CC=17).
        if (child_speed < 3 && hub_dev->speed == 3) {
            c_slot_ctx[2] = ((uint32_t)hub_dev->slot_id & 0xFF) | (((uint32_t)p & 0xFF) << 8);
        } else {
            c_slot_ctx[2] = 0;
        }
        uint32_t *c_ep0_ctx = (uint32_t *)(child_in_ctx + 2 * ctx_sz);
        uint16_t c_max_packet = (child_speed >= 4) ? 512 : ((child_speed == 3) ? 64 : 8);
        c_ep0_ctx[0] = 0;
        c_ep0_ctx[1] = (3U << 1) | (4U << 3) | ((uint32_t)c_max_packet << 16);

        // Allocate EP0 Transfer Ring
        usb_device_t child_dev;
        usb_local_memset(&child_dev, 0, sizeof(child_dev));
        child_dev.ctrl = ctrl;
        child_dev.slot_id = child_slot;
        child_dev.port_num = hub_dev->port_num;
        child_dev.speed = child_speed;

        uint32_t ep0_bytes = EP_RING_TRBS * sizeof(xhci_trb_t);
        child_dev.ep0_ring = (xhci_trb_t *)kmalloc_aligned(ep0_bytes, 64);
        usb_local_memset(child_dev.ep0_ring, 0, ep0_bytes);
        child_dev.ep0_ring[EP_RING_TRBS - 1].parameter = (uintptr_t)child_dev.ep0_ring;
        child_dev.ep0_ring[EP_RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE_CYCLE;
        child_dev.ep0_enqueue_idx = 0;
        child_dev.ep0_cycle_state = 1;

        *(uint64_t *)(&c_ep0_ctx[2]) = (uintptr_t)child_dev.ep0_ring | 1U;
        c_ep0_ctx[4] = 8;

        // 7. Issue Address Device Command
        xhci_trb_t c_addr_cmd;
        c_addr_cmd.parameter = (uintptr_t)child_in_ctx;
        c_addr_cmd.status = 0;
        c_addr_cmd.control = TRB_TYPE(TRB_ADDRESS_DEV_CMD) | ((uint32_t)child_slot << 24);

        xhci_trb_t c_addr_evt;
        int c_addr_res = xhci_send_command(ctrl, &c_addr_cmd, &c_addr_evt);
        if (c_addr_res != 0) {
            log_error("HUB", "Address Device failed for device on Hub Port %u (code %d)!", p, c_addr_res);
            xhci_disable_slot(ctrl, child_slot);
            continue;
        }

        log_info("HUB", "Device on Hub Downstream Port %u Addressed Successfully (Slot %u)!", p, child_slot);

        // 8. Get Device Descriptor
        usb_setup_packet_t c_req_dev;
        c_req_dev.bmRequestType = 0x80;
        c_req_dev.bRequest = USB_REQ_GET_DESCRIPTOR;
        c_req_dev.wValue = (USB_DESC_DEVICE << 8);
        c_req_dev.wIndex = 0;

        // Two-phase descriptor read for Full-Speed (speed 1) or Low-Speed (speed 2)
        // to prevent xHCI Babble Error (CC=3) when child device returns packets > 8 bytes
        if (child_speed < 3) {
            c_req_dev.wLength = 8;
            int r8 = usb_control_transfer(&child_dev, &c_req_dev, &child_dev.dev_desc, 8);
            if (r8 != 0) {
                log_error("HUB", "Failed initial 8-byte Device Descriptor on Hub Port %u (error %d)", p, r8);
                xhci_disable_slot(ctrl, child_slot);
                mdelay(20);
                continue;
            }
            uint16_t c_dev_ep0_max = child_dev.dev_desc.bMaxPacketSize0;
            if (c_dev_ep0_max > 0 && c_dev_ep0_max != c_max_packet) {
                log_info("HUB", "Updating Hub Port %u EP0 MaxPacketSize to %u via Evaluate Context...", p, c_dev_ep0_max);
                *(uint32_t *)(child_in_ctx + 0) = 0;
                *(uint32_t *)(child_in_ctx + 4) = (1U << 1);
                c_ep0_ctx[1] = (c_ep0_ctx[1] & ~0xFFFF0000U) | ((uint32_t)c_dev_ep0_max << 16);
                xhci_trb_t eval_cmd2;
                eval_cmd2.parameter = (uintptr_t)child_in_ctx;
                eval_cmd2.status = 0;
                eval_cmd2.control = TRB_TYPE(TRB_EVAL_CTX_CMD) | ((uint32_t)child_slot << 24);
                xhci_send_command(ctrl, &eval_cmd2, &c_addr_evt);
                c_max_packet = c_dev_ep0_max;
            }
        }

        c_req_dev.wLength = sizeof(usb_device_desc_t);
        int c_desc_res = usb_control_transfer(&child_dev, &c_req_dev, &child_dev.dev_desc, sizeof(usb_device_desc_t));
        if (c_desc_res != 0) {
            log_error("HUB", "Failed to retrieve Device Descriptor on Hub Port %u (error %d)", p, c_desc_res);
            xhci_disable_slot(ctrl, child_slot);
            mdelay(20);
            continue;
        }

        log_info("HUB", "Downstream Device: VID=0x%04X, PID=0x%04X, Class=0x%02X, Subclass=0x%02X, Proto=0x%02X",
                 child_dev.dev_desc.idVendor, child_dev.dev_desc.idProduct,
                 child_dev.dev_desc.bDeviceClass, child_dev.dev_desc.bDeviceSubClass, child_dev.dev_desc.bDeviceProtocol);

        // Update EP0 MaxPacket if needed (SuperSpeed exponent or larger packet)
        uint16_t c_dev_ep0_max = child_dev.dev_desc.bMaxPacketSize0;
        if (child_speed >= 4) {
            c_dev_ep0_max = (1U << child_dev.dev_desc.bMaxPacketSize0);
        }
        if (c_dev_ep0_max > 0 && c_dev_ep0_max != c_max_packet) {
            *(uint32_t *)(child_in_ctx + 0) = 0;
            *(uint32_t *)(child_in_ctx + 4) = (1U << 1);
            c_ep0_ctx[1] = (c_ep0_ctx[1] & ~0xFFFF0000U) | ((uint32_t)c_dev_ep0_max << 16);
            xhci_trb_t eval_cmd2;
            eval_cmd2.parameter = (uintptr_t)child_in_ctx;
            eval_cmd2.status = 0;
            eval_cmd2.control = TRB_TYPE(TRB_EVAL_CTX_CMD) | ((uint32_t)child_slot << 24);
            xhci_send_command(ctrl, &eval_cmd2, &c_addr_evt);
        }

        // Query String Identifiers
        char c_mfg[48] = {0};
        char c_prod[48] = {0};
        if (child_dev.dev_desc.iManufacturer) usb_get_string_descriptor(&child_dev, child_dev.dev_desc.iManufacturer, c_mfg, sizeof(c_mfg));
        if (child_dev.dev_desc.iProduct) usb_get_string_descriptor(&child_dev, child_dev.dev_desc.iProduct, c_prod, sizeof(c_prod));
        if (c_mfg[0] || c_prod[0]) {
            log_info("HUB", "Downstream Ident: [%s] [%s]", c_mfg, c_prod);
        }

        // 9. Get Configuration Descriptor
        usb_setup_packet_t c_req_cfg;
        c_req_cfg.bmRequestType = 0x80;
        c_req_cfg.bRequest = USB_REQ_GET_DESCRIPTOR;
        c_req_cfg.wValue = (USB_DESC_CONFIGURATION << 8);
        c_req_cfg.wIndex = 0;
        c_req_cfg.wLength = 9;

        usb_config_desc_t c_cfg_hdr;
        int c_cfg_res = usb_control_transfer(&child_dev, &c_req_cfg, &c_cfg_hdr, 9);
        if (c_cfg_res != 0) {
            log_error("HUB", "Failed to retrieve Config Header on Hub Port %u (error %d)", p, c_cfg_res);
            xhci_disable_slot(ctrl, child_slot);
            mdelay(20);
            continue;
        }

        uint16_t c_total = c_cfg_hdr.wTotalLength;
        if (c_total > sizeof(child_dev.config_buf)) c_total = sizeof(child_dev.config_buf);
        c_req_cfg.wLength = c_total;
        c_cfg_res = usb_control_transfer(&child_dev, &c_req_cfg, child_dev.config_buf, c_total);
        if (c_cfg_res != 0) {
            log_error("HUB", "Failed to retrieve Config Descriptor on Hub Port %u (error %d)", p, c_cfg_res);
            xhci_disable_slot(ctrl, child_slot);
            mdelay(20);
            continue;
        }
        child_dev.config_len = c_total;

        // 10. Parse Interfaces & Endpoints
        uint8_t *c_ptr = child_dev.config_buf;
        uint8_t *c_end = c_ptr + c_total;
        bool is_msc_iface = false;
        uint8_t cur_iface = 0;

        while (c_ptr + 2 <= c_end) {
            uint8_t len = c_ptr[0];
            uint8_t type = c_ptr[1];
            if (len == 0 || c_ptr + len > c_end) break;

            if (type == USB_DESC_INTERFACE) {
                usb_interface_desc_t *iface = (usb_interface_desc_t *)c_ptr;
                cur_iface = iface->bInterfaceNumber;
                if (iface->bInterfaceClass == USB_CLASS_MASS_STORAGE && iface->bInterfaceProtocol == 0x50) {
                    is_msc_iface = true;
                    child_dev.has_msc = true;
                    child_dev.msc_iface_num = cur_iface;
                    log_info("HUB", "  Hub Port %u: Found USB Mass Storage Interface %u!", p, cur_iface);
                } else {
                    is_msc_iface = false;
                }
            } else if (type == USB_DESC_ENDPOINT && is_msc_iface) {
                usb_endpoint_desc_t *ep = (usb_endpoint_desc_t *)c_ptr;
                uint8_t ep_type = ep->bmAttributes & 0x03;
                if (ep_type == 2) { // Bulk
                    if (ep->bEndpointAddress & 0x80) {
                        child_dev.msc_bulk_in_ep = ep->bEndpointAddress;
                        child_dev.msc_bulk_in_max_packet = ep->wMaxPacketSize;
                        log_info("HUB", "    -> MSC Bulk IN EP: 0x%02X (MaxPacket: %u)", ep->bEndpointAddress, ep->wMaxPacketSize);
                    } else {
                        child_dev.msc_bulk_out_ep = ep->bEndpointAddress;
                        child_dev.msc_bulk_out_max_packet = ep->wMaxPacketSize;
                        log_info("HUB", "    -> MSC Bulk OUT EP: 0x%02X (MaxPacket: %u)", ep->bEndpointAddress, ep->wMaxPacketSize);
                    }
                }
            }
            c_ptr += len;
        }

        // 11. Activate Configuration 1
        usb_setup_packet_t c_set_cfg;
        c_set_cfg.bmRequestType = 0x00;
        c_set_cfg.bRequest = USB_REQ_SET_CONFIGURATION;
        c_set_cfg.wValue = 1;
        c_set_cfg.wIndex = 0;
        c_set_cfg.wLength = 0;
        usb_control_transfer(&child_dev, &c_set_cfg, NULL, 0);

        // 12. If Mass Storage: configure bulk endpoints & initialize SCSI device
        if (child_dev.has_msc) {
            int msc_cfg = usb_configure_bulk_endpoints(&child_dev,
                                                       child_dev.msc_bulk_in_ep, child_dev.msc_bulk_in_max_packet,
                                                       child_dev.msc_bulk_out_ep, child_dev.msc_bulk_out_max_packet);
            if (msc_cfg == 0) {
                int msc_init = usb_msc_init_device(&child_dev);
                if (msc_init == 0) {
                    log_info("HUB", "Mass Storage device on Hub Port %u fully initialized!", p);
                    if (out_msc_dev && !found_any_msc) {
                        usb_local_memcpy(out_msc_dev, &child_dev, sizeof(usb_device_t));
                    }
                    if (g_hub_msc_callback) {
                        g_hub_msc_callback(&child_dev, hub_dev->port_num);
                    }
                    found_any_msc = true;
                } else {
                    log_error("HUB", "usb_msc_init_device failed on Hub Port %u (code %d)", p, msc_init);
                }
            }
        }
    }

    return found_any_msc ? 0 : -1;
}

