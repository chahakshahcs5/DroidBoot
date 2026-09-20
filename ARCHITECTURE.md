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
|  PCI Subsystem  |         | Memory & Heap   |         | Serial / Screen |
|  Scan for xHCI  |         | E820 Allocator  |         | Logging Output  |
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
         v
+-----------------+
| MTP Subsystem   |
| (AOSP / libmtp) |
+-----------------+
         |
         v
+-------------------------------------------------------+
| BootSource Abstraction (MTP / SD FAT32 / Memory Image)|
+-------------------------------------------------------+
         |
         v
+-------------------------------------------------------+
| Image & Filesystem Parser (bzImage / MBR / GPT / ext4)|
+-------------------------------------------------------+
         |
         v
+-------------------------------------------------------+
| Linux 32-bit Boot Protocol Loader                     |
| Setup boot_params, command line, initrd, jump to 1 MB |
+-------------------------------------------------------+
         |
         v
+-------------------------------------------------------+
|                      REAL LINUX                       |
+-------------------------------------------------------+
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
  1. Sets up data and stack segments (`cli`, `xor ax, ax`, `mov ds, ax`, `mov es, ax`, `mov ss, ax`, `mov sp, 0x7C00`, `sti`).
  2. Saves BIOS boot drive `DL` to a known memory location.
  3. Resets disk controller (`INT 13h, AH=0x00`).
  4. Checks for INT 13h BIOS extensions (`AH=0x41, BX=0x55AA`).
  5. Constructs a Disk Address Packet (DAP) pointing to LBA 1 (Stage 2) with target buffer `0x0000:0x8000`.
  6. Executes `INT 13h, AH=0x42` (Extended Read).
  7. Verifies Stage 2 signature ("STG2") at `0x8000`.
  8. Passes boot drive in `DL` and jumps to `0x0000:0x8000`.

### 2.2 Stage 2 (Bootstrap Loader)
* **Filename**: `src/stage2/stage2.asm`
* **CPU Mode**: 16-bit Real Mode → Unreal Mode → 32-bit Flat Protected Mode.
* **Entry Point**: `0x0000:0x8000`.
* **Actions**:
  1. Queries the system memory map using `INT 15h, AX=0xE820`. Stores entries at `0x0000:0x9000`.
  2. Enables the A20 Gate:
     * Fast A20 test via System Control Port `0x92`.
     * Keyboard Controller (8042) fallback if required.
     * Validates A20 is active by testing address wrap-around at `0x107C00` vs `0x007C00`.
  3. Loads Stage 3:
     * Reads Stage 3 sector count and start LBA from its header.
     * Uses Unreal Mode (big real mode) or chunked BIOS reads to load Stage 3 into physical memory at `0x0010_0000` (1 MiB).
  4. Initializes the Global Descriptor Table (GDT):
     * Entry 0: Null descriptor.
     * Entry 1 (0x08): 32-bit Code Segment (Base=0, Limit=4GB, Access=0x9A, Flags=0xCF).
     * Entry 2 (0x10): 32-bit Data Segment (Base=0, Limit=4GB, Access=0x92, Flags=0xCF).
  5. Sets `CR0.PE = 1` (Protected Mode Enable).
  6. Executes far jump: `jmp 0x08:0x00100000`.

### 2.3 Stage 3 (C Runtime)
* **Entry Point**: `stage3_entry` at `0x0010_0000`.
* **Environment**: 32-bit flat protected mode, paging disabled initially.
* **Responsibilities**:
  * Initialize COM1 UART (0x3F8) for 115200 baud serial boot logging.
  * Initialize VGA text mode screen driver (0xB8000).
  * Parse E820 memory map from `0x9000` and initialize bump/freelist physical memory allocator.
  * Scan PCI configuration space (I/O ports 0xCF8/0xCFC) across 256 buses, 32 devices, 8 functions.
  * Identify xHCI host controller (`Class 0x0C, Subclass 0x03, Prog-IF 0x30`).
  * Initialize xHCI hardware registers and rings.
  * Enumerate connected USB devices.
  * Establish MTP session with connected Android phone.
  * Retrieve Linux image stream into high RAM buffer (`0x0200_0000`).
  * Parse image format, extract `bzImage` and `initramfs`.
  * Construct Linux `boot_params` structure and transfer execution to Linux.
