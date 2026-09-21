# System Architecture & Technical Specifications

This document specifies the technical architecture of the Android → Linux Legacy BIOS Bootloader.

---

## 1. System Overview

```
                      +-----------------------------+
                      |         Legacy BIOS         |
                      +-----------------------------+
                                     |
                                     v
                      +-----------------------------+
                      |     Stage 1 (MBR Sector)    |
                      |  16-bit Real Mode (0x7C00)  |
                      +-----------------------------+
                                     |
                                     v
                      +-----------------------------+
                      |      Stage 2 Bootstrap      |
                      |  A20, E820, Protected Mode  |
                      +-----------------------------+
                                     |
                                     v
                      +-----------------------------+
                      |   Stage 3 C Runtime (1 MB)  |
                      | 32-bit Flat Protected Mode  |
                      +-----------------------------+
                                     |
         +---------------------------+---------------------------+
         |                           |                           |
         v                           v                           v
+-----------------+         +-----------------+         +-----------------+
|  PCI Subsystem  |         | Memory & Heap   |         | Serial / VGA /  |
|  Scan for xHCI  |         | E820 Allocator  |         | BIOS Thunk Log  |
+-----------------+         +-----------------+         +-----------------+
         |
         v
+-----------------+
|   xHCI Driver   |
|   (libpayload)  |
+-----------------+
         |
         v
+-----------------+
| USB Core Stack  |
| Enumerate Ports |
+-----------------+
         |
         +---------------------------+---------------------------+
         |                           |                           |
         v                           v                           v
+-----------------+         +-----------------+         +-----------------+
| Android ADB     |         | Android MTP     |         | USB Mass        |
| Root Bridge     |         | Initiator       |         | Storage (MSC)   |
| (RSA Auth)      |         | (AOSP / libmtp) |         | (SCSI BOT)      |
+-----------------+         +-----------------+         +-----------------+
         |                           |                           |
         | 'U' Trigger               |                           |
         v                           |                           |
+-----------------+                  |                           |
| ConfigFS Gadget |                  |                           |
| Switch to UMS   |                  |                           |
+-----------------+                  |                           |
         |                           |                           |
         +---------------------------)---------------------------+
                                     |                           |
                                     v                           v
                      +-----------------------------+ +-----------------------------+
                      | In-RAM ISO Staging Mode     | | Direct Block Access Mode    |
                      | (MTP Bulk Stream, mBFT)     | | (0 MB in RAM, Instant Boot) |
                      +-----------------------------+ +-----------------------------+
                                     \                         /
                                      \                       /
                                       v                     v
                                  +-----------------------------+
                                  |   Dynamic Multi-OS Scanner  |
                                  |   & Persistence Engine      |
                                  +-----------------------------+
                                                 |
                                                 v
                                  +-----------------------------+
                                  | Interactive Boot Menu (TUI) |
                                  | (BIOS INT 16h, PS/2, Serial)|
                                  +-----------------------------+
                                                 |
                                                 v
                                  +-----------------------------+
                                  | Linux 32-bit Boot Protocol  |
                                  | (boot_params, e820, initrd) |
                                  +-----------------------------+
                                                 |
                                                 v
                                  +-----------------------------+
                                  |  Low-Memory Trampoline Jump |
                                  |  (0x00008000 -> 0x00100000) |
                                  +-----------------------------+
                                                 |
                                                 v
                                  +-----------------------------+
                                  |         REAL LINUX          |
                                  +-----------------------------+
```

---

## 2. Subsystem Architectural Details

### 2.1 Stage 1 (MBR Boot Sector)
* **Filename**: `src/stage1/stage1.asm`
* **Size**: Exactly 512 bytes.
* **CPU Mode**: 16-bit Real Mode.
* **Entry Point**: `0x0000:0x7C00`.
* **Register state on entry**:
  * `DL`: BIOS boot drive number (e.g., `0x80` for hard drive/SD card, `0x00` for floppy).
  * `CS:IP`: `0x0000:0x7C00` or `0x07C0:0x0000`.
