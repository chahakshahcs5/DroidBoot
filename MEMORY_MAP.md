# Physical Memory Map & Ownership

This document defines the physical memory layout of the system during boot, specifying memory ownership, stage load addresses, hardware structures, and buffer allocations.

All addresses are physical addresses below 4 GiB accessible in 32-bit flat protected mode.

---

## Physical Address Map

```
+-----------------------------------+ 0x0000_0000
| Real-Mode IVT (1024 B)            | Real-Mode Interrupt Vector Table
+-----------------------------------+ 0x0000_0400
| BIOS Data Area (256 B)            | BDA (timer ticks, equipment list)
+-----------------------------------+ 0x0000_0500
| Early Scratch / Low Stack (26 KB) | Stage 1 & 2 scratch memory
+-----------------------------------+ 0x0000_7000
| BIOS Thunk Code & Mailbox (512 B) | Real-Mode Thunk gateway (bios_thunk.S)
+-----------------------------------+ 0x0000_7C00
| Stage 1 MBR Boot Sector (512 B)   | Loaded by BIOS, executed at 0x7C00
+-----------------------------------+ 0x0000_7E00
| Disk Address Packet (DAP) (512 B) | INT 13h extended read structures
+-----------------------------------+ 0x0000_8000
| Stage 2 Bootstrap / Trampoline    | Real-mode setup; reused for Linux Trampoline
+-----------------------------------+ 0x0000_9000
| E820 Map / Linux boot_params (4K) | Up to 128 E820 records; Linux boot_params
+-----------------------------------+ 0x0000_9A00
| Linux Command Line (1.5 KiB)      | Formatted kernel cmdline string
+-----------------------------------+ 0x0001_0000
| Stage 2 Staging Buffer (64 KiB)   | Low-memory bounce buffer for Stage 3 load
+-----------------------------------+ 0x0008_0000
| Stage 3 C Stack (128 KiB)         | Grows downwards from 0x0009_FFF0
+-----------------------------------+ 0x000A_0000
| Video RAM, Option ROMs, BIOS ROM  | RESERVED HARDWARE REGION (VGA at 0xB8000)
+-----------------------------------+ 0x000E_0000
| mBFT ACPI Table (256 B)           | MEMDISK Boot Information Table for In-RAM ISO
+-----------------------------------+ 0x0010_0000 (1 MiB BOUNDARY)
| Stage 3 C Runtime Code & Data     | .text, .rodata, .data, .bss (reused by Linux kernel)
+-----------------------------------+ 0x0040_0000 (4 MiB)
| xHCI DMA Memory Region (4 MiB)    | DCBAA, Rings, Scratchpads, TRBs
+-----------------------------------+ 0x0080_0000 (8 MiB)
| Bootloader Heap (24 MiB)          | Dynamic memory allocator
+-----------------------------------+ 0x0200_0000 (32 MiB)
| Kernel Buffer (bzImage) & Initrd  | Staging buffer for kernel and safe initrd address
+-----------------------------------+ 0x0400_0000 (64 MiB)
| In-RAM ISO Staging Buffer (96 MiB)| Holds streamed ISO when booting via MTP
+-----------------------------------+ 0x0A00_0000 (160 MiB)
| Available System RAM              | Free memory discovered via E820
+-----------------------------------+ 
| End of Physical RAM               | End of Usable RAM (from E820 map)
+-----------------------------------+
```

---

## Detailed Subsystem Ownership

### 1. Stage 1 (0x0000_7C00 - 0x0000_7DFF)
* Size: Exactly 512 bytes.
* Set up by: BIOS POST.
* Entry state: Real Mode (16-bit), `CS:IP = 0x0000:0x7C00`, `DL = BIOS Boot Drive`.
* Output: Preserves `DL`, loads Stage 2 into `0x8000`, jumps to `0x8000`.

### 2. Stage 2 (0x0000_8000 - 0x0000_8FFF)
* Size: Up to 4 KiB (8 sectors).
* Real mode responsibilities:
  * Probes E820 memory map into `0x0000_9000`.
  * Enables A20 gate.
  * Loads Stage 3 from disk sectors into low conventional memory (`0x0001_0000`) via `INT 13h`.
  * Loads 32-bit GDT (`gdt_descriptor`).
  * Switches CPU to Protected Mode (`CR0.PE = 1`).
  * Relocates Stage 3 from `0x0001_0000` to `0x0010_0000` (1 MiB).
  * Performs far jump: `jmp 0x08:stage3_entry`.
* **Re-use during Linux Boot**: The address `0x0000_8000` is overwritten by the 39-to-60 byte `linux_boot_jump` trampoline during final kernel handoff.

### 3. Real-Mode BIOS Thunking (0x0000_7000 - 0x0000_71FF)
* Trampoline code copied to `0x0000_7000`.
* Mailbox at `0x0000_7100` stores arguments (drive, command, DAP offset), saved 32-bit `ESP`, and return codes.
* Allows Stage 3 to call `INT 13h` (disk logging to SD card), `INT 10h` (VBE video), and `INT 16h` (keyboard polling).

### 4. Stage 3 C Runtime (0x0010_0000 - 0x003F_FFFF)
* 32-bit Flat Protected Mode C runtime linked at 1 MiB (`0x0010_0000`).
* Stack located below 1 MiB (`0x0008_0000` - `0x0009_FFFF`), avoiding collision with 1 MiB+ memory.

### 5. xHCI DMA Structures (0x0040_0000 - 0x007F_FFFF)
* **DCBAA (Device Context Base Address Array)**: 64-byte aligned, holds 64-bit pointers to Device Contexts.
* **Command Ring**: 64-byte aligned circular TRB buffer.
* **Event Ring & ERST**: 64-byte aligned Event Ring Segment Table and Segment Buffers.
* **Scratchpad Buffers**: Page-aligned (4096-byte) physical pages allocated for xHCI internal state saving.
* **Transfer Rings**: 64-byte aligned per-endpoint TRB rings.

### 6. RAM Footprint Comparison by Boot Mode

| Mode | Kernel & Initrd | Filesystem Footprint in RAM | Total RAM Consumed |
| :--- | :--- | :--- | :--- |
| **Direct Block Access (UMS/USB MSC)** | ~35–100 MB at `0x0200_0000` | **0 MB** (Streamed on demand over USB) | **~35–100 MB** |
| **In-RAM ISO Boot (MTP Streaming)** | ~35–100 MB at `0x0200_0000` | Full ISO cached at `0x0400_0000` (Type 2 Reserved) | **~500 MB – 2 GB** |

