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

## 2. Unified Test Runner (`python test.py`)

The repository provides a single master test orchestrator at the root of the project:

### 2.1 Complete Automated Regression Suite (Default)
```bash
python test.py                 # Runs complete automated regression suite (all 6 suites)
python test.py --debug         # Runs complete regression suite against Debug build image
```
* Runs all 6 headless automated test suites in sequence.
* Emulates xHCI, USB mass storage, dynamic gadget switching, and SD card live block writeback in pure software.
* Tests all boot phases: Stage 1 MBR → Stage 2 A20 → Stage 3 Protected Mode → E820 RAM → PCI scan → xHCI initialization → USB enumeration → TUI menu → Linux kernel handoff → SD card persistence.

### 2.2 Running Individual Test Suites
```bash
python test.py --suite baseline     # Phase 1-10 bootstrap, memory map, PCI, xHCI, TUI menu, self-test
python test.py --suite persistence  # Multi-profile persistence selector and clean disposable session
python test.py --suite sizing       # Dynamic custom persistence overlay capacity sizing (2GB, 4GB, 8GB, 16GB)
python test.py --suite ram          # In-RAM ISO streaming and phram parameter kernel handoff
python test.py --suite gadget       # Dynamic phone mode-switch simulation (MTP -> UMS via QMP hotplug)
python test.py --suite sdlog        # Live SD card writeback persistence (FAT32 BOOTLOG.TXT, BOOT0001.LOG, LBA 1024)
```

### 2.3 Interactive Visual Debugging
```bash
python test.py --interactive
```
* Launches QEMU with a graphical window and live COM1 serial UART output connected to your terminal.
* Allows manual arrow-key navigation, in-place kernel command-line editing (`'e'`), and manual OS selection.

### 2.4 Testing Debug vs. Release Images
* **Release (`build/boot.img`)**: Maximum speed, clean console, persistent disk logging disabled by design (`IS_DEBUG_BUILD = 0`).
* **Debug (`build/boot-debug.img`)**: Deep hardware diagnostics, verbose xHCI/USB packet logging, and simultaneous live logging to SD card (`BOOTLOG.TXT`, `BOOT0001.LOG`, LBA 1024) and Android phone (`/sdcard/BootManager/...`).
```bash
python test.py                 # Tests Release image (build/boot.img)
python test.py --debug         # Tests Debug image (build/boot-debug.img)
```

---

## 3. Persistent Log Extraction (`tools/read_bootlog.py`)

Even if the screen is black or the target machine freezes, logs are written simultaneously to both the boot SD card / USB drive and the Android phone:

```bash
# 1. Read directly from connected Android phone via ADB:
python tools/read_bootlog.py --phone

# 2. List all historical boot logs stored on Android phone:
python tools/read_bootlog.py --phone --list

# 3. Read specific historical log from phone:
python tools/read_bootlog.py --phone --file boot_20260921_191021.log

# 4. Read from physical boot SD card / USB drive (e.g., E:\ on Windows):
python tools/read_bootlog.py E:\BOOTLOG.TXT

# 5. List all rotating boot sessions on SD card:
python tools/read_bootlog.py --list E:\
```
* Decodes both the FAT32 cluster chain (`BOOTLOG.TXT`, `BOOT0001.LOG`..`BOOT0010.LOG`) and raw backup sectors (LBA 1024..1151).

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