* **Actions**:
  1. Sets up canonical segments and stack (`cli`, `xor ax, ax`, `mov ds, ax`, `mov es, ax`, `mov ss, ax`, `mov sp, 0x7C00`, `sti`).
  2. Preserves BIOS boot drive `DL`.
  3. Resets disk controller (`INT 13h, AH=0x00`).
  4. Checks for INT 13h BIOS extensions (`AH=0x41, BX=0x55AA`).
  5. Populates Disk Address Packet (DAP) pointing to LBA 1 (Stage 2) with target buffer `0x0000:0x8000`.
  6. Executes `INT 13h, AH=0x42` (Extended Read).
  7. Verifies Stage 2 signature ("STG2" = `0x32475453`) at `0x8004`.
  8. Passes boot drive in `DL` and jumps to `0x0000:0x8000`.

### 2.2 Stage 2 (Bootstrap Loader)
* **Filename**: `src/stage2/stage2.asm`
* **CPU Mode**: 16-bit Real Mode → 32-bit Flat Protected Mode.
* **Entry Point**: `0x0000:0x8000`.
* **Actions**:
  1. Queries the system memory map using `INT 15h, AX=0xE820`. Stores records at `0x0000:0x9000`.
  2. Enables the A20 Gate:
     * Fast A20 test via System Control Port `0x92`.
     * Keyboard Controller (8042) fallback if required.
     * Validates A20 is active by testing address wrap-around at `0x107C00` vs `0x007C00`.
  3. Loads Stage 3:
     * Reads Stage 3 sector count and start LBA from header.
     * Uses `INT 13h, AH=0x42` to load Stage 3 in 64-sector chunks into temporary conventional memory at `0x1000:0x0000` (`0x10000`).
  4. Initializes the Global Descriptor Table (GDT):
     * Entry 0: Null descriptor.
     * Entry 1 (`0x08`): 32-bit Code Segment (Base=0, Limit=4GB, Access=0x9A, Flags=0xCF).
     * Entry 2 (`0x10`): 32-bit Data Segment (Base=0, Limit=4GB, Access=0x92, Flags=0xCF).
  5. Sets `CR0.PE = 1` (Protected Mode Enable).
  6. Executes far jump: `jmp 0x08:pmode_entry`.
  7. In 32-bit mode, relocates Stage 3 from `0x10000` to `0x0010_0000` (1 MiB) via `rep movsd`.
  8. Prepares `boot_info` pointer in `EAX` and jumps to Stage 3 entry point at `0x0010_0000`.

### 2.3 Stage 3 (C Runtime Core)
* **Filename**: `src/stage3/core/entry.S`, `src/stage3/core/main.c`
* **Entry Point**: `stage3_entry` at `0x0010_0000`.
* **Environment**: 32-bit flat protected mode.
* **Responsibilities**:
  * Initializes 16-byte aligned stack (grows downwards from `0x0009_FFF0`) and zeroes `.bss`.
  * Initializes COM1 UART (`0x3F8`) at 115200 baud 8N1 for real-time serial logging.
  * Initializes VGA text console (`0xB8000`) with color formatting.
  * Initializes persistent SD disk logging subsystem via real-mode BIOS thunk.
  * Parses E820 memory map and initializes bump/slab physical memory allocator.
  * Scans PCI configuration space (`0xCF8/0xCFC`) across 256 buses, 32 devices, 8 functions.
  * Discovers and initializes xHCI USB 3.x host controller.
  * Probes connected USB devices, performs enumeration, and launches boot loop.

### 2.4 USB Host Controller (xHCI) & USB Stack
* **Filenames**: `src/stage3/xhci/xhci.c`, `src/stage3/usb/usb.c`, `src/stage3/usb/usb_msc.c`
* **Upstream Heritage**: coreboot / libpayload.
* **Architecture**:
  * MMIO operational register mapping, controller reset (`USBCMD.HCRST`), slot configuration.
  * 64-byte aligned DMA structures: Device Context Base Address Array (DCBAA), Command Ring, Event Ring with Event Ring Segment Table (ERST), and Scratchpad Buffers.
  * Synchronous polling ring model tailored for deterministic single-threaded firmware execution.
  * Root hub port reset, Enable Slot, Input Context allocation, and Address Device commands.
  * Control transfers for Device, Configuration, and Interface descriptors.
  * Bulk endpoint transfer rings for fast data streaming.

