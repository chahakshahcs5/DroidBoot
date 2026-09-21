#include "xhci.h"
#include "../../include/io.h"
#include "../core/printf.h"
#include "../memory/memory.h"
#include <stddef.h>



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

static const char *get_speed_name(uint8_t speed) {
    switch (speed) {
        case 1: return "Full-Speed (12 Mbps)";
        case 2: return "Low-Speed (1.5 Mbps)";
        case 3: return "High-Speed (480 Mbps)";
        case 4: return "SuperSpeed (5 Gbps)";
        case 5: return "SuperSpeedPlus (10 Gbps)";
        default: return "Unknown Speed";
    }
}

static void xhci_bios_handoff(xhci_controller_t *ctrl) {
    uint32_t hccparams1 = xhci_read32(ctrl->mmio_base + XHCI_CAP_HCCPARAMS1);
    uint32_t xecp = (hccparams1 >> 16) & 0xFFFF;
    if (xecp == 0) return;

    uintptr_t ext_cap = ctrl->mmio_base + (xecp << 2);

    while (ext_cap) {
        uint32_t val = xhci_read32(ext_cap);
        uint8_t id = val & 0xFF;
        uint8_t next = (val >> 8) & 0xFF;

        if (id == 0x01) { // USB Legacy Support (USBLEGSUP)
            if (val & (1U << 16)) { // BIOS Owned Semaphore
                log_info("XHCI", "Requesting xHCI ownership from BIOS SMM...");
                xhci_write32(ext_cap, val | (1U << 24)); // Set OS Owned

                int wait_ms = 1000;
                while ((xhci_read32(ext_cap) & (1U << 16)) && --wait_ms > 0) {
                    mdelay(1);
                }

                if (wait_ms == 0) {
                    log_error("XHCI", "BIOS ownership release timed out, proceeding...");
                } else {
                    log_info("XHCI", "BIOS successfully released xHCI ownership.");
                }

                // Clear SMI enable bits in USBLEGCTLSTS (offset + 4)
                uint32_t legctl = xhci_read32(ext_cap + 4);
                legctl &= 0x1FEE0000;
                xhci_write32(ext_cap + 4, legctl);
            }
            break;
        }

        if (next == 0) break;
        ext_cap += (next << 2);
    }
}

static inline uint32_t xhci_portsc_clean(uint32_t portsc) {
    // Clear PED (bit 1), PR (bit 4), and all RW1C change bits (bits 17-23)
    // Always preserve PP (Port Power, bit 9)
    uint32_t mask = XHCI_PORT_PED | XHCI_PORT_PR | XHCI_PORT_RW1C_MASK;
    return (portsc & ~mask) | XHCI_PORT_PP;
}

