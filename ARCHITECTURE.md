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
  * **FAT32 BOOTLOG.TXT**: Directly updates the cluster sectors of `BOOTLOG.TXT` on Partition 1 via BIOS thunking.
  * **Raw LBA 256 Backup**: Atomically writes a 512-byte header with magic `BOOTLOG!` and log sectors starting at fixed LBA 256. If FAT32 is damaged, logs can always be extracted via `tools/read_log.py`.
  * Audio feedback on flush via PC speaker (`sound.h`).

### 2.8 Dynamic Multi-OS Scanner & Persistence Engine
* **Filenames**: `src/stage3/image/os_scanner.c`, `src/stage3/filesystem/iso_reader.c`
* **Features**:
  * Probes USB MSC devices, MTP sessions, ADB root directories, and the boot SD card.
  * ISO 9660 reader parses Primary Volume Descriptors, directory records, and path tables without loading entire images.
  * Identifies Linux kernels (`vmlinuz`, `bzImage`) and initramfs files (`initrd.img`, `initramfs-lts`).
  * Detects distribution flavors (Ubuntu Casper live, Alpine Linux, Arch, Debian).
  * Automatically configures distribution-specific persistence:
    * **Ubuntu / Casper**: `boot=casper persistent persistent-path=/BootManager/persistence/`
    * **Alpine Linux**: `phram=iso,ADDR,SIZE memdisk=yes apkovl=sda1:`
    * **Clean Session**: Ephemeral in-memory execution with no disk modifications.

### 2.9 Linux 32-bit Boot Protocol Loader & Trampoline Handoff
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