### 2.5 Android ADB Root Bridge & Dynamic UMS Gadget Switch
* **Filename**: `src/stage3/adb/adb.c`, `adb.h`
* **Features**:
  * Implements freestanding ADB wire protocol framing (`A_CNXN`, `A_AUTH`, `A_OPEN`, `A_OKAY`, `A_CLSE`, `A_WRTE`).
  * Loads host RSA public key (`ADBKEY.PUB`) dynamically from SD card Partition 1.
  * Executes non-destructive pre-flight checks against phone's `su` shell:
    * Tests kernel ConfigFS support (`/config/usb_gadget/g1/functions/mass_storage.0` or `mass_storage.usb0`).
    * Fallback to legacy SysFS (`/sys/class/android_usb`).
  * Attaches target ISO file to LUN 0 (`lun.0/file`), sets read-only, and binds gadget to UDC.
  * Triggers USB port reprobing; the phone re-enumerates as a hardware USB Mass Storage drive.
  * Enables **Direct Block Access**: the laptop boots the OS with **0 MB consumed in laptop RAM**!

### 2.6 Real-Mode BIOS Thunking Subsystem
* **Filename**: `src/stage3/bios/bios_thunk.S`, `bios_disk.c`
* **Mechanism**:
  * Provides a low-overhead gateway that transitions CPU from 32-bit Protected Mode back to 16-bit Real Mode and returns cleanly.
  * Mailbox buffer at `0x0000_7100` passes drive number, DAP pointer, command codes, and saves 32-bit `ESP`.
  * Copies 16-bit thunk trampoline to `0x0000_7000`.
  * Supports:
    * `bios_int13_call`: Extended disk read/write for SD card access.
    * `bios_int10_call`: VBE video mode query and switching.
    * `bios_int16_call`: Polling keyboard keystrokes directly through BIOS services (enables built-in laptop keyboards and USB legacy emulation keyboards without needing HID drivers).

### 2.7 Dual Persistent SD Disk Logging Subsystem
* **Filename**: `src/stage3/debug/disk_log.c`, `disk_log.h`
* **Strategy**:
  * Maintains a 64 KiB circular in-memory log buffer.
  * Intercepts all `printk` and `log_info` calls.
  * **FAT32 BOOTLOG.TXT**: Directly updates the cluster sectors of `BOOTLOG.TXT` on Partition 1 via ChaN FatFs / BIOS thunking.
  * **Raw LBA 1024 Backup**: Atomically writes a 512-byte header with magic `BOOTLOG!` and log sectors starting at fixed LBA 1024 (sectors 1024..1151). Moving raw logging to LBA 1024 expands Stage 3 binary headroom up to 510 KB (LBA 3..1023). If FAT32 is damaged, logs can always be extracted via `tools/read_bootlog.py` or `tools/read_log.py`.
  * Audio feedback on flush via PC speaker (`sound.h`).

### 2.8 Dynamic Multi-OS Scanner & Universal Boot Config Parser
* **Filenames**: `src/stage3/image/os_scanner.c`, `src/stage3/filesystem/iso_reader.c`, `src/stage3/filesystem/boot_cfg_parser.c`
* **Features**:
  * Probes USB MSC devices, MTP sessions, ADB root directories, and the boot SD card.
  * ISO 9660 reader with **Rock Ridge (SUSP/RRIP NM)** and **Joliet (UCS-2)** support for un-truncated POSIX filenames and deep directory traversal without loading full images.
  * **Universal Dynamic Config Parser**:
    * Dynamically discovers and parses distro-native boot configurations (`/boot/grub/grub.cfg`, `/boot/grub/loopback.cfg`, `/isolinux/isolinux.cfg`, `/syslinux/syslinux.cfg`, etc.).
    * Extracts distro-native kernel paths (`vmlinuz*`, `linux*`), initramfs paths (`initrd*`, `initramfs*`), and distro-native boot parameters (e.g. `boot=casper`, `alpine_repo=`, `archisobasedir=`, `root=`).
    * Resolves GRUB variables (e.g. `${iso_path}`) dynamically.
  * Automatically configures distribution-specific persistence and boot parameters:
    * **Ubuntu / Debian / Casper**: `boot=casper persistent persistent-path=/BootManager/persistence/`
    * **Alpine Linux**: `phram=iso,ADDR,SIZE memdisk=yes` (attaches `apkovl=LABEL=BOOTLOADER:<profile>` only when an Alpine persistence profile is actively selected)
    * **Generic Distros / Hard Drive OS**: Preserves native distro options extracted from boot configuration without forcing unnecessary overlays.

