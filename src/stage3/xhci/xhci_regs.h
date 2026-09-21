#ifndef XHCI_REGS_H
#define XHCI_REGS_H

#include <stdint.h>

static inline uint32_t xhci_read32(uintptr_t addr) {
    return *(volatile uint32_t *)addr;
}

static inline void xhci_write32(uintptr_t addr, uint32_t val) {
    *(volatile uint32_t *)addr = val;
}

static inline void xhci_write64(uintptr_t addr, uint64_t val) {
    xhci_write32(addr, (uint32_t)(val & 0xFFFFFFFF));
    xhci_write32(addr + 4, (uint32_t)(val >> 32));
}

// Capability Register Offsets (relative to MMIO Base)
#define XHCI_CAP_CAPLENGTH      0x00
#define XHCI_CAP_HCIVERSION     0x02
#define XHCI_CAP_HCSPARAMS1     0x04
#define XHCI_CAP_HCSPARAMS2     0x08
#define XHCI_CAP_HCSPARAMS3     0x0C
#define XHCI_CAP_HCCPARAMS1     0x10
#define XHCI_CAP_DBOFF          0x14
#define XHCI_CAP_RTSOFF         0x18
#define XHCI_CAP_HCCPARAMS2     0x1C

// Operational Register Offsets (relative to MMIO Base + CAPLENGTH)
#define XHCI_OP_USBCMD          0x00
#define XHCI_OP_USBSTS          0x04
#define XHCI_OP_PAGESIZE        0x08
#define XHCI_OP_DNCTRL          0x14
#define XHCI_OP_CRCR            0x18
#define XHCI_OP_DCBAAP          0x30
#define XHCI_OP_CONFIG          0x38
#define XHCI_OP_PORTS_BASE      0x400

// USBCMD Register Bits
#define XHCI_CMD_RS             (1U << 0)   // Run/Stop
#define XHCI_CMD_HCRST          (1U << 1)   // Host Controller Reset
#define XHCI_CMD_INTE           (1U << 2)   // Interrupter Enable
#define XHCI_CMD_HSEE           (1U << 3)   // Host System Error Enable

// USBSTS Register Bits
#define XHCI_STS_HCH            (1U << 0)   // Host Controller Halted
#define XHCI_STS_HSE            (1U << 2)   // Host System Error
#define XHCI_STS_EINT           (1U << 3)   // Event Interrupt
#define XHCI_STS_PCD            (1U << 4)   // Port Change Detect
#define XHCI_STS_CNR            (1U << 11)  // Controller Not Ready

// PORTSC Register Bits
#define XHCI_PORT_CCS           (1U << 0)   // Current Connect Status
#define XHCI_PORT_PED           (1U << 1)   // Port Enabled/Disabled
#define XHCI_PORT_OCA           (1U << 3)   // Over-current Active
#define XHCI_PORT_PR            (1U << 4)   // Port Reset
#define XHCI_PORT_PLS_MASK      (0xFU << 5) // Port Link State
#define XHCI_PORT_PP            (1U << 9)   // Port Power
#define XHCI_PORT_SPEED_MASK    (0xFU << 10)// Port Speed
#define XHCI_PORT_SPEED_SHIFT   10
#define XHCI_PORT_CSC           (1U << 17)  // Connect Status Change
#define XHCI_PORT_PEC           (1U << 18)  // Port Enable Change
#define XHCI_PORT_WRC           (1U << 19)  // Warm Port Reset Change
#define XHCI_PORT_OCC           (1U << 20)  // Over-current Change
#define XHCI_PORT_PRC           (1U << 21)  // Port Reset Change
#define XHCI_PORT_PLC           (1U << 22)  // Port Link State Change
#define XHCI_PORT_CEC           (1U << 23)  // Config Error Change
#define XHCI_PORT_WPR           (1U << 31)  // Warm Port Reset (USB 3.0 only)
#define XHCI_PORT_RW1C_MASK     (0x7FU << 17) // All RW1C change bits