int xhci_init(pci_device_t *pci_dev, xhci_controller_t *ctrl) {
    if (!pci_dev || !ctrl) return -1;

    ctrl->pci_dev = pci_dev;

    // 1. Enable Bus Master & Memory Space in PCI Command Register
    uint16_t pci_cmd = pci_read_config16(pci_dev->bus, pci_dev->dev, pci_dev->func, 0x04);
    pci_write_config16(pci_dev->bus, pci_dev->dev, pci_dev->func, 0x04, pci_cmd | 0x06);

    // 2. Map MMIO Base Address
    ctrl->mmio_base = (uintptr_t)(pci_dev->bar0 & ~0xFU);
    log_info("XHCI", "Initializing xHCI Controller at MMIO Base 0x%08X", (uint32_t)ctrl->mmio_base);

    // 3. Read Capability Registers (using 32-bit aligned MMIO read)
    uint32_t caplength_version = xhci_read32(ctrl->mmio_base + XHCI_CAP_CAPLENGTH);
    ctrl->cap_len = (uint8_t)(caplength_version & 0xFF);
    ctrl->hci_version = (uint16_t)(caplength_version >> 16);

    uint32_t hcsparams1 = xhci_read32(ctrl->mmio_base + XHCI_CAP_HCSPARAMS1);
    ctrl->max_slots = (uint8_t)(hcsparams1 & 0xFF);
    ctrl->max_intrs = (uint16_t)((hcsparams1 >> 8) & 0x7FF);
    ctrl->max_ports = (uint8_t)((hcsparams1 >> 24) & 0xFF);

    uint32_t hcsparams2 = xhci_read32(ctrl->mmio_base + XHCI_CAP_HCSPARAMS2);
    uint32_t sb_hi = (hcsparams2 >> 21) & 0x1F;
    uint32_t sb_lo = (hcsparams2 >> 27) & 0x1F;
    ctrl->max_scratchpad_bufs = (sb_hi << 5) | sb_lo;

    uint32_t hccparams1 = xhci_read32(ctrl->mmio_base + XHCI_CAP_HCCPARAMS1);
    ctrl->context_size = (hccparams1 & (1U << 2)) ? 64 : 32;

    uint32_t dboff = xhci_read32(ctrl->mmio_base + XHCI_CAP_DBOFF) & ~0x3U;
    uint32_t rtsoff = xhci_read32(ctrl->mmio_base + XHCI_CAP_RTSOFF) & ~0x1FU;

    ctrl->op_regs = ctrl->mmio_base + ctrl->cap_len;
    ctrl->db_regs = ctrl->mmio_base + dboff;
    ctrl->rt_regs = ctrl->mmio_base + rtsoff;

    log_info("XHCI", "xHCI Version %u.%u | MaxSlots: %u | MaxPorts: %u | ContextSize: %u bytes (CSZ=%u)",
             ctrl->hci_version >> 8, (ctrl->hci_version >> 4) & 0xF,
             ctrl->max_slots, ctrl->max_ports, ctrl->context_size, (hccparams1 >> 2) & 1);

    // 4. BIOS Handoff & Controller Reset
    xhci_bios_handoff(ctrl);

    // Stop controller: clear USBCMD.RS
    uint32_t cmd = xhci_read32(ctrl->op_regs + XHCI_OP_USBCMD);
    cmd &= ~XHCI_CMD_RS;
    xhci_write32(ctrl->op_regs + XHCI_OP_USBCMD, cmd);

    // Wait until controller is halted (USBSTS.HCH == 1)
    int timeout = 100;
    while (!(xhci_read32(ctrl->op_regs + XHCI_OP_USBSTS) & XHCI_STS_HCH) && --timeout > 0) {
        mdelay(1);
    }

    // Reset controller: set USBCMD.HCRST
    xhci_write32(ctrl->op_regs + XHCI_OP_USBCMD, XHCI_CMD_HCRST);
    timeout = 100;
    while ((xhci_read32(ctrl->op_regs + XHCI_OP_USBCMD) & XHCI_CMD_HCRST) && --timeout > 0) {
        mdelay(1);
    }
    if (timeout == 0) {
        log_error("XHCI", "Controller reset timed out!");
        return -2;
    }

    // Wait until Controller Not Ready (USBSTS.CNR) clears
    timeout = 100;
    while ((xhci_read32(ctrl->op_regs + XHCI_OP_USBSTS) & XHCI_STS_CNR) && --timeout > 0) {
        mdelay(1);
    }
    log_info("XHCI", "Controller Reset OK, Hardware Ready.");

    // 5. Configure Max Enabled Slots
    xhci_write32(ctrl->op_regs + XHCI_OP_CONFIG, ctrl->max_slots);

    // 6. Allocate DCBAA (Device Context Base Address Array)
    uint32_t dcbaa_size = (ctrl->max_slots + 1) * sizeof(uint64_t);
    ctrl->dcbaa = (uint64_t *)kmalloc_aligned(dcbaa_size, 64);
    if (!ctrl->dcbaa) {
        log_error("XHCI", "Failed to allocate DCBAA!");
        return -3;
    }
    for (uint32_t i = 0; i <= ctrl->max_slots; i++) {
        ctrl->dcbaa[i] = 0;
    }

    // 7. Scratchpad Buffers (if hardware requires them)
    if (ctrl->max_scratchpad_bufs > 0) {
        uint32_t sp_array_size = ctrl->max_scratchpad_bufs * sizeof(uint64_t);
        uint64_t *sp_array = (uint64_t *)kmalloc_aligned(sp_array_size, 64);
        for (uint32_t i = 0; i < ctrl->max_scratchpad_bufs; i++) {
            void *sp_page = kmalloc_aligned(4096, 4096);
            sp_array[i] = (uintptr_t)sp_page;
        }
        ctrl->dcbaa[0] = (uintptr_t)sp_array;
    }

    // Program DCBAAP register
    xhci_write64(ctrl->op_regs + XHCI_OP_DCBAAP, (uintptr_t)ctrl->dcbaa);

    // 8. Allocate Command Ring
    uint32_t cmd_ring_bytes = XHCI_CMD_RING_TRBS * sizeof(xhci_trb_t);
    ctrl->cmd_ring = (xhci_trb_t *)kmalloc_aligned(cmd_ring_bytes, 64);
    for (uint32_t i = 0; i < XHCI_CMD_RING_TRBS; i++) {
        ctrl->cmd_ring[i].parameter = 0;
        ctrl->cmd_ring[i].status = 0;
        ctrl->cmd_ring[i].control = 0;
    }
    // Set up Link TRB at the end of the ring pointing back to beginning
    uint32_t last_trb = XHCI_CMD_RING_TRBS - 1;
    ctrl->cmd_ring[last_trb].parameter = (uintptr_t)ctrl->cmd_ring;
    ctrl->cmd_ring[last_trb].control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE_CYCLE;
    ctrl->cmd_enqueue_idx = 0;
    ctrl->cmd_cycle_state = 1;

    // Program CRCR (Command Ring Control Register) with RCS = 1
    xhci_write64(ctrl->op_regs + XHCI_OP_CRCR, (uintptr_t)ctrl->cmd_ring | 1U);

    // 9. Allocate Event Ring & Event Ring Segment Table (ERST)
    uint32_t evt_ring_bytes = XHCI_EVENT_RING_TRBS * sizeof(xhci_trb_t);
    ctrl->event_ring = (xhci_trb_t *)kmalloc_aligned(evt_ring_bytes, 64);
    for (uint32_t i = 0; i < XHCI_EVENT_RING_TRBS; i++) {
        ctrl->event_ring[i].parameter = 0;
        ctrl->event_ring[i].status = 0;
        ctrl->event_ring[i].control = 0;
    }
    ctrl->event_dequeue_idx = 0;
    ctrl->event_cycle_state = 1;

    ctrl->erst = (xhci_erst_entry_t *)kmalloc_aligned(sizeof(xhci_erst_entry_t), 64);
    ctrl->erst->ring_segment_base = (uintptr_t)ctrl->event_ring;
    ctrl->erst->ring_segment_size = XHCI_EVENT_RING_TRBS;
    ctrl->erst->reserved = 0;

    // Interrupter 0 Registers (at rt_regs + 0x20)
    uintptr_t intr0 = ctrl->rt_regs + 0x20;
    xhci_write32(intr0 + XHCI_INTR_ERSTSZ, 1);
    xhci_write64(intr0 + XHCI_INTR_ERSTBA, (uintptr_t)ctrl->erst);
    xhci_write64(intr0 + XHCI_INTR_ERDP, (uintptr_t)ctrl->event_ring | XHCI_ERDP_EHB);

    // Enable interrupter 0
    xhci_write32(intr0 + XHCI_INTR_IMAN, XHCI_IMAN_IE | XHCI_IMAN_IP);

    log_info("XHCI", "Command & Event Rings Initialized.");

    // 10. Start Controller (USBCMD.RS = 1)
    cmd = xhci_read32(ctrl->op_regs + XHCI_OP_USBCMD);
    cmd |= XHCI_CMD_RS | XHCI_CMD_INTE;
    xhci_write32(ctrl->op_regs + XHCI_OP_USBCMD, cmd);

    // Wait until controller is running (USBSTS.HCH == 0)
    timeout = 100;
    while ((xhci_read32(ctrl->op_regs + XHCI_OP_USBSTS) & XHCI_STS_HCH) && --timeout > 0) {
        mdelay(1);
    }
    log_info("XHCI", "Controller Running (USBCMD.RS=1, USBSTS.HCH=0).");

    // 11. Power on all Root Hub Ports
    for (uint8_t p = 1; p <= ctrl->max_ports; p++) {
        uintptr_t port_reg = ctrl->op_regs + XHCI_OP_PORTS_BASE + (p - 1) * 0x10;
        uint32_t portsc = xhci_read32(port_reg);
        xhci_write32(port_reg, xhci_portsc_clean(portsc));
    }
    log_info("XHCI", "Root Hub Ports Powered (%u ports).", ctrl->max_ports);

    // Short stabilization delay
    mdelay(20);

    return 0;
}

