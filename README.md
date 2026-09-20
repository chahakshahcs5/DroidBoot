# Android → Linux Legacy BIOS Bootloader

An experimental x86/x86-64 Legacy BIOS bootloader that boots from an SD card, initializes the laptop's USB host controller (xHCI), communicates with an unmodified Android smartphone over USB MTP (Media Transfer Protocol), retrieves a Linux kernel/initramfs image from Android internal storage, and boots into real Linux.

```
Laptop Powers On
      ↓
 Legacy BIOS
      ↓
   SD Card
      ↓
 Bootloader (Stage 1 → Stage 2 → Stage 3)
      ↓
 USB Host Controller (xHCI)
      ↓
  Android Phone (USB MTP)
      ↓
 Android Internal Storage (/Download/)
      ↓
 Linux Image (bzImage + initramfs)
      ↓
 Laptop RAM
      ↓
 Linux 32-bit Boot Protocol
      ↓
   REAL LINUX
```

---

## Core Engineering Principle

> **BUILD THE UNIQUE PARTS. REUSE MATURE OPEN-SOURCE COMPONENTS FOR MATURE SUBSYSTEMS.**

Rather than implementing every complex protocol from scratch, this project stands on mature, battle-tested open-source firmware and bootloader components:
* **xHCI & USB Stack**: Adapted from **coreboot / libpayload** (BSD 3-Clause).
* **FAT32 Filesystem**: Embedded **ChaN's FatFs (R0.15)** (Permissive ChaN license).
* **MTP Protocol**: Wire format definitions from **Android AOSP** (`media/mtp`), parsing logic inspired by **libmtp**.
* **Linux Boot Protocol**: Built strictly against the official **Linux x86 Boot Protocol** (`Documentation/arch/x86/boot.rst`) with **SeaBIOS** `boot_linux` design reference.
* **ext2/3/4 Partition Reader**: **lwext4** (BSD 2-Clause).
* **Multi-Stage BIOS Bootstrap**: Clean NASM 16-bit MBR to 32-bit Protected Mode transition inspired by **Syslinux** and **Limine**.

---

## Project Structure

```
bootmanager/
├── src/
│   ├── stage1/           # 512-byte BIOS MBR boot sector (real mode NASM)
│   ├── stage2/           # Bootstrap loader (A20, E820, Unreal/Prot mode)
│   ├── stage3/           # 32-bit Protected Mode C runtime
│   │   ├── core/         # Entry point, console, panic, string, timer
│   │   ├── memory/       # E820 parser, physical memory allocator
│   │   ├── bios/         # BIOS data area / interrupt structures
│   │   ├── pci/          # PCI bus configuration scanner
│   │   ├── xhci/         # xHCI host controller driver (libpayload-derived)
│   │   ├── usb/          # USB enumeration and descriptor parser
│   │   ├── mtp/          # MTP initiator, packet framing, file streaming
│   │   ├── filesystem/   # ChaN FatFs & lwext4 integration
│   │   ├── image/        # MBR/GPT partition parser & format detector
│   │   ├── linux/        # Linux 32-bit boot protocol loader
│   │   ├── boot/         # Generic BootSource abstraction & boot menu
│   │   └── debug/        # Serial port logger (COM1 0x3F8) & diagnostics
│   └── include/          # Shared bootloader header files
├── tools/
│   ├── mkimage.py        # Automated boot.img image builder & sector validator
│   └── qemu_test.py      # Automated test runner with serial log assertions
├── Makefile              # Native / WSL GNU Makefile
├── build.py              # Cross-platform Python build orchestrator
├── ARCHITECTURE.md       # Detailed technical architecture specification
├── MEMORY_MAP.md         # Full physical memory layout & ownership
├── BOOT_IMAGE_FORMAT.md  # Disk sector layout and stage offsets
├── TESTING.md            # QEMU & physical hardware test strategy
├── THIRD_PARTY.md        # Open-source license and source tracking
└── PROJECT_STATUS.md     # Current phase progress and verification status
```

---

## Build Requirements

* **Assembler**: NASM (>= 2.15)
* **Compiler**: GCC with 32-bit multilib (`gcc -m32`)
* **Linker**: GNU `ld -m elf_i386`
* **Python**: Python 3.8+
* **Emulator**: QEMU (`qemu-system-x86_64`)

### Building the Boot Image

On Windows / WSL:
```bash
# Build boot.img using Makefile (in WSL or Linux):
make

# Or using the Python build orchestrator:
python build.py
```

Outputs generated in `build/`:
* `stage1.bin` (exactly 512 bytes)
* `stage2.bin` (sector-aligned bootstrap)
* `stage3.bin` (32-bit flat C runtime ELF/binary)
* `boot.img` (final bootable disk image with verified sector layout)

### Running in QEMU

```bash
make run
# or
python build.py --run
```
