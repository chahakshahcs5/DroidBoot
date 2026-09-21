# Test Strategy & Verification Guide

This document outlines the testing workflow across emulator (QEMU) and physical target hardware, in accordance with the project specification.

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
                  +---------------+---------------+
                  |                               |
       [Unmodified Phone / MTP]         [Rooted Phone / ADB]
                  ↓                               ↓
       MTP Session & File Traversal     ADB Authentication & Shell
                  ↓                               ↓
       In-RAM ISO Staging (mBFT)        ConfigFS Gadget UMS Switch
                  ↓                               ↓
       SD Card apkovl Persistence       Direct Block Access (0 MB RAM)
                  +---------------+---------------+
                                  ↓
                    Linux 32-bit Boot Protocol
                                  ↓
                  Real Linux Kernel Execution Jump
```

---

## 1. What QEMU Proves vs. Does Not Prove

### What QEMU Proves:
* Stage 1 BIOS MBR loading, INT 13h DAP extended sector reads, register preservation (`DL`).
* Stage 2 real-to-protected mode transition, A20 gate activation, E820 memory map parsing.
* Stage 3 C runtime initialization, 32-bit GDT, stack setup, serial UART (`COM1 0x3F8`) logging.
* Real-mode BIOS thunking (`bios_thunk.S`) for INT 13h, INT 10h, and INT 16h keyboard polling.
* Dual persistent disk logging to FAT32 `BOOTLOG.TXT` and raw LBA 256 backup sectors.
* PCI configuration space scanning and PCI device discovery.
* xHCI controller initialization, command/event ring mechanics, and USB enumeration.
* USB Mass Storage (MSC) SCSI Bulk-Only Transport reading 2048-byte CD sectors.
* Linux 32-bit boot protocol header validation, `boot_params` creation, and trampoline kernel jump.

### What QEMU Does NOT Prove:
* Physical laptop BIOS quirkiness (e.g., BIOS mapping SD cards as floppy `0x00` vs hard disk `0x80`).
* Real xHCI silicon quirks (timing, context sizes, and scratchpad limits across Intel, AMD, ASMedia, and Renesas controllers).
* Real-world smartphone USB gadget state machines (lock screen permissions, MTP container timeouts, USB mode selection dialogs).
* Physical USB cable noise and disconnect/reconnect recovery.

---

## 2. Test Runner Suite

The repository contains an automated test suite under `tools/`:

### 2.1 Headless Bootloader Assertion Test
```bash
python tools/qemu_test.py
```
* Boots QEMU headlessly (`-display none -serial stdio`).
* Asserts expected sequence: `[STAGE1] OK` → `[STAGE2] OK` → `[STAGE3] Initialized` → `[PCI] xHCI Controller`.

### 2.2 Alpine Linux Direct Block Boot Test
```bash
python tools/test_alpine_msc.py
```
* Attaches an emulated USB Mass Storage device containing an Alpine Linux ISO.
* Verifies SCSI inquiry, kernel/initrd extraction into RAM, `boot_params` generation, and kernel handoff.

### 2.3 Ubuntu Casper Live Direct Block Boot Test
```bash
python tools/test_ubuntu_msc.py
```
* Attaches an emulated 4–6 GB Ubuntu Casper live ISO as a USB block device.
* Verifies that the bootloader consumes **0 MB in RAM** for the root filesystem and passes `boot=casper` command line arguments.

### 2.4 Dynamic USB Disconnect / Reconnect & Switch Test
```bash
python tools/test_dynamic_msc_switch.py
```
* Simulates dynamic phone attachment and runtime gadget switching from MTP/charge mode to USB Mass Storage.

### 2.5 Physical Android Phone Passthrough Test
```bash
python tools/test_physical_phone_qemu.py
```
* Connect your physical Android phone to your PC via USB.
* Runs QEMU with native USB passthrough (`-device usb-host,vendorid=...,productid=...`) to test real Android MTP and ADB communication in the emulator.

---

## 3. Persistent Log Extraction (`tools/read_log.py`)

Even if the screen is black or the target machine freezes, logs are written to the boot SD card:
```bash
# Read logs from a disk image:
python tools/read_log.py build/boot.img

# Read logs directly from a physical SD card drive (e.g., \\.\PhysicalDrive2 on Windows or /dev/sdX on Linux):
python tools/read_log.py \\.\PhysicalDrive2
```
* Decodes both the FAT32 `BOOTLOG.TXT` file and the raw backup header at LBA 256.

---

## 4. Audio Cue Feedback Reference (PC Speaker)

For debugging on physical laptops without working video or serial ports, the bootloader provides auditory feedback via [sound.h](file:///c:/Users/chaha/Projects/bootmanager/src/stage3/debug/sound.h):

| Audio Tone | Frequency / Pattern | Meaning |
| :--- | :--- | :--- |
| **Boot Tone** | Ascending double-tone (880 Hz → 1175 Hz) | Stage 3 C runtime initialized successfully. |
| **Phone Connected** | High melodic chime (1320 Hz → 1760 Hz) | Android device enumerated and MTP/ADB active. |
| **User Action Prompt** | Two short alert beeps (988 Hz) | Phone is in charge mode; unlock phone and select MTP. |
| **Error Tone** | Descending buzz (440 Hz → 330 Hz) | Critical error encountered (no bootable OS found). |
| **Kernel Jump Fanfare** | 3-note ascending fanfare (1046 Hz → 1318 Hz → 1568 Hz) | Trampoline executed; execution handed to Linux kernel. |

---

## 5. Physical Hardware Verification Checklist

1. **Write Boot Image to SD Card**:
   ```bash
   python build.py
   # Write build/boot.img to SD card using Rufus (DD mode) or dd (e.g. dd if=build/boot.img of=/dev/sdX bs=4M status=progress)
   ```
2. **Configure Laptop BIOS**:
   * Enable Legacy BIOS / CSM (Compatibility Support Module).
   * Disable Secure Boot.
   * Place SD Card / USB HDD first in boot order.
3. **Power On Laptop**:
   * Listen for the ascending boot tone.
   * Watch VGA console for `ANDROID -> LINUX BOOTLOADER` banner.
4. **Connect Android Smartphone**:
   * Unlock phone.
   * If non-rooted: Tap "File Transfer / MTP". Listen for high chime.
   * If rooted: Press `'U'` on keyboard to switch phone to USB Mass Storage.
5. **Boot Linux**:
   * Select OS number or wait for countdown timer.
   * Listen for kernel jump fanfare as Linux boots.