void xhci_poll_ports(xhci_controller_t *ctrl) {
    if (!ctrl) return;

    log_info("XHCI", "Scanning %u Root Hub Ports for connected devices...", ctrl->max_ports);
    uint32_t connected_count = 0;

    for (uint8_t p = 1; p <= ctrl->max_ports; p++) {
        uintptr_t port_reg = ctrl->op_regs + XHCI_OP_PORTS_BASE + (p - 1) * 0x10;
        uint32_t portsc = xhci_read32(port_reg);

        if (portsc & XHCI_PORT_CCS) {
            uint8_t speed = (uint8_t)((portsc & XHCI_PORT_SPEED_MASK) >> XHCI_PORT_SPEED_SHIFT);
            bool enabled = (portsc & XHCI_PORT_PED) != 0;
            log_info("XHCI", "  Port %u: CONNECTED | Speed: %s | Enabled: %s",
                     p, get_speed_name(speed), enabled ? "YES" : "NO");
            connected_count++;
        }
    }

    if (connected_count == 0) {
        log_info("XHCI", "  No USB devices currently attached to root hub ports.");
    }
}

int xhci_reset_port(xhci_controller_t *ctrl, uint8_t port_id) {
    if (!ctrl || port_id == 0 || port_id > ctrl->max_ports) return -1;

    uintptr_t port_reg = ctrl->op_regs + XHCI_OP_PORTS_BASE + (port_id - 1) * 0x10;
    uint32_t portsc = xhci_read32(port_reg);

    // If device is not connected, nothing to reset
    if (!(portsc & XHCI_PORT_CCS)) return -1;

    // If port is already enabled SuperSpeed (speed >= 4, e.g. USB 3.0 SD card reader), no reset needed
    uint8_t speed = (uint8_t)((portsc & XHCI_PORT_SPEED_MASK) >> XHCI_PORT_SPEED_SHIFT);
    if ((portsc & XHCI_PORT_PED) && speed >= 4) {
        return 0;
    }

    if (speed >= 4) {
        // SuperSpeed (USB 3.0+): Warm Port Reset (WPR, bit 31)
        xhci_write32(port_reg, xhci_portsc_clean(portsc) | XHCI_PORT_WPR);

        int timeout = 100;
        while ((xhci_read32(port_reg) & XHCI_PORT_WPR) && --timeout > 0) {
            mdelay(2);
        }

        portsc = xhci_read32(port_reg);
        if (portsc & XHCI_PORT_WRC) {
            xhci_write32(port_reg, xhci_portsc_clean(portsc) | XHCI_PORT_WRC);
        }
    } else {
        // USB 2.0 / USB 1.1: Port Reset (PR, bit 4)
        xhci_write32(port_reg, xhci_portsc_clean(portsc) | XHCI_PORT_PR);

        int timeout = 100;
        while ((xhci_read32(port_reg) & XHCI_PORT_PR) && --timeout > 0) {
            mdelay(2);
        }

        portsc = xhci_read32(port_reg);
        if (portsc & XHCI_PORT_PRC) {
            xhci_write32(port_reg, xhci_portsc_clean(portsc) | XHCI_PORT_PRC);
        }

        // USB 2.0 Reset Recovery Time (TRSTRCY >= 10ms as per USB 2.0 Spec 7.1.7.5)
        mdelay(20);
    }

    portsc = xhci_read32(port_reg);
    return (portsc & XHCI_PORT_PED) ? 0 : -2;
}

