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
| Early Scratch / Low Stack (29 KB) | Stage 1 & 2 scratch memory
+-----------------------------------+ 0x0000_7C00
| Stage 1 MBR Boot Sector (512 B)   | Loaded by BIOS, executed at 0x7C00
+-----------------------------------+ 0x0000_7E00
| Disk Address Packet (DAP) (512 B) | INT 13h extended read structures
+-----------------------------------+ 0x0000_8000
| Stage 2 Bootstrap (4 KiB)         | Real-mode setup, A20, E820, GDT
+-----------------------------------+ 0x0000_9000
| E820 Memory Map Buffer (4 KiB)    | Up to 128 INT 15h E820 records
+-----------------------------------+ 0x0000_A000
| Low Sector Buffers (~472 KiB)     | BIOS INT 13h bounce buffers (<1 MB)
+-----------------------------------+ 0x0000_80000
| Stage 3 C Stack (128 KiB)         | Grows downwards from 0x0009_FFFF
+-----------------------------------+ 0x000A_0000
| Video RAM, Option ROMs, BIOS ROM  | RESERVED HARDWARE REGION
+-----------------------------------+ 0x0010_0000 (1 MiB BOUNDARY)
| Stage 3 C Runtime Code & Data     | .text, .rodata, .data, .bss (3 MiB)
+-----------------------------------+ 0x0040_0000 (4 MiB)
| xHCI DMA Memory Region (4 MiB)    | DCBAA, Rings, Scratchpads, TRBs
+-----------------------------------+ 0x0080_0000 (8 MiB)
| Bootloader Heap (24 MiB)          | Dynamic memory allocator
+-----------------------------------+ 0x0200_0000 (32 MiB)
| MTP Transfer & Image Buffer (128M)| Holds retrieved Linux image from Android
+-----------------------------------+ 0x0A00_0000 (160 MiB)
| Available System RAM              | Free memory discovered via E820
+-----------------------------------+ 
| Linux Kernel & Initramfs Target   | Relocated according to Linux boot protocol
+-----------------------------------+ End of Physical RAM (from E820)
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
  * Loads Stage 3 from disk sectors into `0x0010_0000` (1 MiB).
  * Loads 32-bit GDT (`gdt_descriptor`).
  * Switches CPU to Protected Mode (`CR0.PE = 1`).
  * Performs far jump: `jmp 0x08:stage3_entry`.

### 3. Stage 3 (0x0010_0000 - 0x003F_FFFF)
* 32-bit Flat Protected Mode C runtime.
* Code, rodata, data, and BSS linked at 1 MiB (`0x0010_0000`).
* Stack located below 1 MiB (`0x0008_0000` - `0x0009_FFFF`), avoiding collision with 1 MiB+ memory.

### 4. xHCI DMA Structures (0x0040_0000 - 0x007F_FFFF)
* **DCBAA (Device Context Base Address Array)**: 64-byte aligned, holds 64-bit pointers to Device Contexts.
* **Command Ring**: 64-byte aligned circular TRB buffer.
* **Event Ring & ERST**: 64-byte aligned Event Ring Segment Table and Segment Buffers.
* **Scratchpad Buffers**: Page-aligned (4096-byte) physical pages allocated for xHCI internal state saving.
* **Transfer Rings**: 64-byte aligned per-endpoint TRB rings.

### 5. MTP Transfer Buffer (0x0200_0000 - 0x09FF_FFFF)
* 128 MiB contiguous memory region dedicated to receiving large Linux images streamed from Android `/Download/`.
* High enough above bootloader code to prevent accidental corruption.
