#ifndef XHCI_H
#define XHCI_H

#include <stdint.h>
#include <stdbool.h>
#include "xhci_regs.h"
#include "../pci/pci.h"

#define XHCI_CMD_RING_TRBS   64
#define XHCI_EVENT_RING_TRBS 64

typedef struct xhci_controller {
    pci_device_t *pci_dev;
    uintptr_t mmio_base;
    uintptr_t op_regs;
    uintptr_t db_regs;
    uintptr_t rt_regs;

    uint8_t   cap_len;
    uint16_t  hci_version;
    uint8_t   max_slots;
    uint16_t  max_intrs;
    uint8_t   max_ports;
    uint8_t   context_size; // 32 or 64 bytes (from HCCPARAMS1.CSZ)
    uint32_t  max_scratchpad_bufs;

    // Device Context Base Address Array (DCBAA)
    volatile uint64_t *dcbaa;

    // Command Ring
    volatile xhci_trb_t *cmd_ring;
    uint32_t            cmd_enqueue_idx;
    uint8_t             cmd_cycle_state;

    // Event Ring & ERST
    volatile xhci_trb_t *event_ring;
    uint32_t            event_dequeue_idx;
    uint8_t             event_cycle_state;
    xhci_erst_entry_t   *erst;
} xhci_controller_t;

int  xhci_init(pci_device_t *pci_dev, xhci_controller_t *ctrl);
void xhci_poll_ports(xhci_controller_t *ctrl);
int  xhci_reset_port(xhci_controller_t *ctrl, uint8_t port_id);
int  xhci_send_command(xhci_controller_t *ctrl, xhci_trb_t *cmd, xhci_trb_t *event_out);
void xhci_abort_command_ring(xhci_controller_t *ctrl);
int  xhci_enable_slot(xhci_controller_t *ctrl, uint8_t *slot_id_out);
int  xhci_disable_slot(xhci_controller_t *ctrl, uint8_t slot_id);
void xhci_stop(xhci_controller_t *ctrl);

const char *xhci_cc_to_string(uint8_t cc);
const char *xhci_pls_to_string(uint8_t pls);

#endif // XHCI_H