int xhci_send_command(xhci_controller_t *ctrl, xhci_trb_t *cmd, xhci_trb_t *event_out) {
    if (!ctrl || !cmd) return -1;

    // Enqueue command TRB onto Command Ring
    uint32_t idx = ctrl->cmd_enqueue_idx;
    xhci_trb_t *trb = &ctrl->cmd_ring[idx];

    trb->parameter = cmd->parameter;
    trb->status = cmd->status;
    // Set cycle bit
    uint32_t control = (cmd->control & ~1U) | (ctrl->cmd_cycle_state ? 1U : 0U);
    trb->control = control;

    uintptr_t trb_phys = (uintptr_t)trb;

    // Advance enqueue index
    ctrl->cmd_enqueue_idx++;
    if (ctrl->cmd_enqueue_idx == (XHCI_CMD_RING_TRBS - 1)) {
        // Link TRB reached: toggle cycle bit on next pass
        ctrl->cmd_enqueue_idx = 0;
        ctrl->cmd_cycle_state ^= 1;
    }

    // Ring Host Controller Doorbell (Target = 0 for Host Controller Command)
    xhci_write32(ctrl->db_regs, 0);

    // Poll Event Ring for Command Completion Event
    int timeout = 500;
    while (--timeout > 0) {
        xhci_trb_t *evt = &ctrl->event_ring[ctrl->event_dequeue_idx];
        uint32_t cycle = evt->control & 1U;

        if (cycle == ctrl->event_cycle_state) {
            uint32_t type = (evt->control >> TRB_TYPE_SHIFT) & 0x3F;
            if (type == TRB_COMMAND_COMPL) {
                if (evt->parameter == trb_phys) {
                    if (event_out) {
                        *event_out = *evt;
                    }
                    // Advance event dequeue index
                    ctrl->event_dequeue_idx++;
                    if (ctrl->event_dequeue_idx == XHCI_EVENT_RING_TRBS) {
                        ctrl->event_dequeue_idx = 0;
                        ctrl->event_cycle_state ^= 1;
                    }

                    // Update ERDP
                    uintptr_t intr0 = ctrl->rt_regs + 0x20;
                    uintptr_t erdp = (uintptr_t)&ctrl->event_ring[ctrl->event_dequeue_idx];
                    xhci_write64(intr0 + XHCI_INTR_ERDP, erdp | XHCI_ERDP_EHB);

                    uint8_t cc = (uint8_t)((evt->status >> 24) & 0xFF);
                    return (cc == TRB_COMPL_SUCCESS) ? 0 : (int)cc;
                }
            }

            // Consume other event
            ctrl->event_dequeue_idx++;
            if (ctrl->event_dequeue_idx == XHCI_EVENT_RING_TRBS) {
                ctrl->event_dequeue_idx = 0;
                ctrl->event_cycle_state ^= 1;
            }
            uintptr_t intr0 = ctrl->rt_regs + 0x20;
            uintptr_t erdp = (uintptr_t)&ctrl->event_ring[ctrl->event_dequeue_idx];
            xhci_write64(intr0 + XHCI_INTR_ERDP, erdp | XHCI_ERDP_EHB);
        }
        udelay(100);
    }

    log_error("XHCI", "Command completion timed out!");
    return -100;
}

