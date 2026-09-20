# Test Strategy & Verification Guide

This document outlines the testing workflow across emulator (QEMU) and physical target hardware, in accordance with **Sections 13, 14, and 15** of the project specification.

---

## Testing Hierarchy

```
QEMU Emulation
      ↓
SD Boot on Physical Laptop (CSM / Legacy BIOS)
      ↓
PCI / xHCI Host Controller Discovery
      ↓
USB Enumeration of Connected Devices
      ↓
Android Phone Identification (MTP Interface)
      ↓
MTP Session & Filesystem Traversal (/Download/)
      ↓
File Retrieval & Verification (TEST.TXT)
      ↓
Linux Image Retrieval (Streaming into RAM)
      ↓
Linux Image & Filesystem Parsing
      ↓
Linux Kernel Boot (Handoff to Real Linux)
```

---

## 1. What QEMU Proves vs. Does Not Prove

### What QEMU Proves:
* Stage 1 BIOS MBR loading, INT 13h DAP extended sector reads, register preservation (`DL`).
* Stage 2 real-to-protected mode transition, A20 gate activation, E820 memory map parsing.
* Stage 3 C runtime initialization, 32-bit GDT, stack setup, serial UART (COM1 0x3F8) logging.
* PCI configuration space scanning and PCI device discovery.
* Emulated xHCI controller initialization and basic USB enumeration (using QEMU's `qemu-xhci`).
* FAT32 partition reading and ChaN FatFs operation.
* Linux 32-bit boot protocol header validation, `boot_params` creation, and kernel jumping.

### What QEMU Does NOT Prove:
* Physical laptop BIOS quirkiness (some BIOSes map USB/SD drives differently, e.g., floppy vs hard disk emulation).
* Real xHCI silicon quirks (Intel, AMD, ASMedia, Renesas host controllers have specific timing and scratchpad requirements).
* Actual Android USB MTP behavior (Android smartphones have dynamic MTP states, lock screen permissions, USB connection modes, and proprietary container handling).
* Real-world bulk transfer throughput and cable disconnect recovery.

---

## 2. Automated QEMU Test Harness

The test runner `tools/qemu_test.py` launches QEMU with `-serial stdio` and `-display none` and asserts expected log tags:

```bash
python tools/qemu_test.py
```

Expected log output sequence:
```text
[BIOS] Boot Drive: 0x80
[STAGE1] Loading Stage 2... OK
[STAGE2] Probing E820 Memory Map... OK
[STAGE2] Enabling A20 Gate... OK
[STAGE2] Loading Stage 3 (N sectors)... OK
[STAGE2] Entering 32-bit Protected Mode...
[STAGE3] Stage 3 C Runtime Initialized at 0x00100000
[PCI] Scanning PCI Bus...
[PCI] Found xHCI Controller at 00:0X.0 (Vendor: ... Device: ...)
```

---

## 3. Physical Hardware Test Procedure

When validating on physical hardware:
1. **Prepare Bootable SD Card**:
   ```bash
   # Identify SD card physical drive (e.g., PhysicalDrive2 on Windows or /dev/sdX on Linux)
   # Write boot.img using dd or Rufus in DD mode
   ```
2. **Insert into Target Laptop**:
   * Enable Legacy BIOS / CSM in BIOS settings.
   * Disable Secure Boot.
   * Boot from SD card.
3. **Verify Stages**:
   * Watch laptop screen for VGA text output: `[STAGE1]` → `[STAGE2]` → `[STAGE3]`.
4. **Connect Android Phone via USB**:
   * Ensure phone is unlocked.
   * Set USB mode to "File Transfer / MTP".
   * Watch bootloader console detect MTP interface and enumerate `/Download/`.