// Interrupter Register Offsets (relative to MMIO Base + RTSOFF + 0x20 * i)
#define XHCI_INTR_IMAN          0x00
#define XHCI_INTR_IMOD          0x04
#define XHCI_INTR_ERSTSZ        0x08
#define XHCI_INTR_ERSTBA        0x10
#define XHCI_INTR_ERDP          0x18

#define XHCI_IMAN_IP            (1U << 0)   // Interrupt Pending
#define XHCI_IMAN_IE            (1U << 1)   // Interrupt Enable
#define XHCI_ERDP_EHB           (1U << 3)   // Event Handler Busy

// TRB Types
#define TRB_NORMAL              1
#define TRB_SETUP               2
#define TRB_DATA                3
#define TRB_STATUS              4
#define TRB_LINK                6
#define TRB_ENABLE_SLOT_CMD     9
#define TRB_DISABLE_SLOT_CMD    10
#define TRB_ADDRESS_DEV_CMD     11
#define TRB_CONFIG_EP_CMD       12
#define TRB_EVAL_CTX_CMD        13
#define TRB_RESET_EP_CMD        14
#define TRB_SET_TR_DEQ_CMD      16
#define TRB_NOOP_CMD            23
#define TRB_TRANSFER_EVENT      32
#define TRB_COMMAND_COMPL       33
#define TRB_PORT_STATUS_CHG     34

// TRB Control Flags
#define TRB_CYCLE               (1U << 0)
#define TRB_TOGGLE_CYCLE        (1U << 1)
#define TRB_ISP                 (1U << 2)   // Interrupt on Short Packet
#define TRB_CH                  (1U << 4)   // Chain bit
#define TRB_IOC                 (1U << 5)   // Interrupt on Completion
#define TRB_IDT                 (1U << 6)   // Immediate Data
#define TRB_TYPE_SHIFT          10
#define TRB_TYPE(t)             (((uint32_t)(t)) << TRB_TYPE_SHIFT)

// TRB Completion Codes
#define TRB_COMPL_SUCCESS       1
#define TRB_COMPL_DATA_BUF_ERR  2
#define TRB_COMPL_BABBLE_ERR    3
#define TRB_COMPL_TX_ERR        4
#define TRB_COMPL_TRB_ERR       5
#define TRB_COMPL_STALL_ERR     6
#define TRB_COMPL_SHORT_TX      13

#pragma pack(push, 1)

// Standard 16-byte xHCI TRB
typedef struct xhci_trb {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

// Event Ring Segment Table Entry (16 bytes)
typedef struct xhci_erst_entry {
    uint64_t ring_segment_base;
    uint32_t ring_segment_size;
    uint32_t reserved;
} xhci_erst_entry_t;

// Slot Context (32 bytes)
typedef struct xhci_slot_ctx {
    uint32_t info_1;    // Route String, Speed, Multi-TT, Hub, Context Entries
    uint32_t info_2;    // Max Exit Latency, Root Hub Port Num, Num Ports
    uint32_t tt_info;   // TT Hub Slot ID, TT Port Num, Interrupter Target
    uint32_t dev_state; // Device Address, Slot State
    uint32_t rsvd[4];
} xhci_slot_ctx_t;

// Endpoint Context (32 bytes)
typedef struct xhci_ep_ctx {
    uint32_t ep_info_1; // EP State, Interval, Max PStreams, Mult, CErr
    uint32_t ep_info_2; // EP Type, HID, Max Burst Size, Max Packet Size
    uint64_t tr_dequeue_ptr; // Transfer Ring Dequeue Pointer + DCS
    uint32_t avg_trb_len; // Average TRB Length, Max ESIT Payload
    uint32_t rsvd[3];
} xhci_ep_ctx_t;

#pragma pack(pop)

#endif // XHCI_REGS_H
