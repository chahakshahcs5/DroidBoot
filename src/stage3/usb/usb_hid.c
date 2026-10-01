#include "usb_hid.h"
#include "../core/printf.h"
#include "../ui/menu.h"
#include "../../include/io.h"
#include "../memory/memory.h"
#include <stddef.h>

// Internal state for a single USB HID boot keyboard
static bool         kbd_available = false;
static usb_device_t kbd_dev_storage;
static usb_device_t *kbd_dev = NULL;
static xhci_controller_t *kbd_ctrl = NULL;
static uint8_t      kbd_int_ep = 0;        // Interrupt IN endpoint address
static uint16_t     kbd_int_max_packet = 8; // Max packet size (usually 8 for boot kbd)
static uint8_t      kbd_prev_keys[6] = {0}; // Previously pressed keys (for debounce)
static uint8_t      kbd_report_buf[8] __attribute__((aligned(64))); // 8-byte boot report DMA buffer
static bool         kbd_trb_pending = false; // Interrupt IN transfer in-flight flag

static void k_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

// HID Usage ID (scan code) to ASCII translation table for boot keyboard
// Covers standard US keyboard layout. Index = HID usage ID, value = ASCII char.
static const char hid_usage_to_ascii[128] = {
    // 0x00-0x03: No event, Error, POST fail, Undefined
    0, 0, 0, 0,
    // 0x04-0x1D: a-z
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
    'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    // 0x1E-0x26: 1-9
    '1', '2', '3', '4', '5', '6', '7', '8', '9',
    // 0x27: 0
    '0',
    // 0x28: Enter
    '\n',
    // 0x29: Escape
    0,  // Handled separately as KEY_ESC
    // 0x2A: Backspace
    '\b',
    // 0x2B: Tab
    '\t',
    // 0x2C: Space
    ' ',
    // 0x2D-0x38: - = [ ] \ # ; ' ` , . /
    '-', '=', '[', ']', '\\', '#', ';', '\'', '`', ',', '.', '/',
    // 0x39-0x3F: Caps Lock, F1-F6
    0, 0, 0, 0, 0, 0, 0,
    // 0x40-0x45: F7-F12
    0, 0, 0, 0, 0, 0,
    // 0x46-0x4E: Print Screen, Scroll Lock, Pause, Insert, Home, PgUp, Delete, End, PgDn
    0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x4F-0x52: Right Arrow, Left Arrow, Down Arrow, Up Arrow
    0, 0, 0, 0,  // Handled separately as KEY_RIGHT/LEFT/DOWN/UP
    // 0x53+: Num Lock, etc.
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

// Convert HID usage ID to menu key code
static int hid_usage_to_key(uint8_t usage) {
    if (usage == 0) return -1; // No key

    // Arrow keys
    if (usage == 0x52) return KEY_UP;
    if (usage == 0x51) return KEY_DOWN;
    if (usage == 0x50) return KEY_LEFT;
    if (usage == 0x4F) return KEY_RIGHT;

    // Special keys
    if (usage == 0x28) return '\n';     // Enter
    if (usage == 0x58) return '\n';     // Keypad Enter
    if (usage == 0x29) return KEY_ESC;  // Escape
    if (usage == 0x2A) return '\b';     // Backspace
    if (usage == 0x4A) return KEY_ESC;  // Home (mapped to Escape for menu navigation)

    // Alphanumeric from lookup table
    if (usage < sizeof(hid_usage_to_ascii) && hid_usage_to_ascii[usage] != 0) {
        return (int)hid_usage_to_ascii[usage];
    }

    return -1; // Unknown/unsupported key
}

// Check if a key usage is new (not in previous report)
static bool is_new_key(uint8_t usage, const uint8_t *prev_keys) {
    for (int i = 0; i < 6; i++) {
        if (prev_keys[i] == usage) return false;
    }
    return true;
}

bool usb_keyboard_is_available(void) {
    return kbd_available;
}

bool usb_keyboard_try_init(xhci_controller_t *ctrl, usb_device_t *dev) {
    if (!ctrl || !dev || dev->is_disconnected) return false;
    if (kbd_available) return false; // Already have a keyboard

    // Parse configuration descriptor to find HID Boot Keyboard interface
    uint8_t *cfg = dev->config_buf;
    uint16_t cfg_len = dev->config_len;
    if (cfg_len < 9) return false;

    uint16_t offset = 0;
    bool found_kbd_iface = false;
    uint8_t kbd_iface_num = 0;

    while (offset + 2 <= cfg_len) {
        uint8_t desc_len = cfg[offset];
        uint8_t desc_type = cfg[offset + 1];
        if (desc_len < 2 || offset + desc_len > cfg_len) break;

        // Interface descriptor (type 0x04)
        if (desc_type == 0x04 && desc_len >= 9) {
            uint8_t bInterfaceClass = cfg[offset + 5];
            uint8_t bInterfaceSubClass = cfg[offset + 6];
            uint8_t bInterfaceProtocol = cfg[offset + 7];

            if (bInterfaceClass == USB_HID_INTF_CLASS &&
                bInterfaceSubClass == USB_HID_INTF_SUBCLASS &&
                bInterfaceProtocol == USB_HID_PROTOCOL_KBD) {
                found_kbd_iface = true;
                kbd_iface_num = cfg[offset + 2]; // bInterfaceNumber
                log_info("HID", "Found USB HID Boot Keyboard interface %u on Port %u (Slot %u)",
                         kbd_iface_num, dev->port_num, dev->slot_id);
            } else {
                if (found_kbd_iface) break; // Past our keyboard interface
            }
        }

        // Endpoint descriptor (type 0x05) — find Interrupt IN endpoint
        if (desc_type == 0x05 && desc_len >= 7 && found_kbd_iface) {
            uint8_t ep_addr = cfg[offset + 2];
            uint8_t ep_attr = cfg[offset + 3];
            uint16_t ep_max = (uint16_t)cfg[offset + 4] | ((uint16_t)cfg[offset + 5] << 8);

            // Interrupt IN: direction bit 7 = IN (1), transfer type bits 0-1 = Interrupt (0x03)
            if ((ep_addr & 0x80) && (ep_attr & 0x03) == 0x03) {
                kbd_int_ep = ep_addr;
                kbd_int_max_packet = ep_max;
                log_info("HID", "  Interrupt IN Endpoint: 0x%02X (MaxPacket: %u bytes)",
                         kbd_int_ep, kbd_int_max_packet);
                break;
            }
        }

        offset += desc_len;
    }

    if (!found_kbd_iface || kbd_int_ep == 0) {
        return false;
    }

    return (usb_keyboard_init(ctrl, dev) == 0);
}

int usb_keyboard_init(xhci_controller_t *ctrl, usb_device_t *dev) {
    if (!ctrl || !dev) return -1;

    // 1. Set Boot Protocol (bRequest=SET_PROTOCOL, wValue=0=Boot Protocol)
    usb_setup_packet_t set_proto;
    set_proto.bmRequestType = 0x21; // Host-to-Device, Class, Interface
    set_proto.bRequest = USB_HID_SET_PROTOCOL;
    set_proto.wValue = USB_HID_BOOT_PROTOCOL;
    set_proto.wIndex = 0; // Interface number
    set_proto.wLength = 0;
    int res = usb_control_transfer(dev, &set_proto, NULL, 0);
    if (res != 0) {
        log_warn("HID", "SET_PROTOCOL(Boot) returned %d (non-fatal, some keyboards ignore this)", res);
    }

    // 2. Configure the Interrupt IN endpoint on the xHCI controller
    // We use the same bulk endpoint configuration path (it works for interrupt endpoints
    // on xHCI since the hardware handles the endpoint type from the context).
    uint8_t ep_num = kbd_int_ep & 0x0F;
    uint8_t ep_ctx_idx = ep_num * 2 + 1; // IN endpoints use odd context indices

    // Allocate transfer ring for the interrupt endpoint
    if (!dev->ep_rings[ep_ctx_idx]) {
        dev->ep_rings[ep_ctx_idx] = (xhci_trb_t *)kmalloc_aligned(256 * sizeof(xhci_trb_t), 64);
        if (!dev->ep_rings[ep_ctx_idx]) {
            log_error("HID", "Failed to allocate Interrupt IN transfer ring!");
            return false;
        }
        for (int i = 0; i < 256; i++) {
            dev->ep_rings[ep_ctx_idx][i].parameter = 0;
            dev->ep_rings[ep_ctx_idx][i].status = 0;
            dev->ep_rings[ep_ctx_idx][i].control = 0;
        }
        // Link TRB at end
        dev->ep_rings[ep_ctx_idx][255].parameter = (uintptr_t)dev->ep_rings[ep_ctx_idx];
        dev->ep_rings[ep_ctx_idx][255].control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE_CYCLE;
        dev->ep_enqueue_idx[ep_ctx_idx] = 0;
        dev->ep_cycle_state[ep_ctx_idx] = 1;
    }

    // 3. Configure Endpoint on xHCI via Configure Endpoint command
    uint8_t *input_ctx = (uint8_t *)kmalloc_aligned(4096, 64);
    if (!input_ctx) return false;
    for (int i = 0; i < 4096; i++) ((uint8_t *)input_ctx)[i] = 0;

    uint8_t ctx_sz = ctrl->context_size ? ctrl->context_size : 32;

    // Input Control Context: Add flag for this endpoint
    uint32_t *icc = (uint32_t *)input_ctx;
    icc[1] = (1U << 0) | (1U << ep_ctx_idx); // Add Slot + this EP

    // Copy existing Slot Context
    void *dev_ctx = (void *)(uintptr_t)ctrl->dcbaa[dev->slot_id];
    if (dev_ctx) {
        for (uint8_t i = 0; i < ctx_sz; i++) {
            input_ctx[ctx_sz + i] = ((uint8_t *)dev_ctx)[i];
        }
    }
    // Update Context Entries in slot context
    uint32_t *slot_ctx = (uint32_t *)(input_ctx + ctx_sz);
    uint32_t cur_entries = (slot_ctx[0] >> 27) & 0x1F;
    if (ep_ctx_idx > cur_entries) {
        slot_ctx[0] = (slot_ctx[0] & ~(0x1FU << 27)) | ((uint32_t)ep_ctx_idx << 27);
    }

    // Configure Endpoint Context
    uint8_t *ep_ctx_ptr = input_ctx + ctx_sz * (ep_ctx_idx + 1);
    uint32_t *ep_ctx_dw = (uint32_t *)ep_ctx_ptr;

    // EP Info 1: Interval = 8 (128ms for boot keyboard), CErr = 3, EP Type = Interrupt IN (7)
    // Interval encoding: for FS/LS interrupt endpoints, interval = exponent of 2^(interval-1) * 125us
    uint32_t interval = 6; // 2^(6-1) * 125us = 4ms polling
    if (dev->speed <= 2) interval = 10; // Slower polling for low-speed devices
    ep_ctx_dw[0] = (interval << 16) | (3U << 1); // CErr = 3
    // EP Info 2: EP Type = Interrupt IN (7), MaxPacketSize
    ep_ctx_dw[1] = (7U << 1) | ((uint32_t)kbd_int_max_packet << 16);
    // TR Dequeue Pointer with DCS = 1
    *(uint64_t *)(ep_ctx_ptr + 8) = (uintptr_t)dev->ep_rings[ep_ctx_idx] | 1U;
    // Average TRB Length
    ep_ctx_dw[4] = kbd_int_max_packet;

    // Send Configure Endpoint command
    xhci_trb_t cfg_cmd = {0};
    cfg_cmd.parameter = (uintptr_t)input_ctx;
    cfg_cmd.control = TRB_TYPE(TRB_CONFIG_EP_CMD) | ((uint32_t)dev->slot_id << 24);
    xhci_trb_t cfg_evt = {0};
    int cfg_res = xhci_send_command(ctrl, &cfg_cmd, &cfg_evt);
    if (cfg_res != 0) {
        log_warn("HID", "Configure Endpoint for keyboard returned %d (trying to proceed anyway)", cfg_res);
    }

    // 4. Mark keyboard as available
    k_memcpy(&kbd_dev_storage, dev, sizeof(usb_device_t));
    kbd_dev = &kbd_dev_storage;
    kbd_ctrl = ctrl;
    kbd_available = true;
    kbd_trb_pending = false;
    for (int i = 0; i < 6; i++) kbd_prev_keys[i] = 0;

    log_info("HID", "USB HID Boot Keyboard initialized on Port %u, EP 0x%02X. Ready for input!",
             dev->port_num, kbd_int_ep);
    return 0;
}

int usb_keyboard_poll(int *out_key) {
    if (!kbd_available || !kbd_dev || !kbd_ctrl || kbd_dev->is_disconnected) {
        kbd_trb_pending = false;
        return -1;
    }

    uint8_t ep_num = kbd_int_ep & 0x0F;
    uint8_t ep_ctx_idx = ep_num * 2 + 1;

    if (!kbd_dev->ep_rings[ep_ctx_idx]) return -1;

    // Only enqueue an interrupt TRB if one isn't already pending
    if (!kbd_trb_pending) {
        volatile xhci_trb_t *ring = kbd_dev->ep_rings[ep_ctx_idx];
        uint32_t idx = kbd_dev->ep_enqueue_idx[ep_ctx_idx];
        volatile xhci_trb_t *trb = &ring[idx];

        for (int i = 0; i < 8; i++) kbd_report_buf[i] = 0;

        trb->parameter = (uintptr_t)kbd_report_buf;
        trb->status = (uint32_t)kbd_int_max_packet;
        uint32_t ctrl_flags = TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP;
        if (kbd_dev->ep_cycle_state[ep_ctx_idx]) ctrl_flags |= TRB_CYCLE;
        trb->control = ctrl_flags;

        idx++;
        if (idx >= 255) {
            ring[255].control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE_CYCLE |
                                (kbd_dev->ep_cycle_state[ep_ctx_idx] ? TRB_CYCLE : 0);
            kbd_dev->ep_cycle_state[ep_ctx_idx] ^= 1;
            idx = 0;
        }
        kbd_dev->ep_enqueue_idx[ep_ctx_idx] = idx;

        *(volatile uint32_t *)(kbd_ctrl->db_regs + kbd_dev->slot_id * 4) = ep_ctx_idx;
        kbd_trb_pending = true;
    }

    // Inspect the primary event ring (non-blocking)
    volatile xhci_trb_t *evt = &kbd_ctrl->event_ring[kbd_ctrl->event_dequeue_idx];
    uint32_t cycle = evt->control & 1U;
    if (cycle != kbd_ctrl->event_cycle_state) {
        return -1; // No event ready
    }

    uint32_t evt_type = (evt->control >> 10) & 0x3F;
    uint8_t  evt_slot = (uint8_t)((evt->control >> 24) & 0xFF);
    uint8_t  evt_cc = (uint8_t)((evt->status >> 24) & 0xFF);

    // Only consume transfer events for our keyboard slot — leave all other
    // events intact on the event ring for other drivers (e.g. phone MSC, ADB, commands)
    if (evt_type != TRB_TRANSFER_EVENT || evt_slot != kbd_dev->slot_id) {
        return -1;
    }

    // Advance event ring dequeue pointer
    kbd_ctrl->event_dequeue_idx++;
    if (kbd_ctrl->event_dequeue_idx == XHCI_EVENT_RING_TRBS) {
        kbd_ctrl->event_dequeue_idx = 0;
        kbd_ctrl->event_cycle_state ^= 1;
    }
    uintptr_t intr0 = kbd_ctrl->rt_regs + 0x20;
    uintptr_t erdp = (uintptr_t)&kbd_ctrl->event_ring[kbd_ctrl->event_dequeue_idx];
    xhci_write64(intr0 + XHCI_INTR_ERDP, erdp | XHCI_ERDP_EHB);

    // Process keyboard transfer completion
    if (evt_type == TRB_TRANSFER_EVENT && evt_slot == kbd_dev->slot_id) {
        kbd_trb_pending = false;

        if (evt_cc == 1 || evt_cc == 13) {
            uint8_t *report = kbd_report_buf;
            for (int k = 2; k < 8; k++) {
                uint8_t usage = report[k];
                if (usage == 0 || usage == 1) continue;
                if (is_new_key(usage, kbd_prev_keys)) {
                    for (int j = 0; j < 6; j++) kbd_prev_keys[j] = report[j + 2];
                    int key = hid_usage_to_key(usage);
                    if (key >= 0 && out_key) {
                        *out_key = key;
                        return 0;
                    }
                }
            }
            for (int j = 0; j < 6; j++) kbd_prev_keys[j] = report[j + 2];
        }
    }

    return -1;
}