int xhci_enable_slot(xhci_controller_t *ctrl, uint8_t *slot_id_out) {
    xhci_trb_t cmd;
    cmd.parameter = 0;
    cmd.status = 0;
    cmd.control = TRB_TYPE(TRB_ENABLE_SLOT_CMD);

    xhci_trb_t evt;
    int res = xhci_send_command(ctrl, &cmd, &evt);
    if (res == 0 && slot_id_out) {
        *slot_id_out = (uint8_t)((evt.control >> 24) & 0xFF);
        log_info("XHCI", "Slot successfully enabled: ID = %u", *slot_id_out);
    }
    return res;
}

int xhci_disable_slot(xhci_controller_t *ctrl, uint8_t slot_id) {
    if (!ctrl || slot_id == 0) return -1;
    xhci_trb_t cmd;
    cmd.parameter = 0;
    cmd.status = 0;
    cmd.control = TRB_TYPE(TRB_DISABLE_SLOT_CMD) | ((uint32_t)slot_id << 24);
    xhci_trb_t evt;
    int res = xhci_send_command(ctrl, &cmd, &evt);
    if (res == 0) {
        log_info("XHCI", "Slot %u disabled cleanly.", slot_id);
    }
    return res;
}

void xhci_stop(xhci_controller_t *ctrl) {
    if (!ctrl || !ctrl->op_regs) return;

    log_info("XHCI", "Stopping xHCI controller for OS handoff...");

    // 1. Disable Interrupter 0 (IMAN.IE = 0)
    if (ctrl->rt_regs) {
        uintptr_t intr0 = ctrl->rt_regs + 0x20;
        xhci_write32(intr0 + XHCI_INTR_IMAN, 0);
    }

    // 2. Stop Controller (clear USBCMD.RS and USBCMD.INTE)
    uint32_t cmd = xhci_read32(ctrl->op_regs + XHCI_OP_USBCMD);
    cmd &= ~(XHCI_CMD_RS | XHCI_CMD_INTE);
    xhci_write32(ctrl->op_regs + XHCI_OP_USBCMD, cmd);

    // 3. Wait until controller is halted (USBSTS.HCH == 1)
    int timeout = 100;
    while (!(xhci_read32(ctrl->op_regs + XHCI_OP_USBSTS) & XHCI_STS_HCH) && --timeout > 0) {
        mdelay(1);
    }

    // 4. Disable PCI Bus Master to prevent any spurious DMA into kernel memory
    if (ctrl->pci_dev) {
        uint16_t pci_cmd = pci_read_config16(ctrl->pci_dev->bus, ctrl->pci_dev->dev, ctrl->pci_dev->func, 0x04);
        pci_write_config16(ctrl->pci_dev->bus, ctrl->pci_dev->dev, ctrl->pci_dev->func, 0x04, pci_cmd & ~0x06);
    }

    log_info("XHCI", "xHCI Controller successfully halted and DMA quiesced.");
}