### 2.9 PIT & TSC Calibrated Hardware Timer Subsystem
* **Filename**: `src/stage3/core/timer.c`, `timer.h`
* **Features**:
  * Frequency-calibrated Intel 8254 Programmable Interval Timer (PIT) on Channel 2 / Port `0x61`.
  * RDTSC (Read Time-Stamp Counter) hardware calibration for cycle-accurate nanosecond/microsecond delays (`udelay`, `mdelay`).
  * Non-blocking timeout primitives (`timer_now_ms`, `timer_elapsed_ms`) preventing infinite hangs during USB enumeration and SCSI handshakes.

### 2.10 Free-List Dynamic Heap Allocator
* **Filename**: `src/stage3/memory/heap.c`, `heap.h`
* **Features**:
  * Segregated free-list memory allocator with boundary-tag block headers and bi-directional coalescing.
  * O(1) best-fit / first-fit allocation with split and merge, preventing heap fragmentation across long sessions.
  * Integrates seamlessly with ChaN FatFs dynamic long filename (LFN) working buffers (`FF_USE_LFN = 3`).

### 2.11 Universal ChaN FatFs VFS Integration
* **Filenames**: `src/stage3/filesystem/ff.c`, `diskio.c`, `fat_source.c`
* **Features**:
  * Production-grade ChaN FatFs R0.15c with dynamic LFN, 4096-byte sector support (`FF_MAX_SS = 4096`), read/write support (`FF_FS_READONLY = 0`), and CMOS RTC timestamps.
  * Unified block driver `diskio.c` binding Drive 0 to BIOS INT 13h SD Card and Drive 1 to xHCI USB Mass Storage (SCSI).
  * High-level VFS wrapper `fat_source.c` providing unified directory enumeration, file streaming, and file creation.

### 2.12 Multi-LUN SCSI Block-Only Transport (BOT)
* **Filename**: `src/stage3/usb/usb_msc.c`
* **Features**:
  * Full SCSI Command Block Wrapper (CBW) / Command Status Wrapper (CSW) protocol implementation.
  * Probes and addresses multiple Logical Unit Numbers (LUNs) via `GET_MAX_LUN`.
  * Auto-recovery: automatic `CLEAR_FEATURE(ENDPOINT_HALT)` and SCSI Request Sense on transport stalls and condition met/check status.

### 2.13 Arrow-Key Interactive TUI & Live Cmdline Editor
* **Filename**: `src/stage3/ui/menu.c`, `menu.h`
* **Features**:
  * Full ANSI keyboard navigation (Arrow Up, Arrow Down, Enter, 'c' for Edit, 'r' for Rescan, 'h' for Help).
  * Real-time USB hotplug badge and OS type badges (`[LIVE-DIR]`, `[ISO-9660]`, `[MTP-STREAM]`).
  * Live in-place kernel command-line editor enabling on-the-fly kernel parameter overrides.

### 2.14 Linux 32-bit Boot Protocol Loader & Trampoline Handoff
* **Filename**: `src/stage3/linux/linux_boot.c`, `linux_boot.h`
* **Specifications**:
  * Validates Linux setup header magic `0x53726448` (`HdrS`) at offset `0x1F1` and protocol version $\ge 2.00$.
  * Constructs full 4096-byte `boot_params` structure at physical `0x0009_0000`.
  * Populates kernel command line at `0x0009_A000`.
  * Maps initramfs address and size safely above kernel decompression boundaries.
  * Populates `boot_params->e820_table` with BIOS memory map, marking any in-RAM ISO region as Type 2 (RESERVED) to prevent memory corruption.
  * For in-RAM ISOs, deploys standard **mBFT** (MEMDISK Boot Information Table) at `0x000E_0000`.
  * Shuts down xHCI controller rings cleanly via `xhci_stop()`.
  * Deploys relocation trampoline to `0x0000_8000`: relocates protected-mode kernel code to `0x0010_0000`, loads 32-bit segment selectors (`0x10`), sets `ESI = boot_params`, clears registers, and jumps to `code32_start`.


