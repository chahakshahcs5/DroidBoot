# Android → Linux Legacy BIOS Bootloader

An advanced x86/x86-64 Legacy BIOS bare-metal bootloader that boots from an SD card or USB drive, initializes the PC's USB 3.x host controller (xHCI), communicates with an Android smartphone over USB (via MTP, ADB, or USB Mass Storage), discovers Linux images stored on the phone, and boots into real Linux via the official 32-bit Linux Boot Protocol.

```
                  Laptop Powers On
                         ↓
                    Legacy BIOS
                         ↓
              SD Card / USB Boot Disk
                         ↓
             Stage 1 (MBR) → Stage 2 → Stage 3
                         ↓
             xHCI Host Controller Driver
                         ↓
            USB Port Probing & Enumeration
                         ↓
         +---------------+---------------+
         |                               |
  [Rooted Phone / UMS]         [Unmodified Phone / MTP]
  ADB Shell Gadget Switch      PTP / MTP Container Stream
         |                               |
  Direct Block Access             In-RAM Staging Buffer
  (0 MB OS in RAM!)               (mBFT Table Installed)
         |                               |
         +---------------+---------------+
                         ↓
               Interactive Boot Menu
        (VGA Console / PS/2 & BIOS Keyboard / Serial)
                         ↓
              Linux 32-bit Boot Protocol
        (boot_params, e820 map, initramfs, cmdline)
                         ↓
                  REAL LINUX KERNEL
```

---

## Dual Boot Architectures

1. **Direct Block Access Mode (0 MB OS in RAM)**:
   * Uses the **ADB Root Bridge** (`src/stage3/adb/`) to configure Android's kernel `configfs` gadget driver (`mass_storage.0`) or `sysfs`.
   * Attaches any `.iso` or `.img` on internal storage (`/sdcard/Download/`, `/storage/emulated/0/`) to a USB Mass Storage LUN.
   * Phone instantly re-enumerates as a hardware USB drive.
   * Only the lightweight kernel (~14 MB) and initrd (~20-90 MB) are staged in memory; large 4–6 GB live systems stream filesystem blocks on-demand directly over USB.

2. **In-RAM MTP Streaming Mode**:
   * Works with **unmodified, non-rooted Android phones** over standard USB MTP.
   * Traverses phone storage datasets via PTP opcodes (`OpenSession`, `GetStorageIDs`, `GetObjectHandles`, `GetObjectInfo`).
   * Streams the ISO image in 1 MB chunks into high RAM (`0x04000000`).
   * Deploys an **mBFT** (MEMDISK Boot Information Table) at `0x000E0000` so Alpine/Linux decompressors and `memdiskfind` locate the root filesystem.
   * Supports persistent user overlays stored on the boot SD card (`apkovl=sda1:` or Casper persistence).

---

## Core Engineering Principle

> **BUILD THE UNIQUE PARTS. REUSE MATURE OPEN-SOURCE COMPONENTS FOR MATURE SUBSYSTEMS.**

Rather than implementing every complex protocol from scratch, this project stands on mature, battle-tested open-source firmware and bootloader components:
* **xHCI & USB Stack**: Adapted from **coreboot / libpayload** (BSD 3-Clause).
* **USB Mass Storage**: Bulk-Only Transport (BOT) with SCSI transparent command set (SPC-2/SBC-2).
* **FAT32 Filesystem**: Embedded **ChaN's FatFs (R0.15)** (Permissive ChaN license).
* **MTP Protocol**: Wire format definitions from **Android AOSP** (`media/mtp`), parsing logic inspired by **libmtp**.
* **Android ADB Protocol**: Freestanding CNXN handshake, RSA token authentication, and command execution.
* **Linux Boot Protocol**: Built strictly against the official **Linux x86 Boot Protocol** (`Documentation/arch/x86/boot.rst`) with **SeaBIOS** `boot_linux` design reference.
* **ext2/3/4 Partition Reader**: **lwext4** (BSD 2-Clause).
* **Multi-Stage BIOS Bootstrap**: Clean NASM 16-bit MBR to 32-bit Protected Mode transition inspired by **Syslinux** and **Limine**.
* **BIOS Thunking**: Real-to-Protected mode gateway (`bios_thunk.S`) for INT 13h (disk), INT 10h (VBE/video), and INT 16h (universal laptop keyboard polling).

---

## Project Structure

```
bootmanager/
├── src/
│   ├── stage1/           # 512-byte BIOS MBR boot sector (16-bit real mode NASM)
│   ├── stage2/           # Bootstrap loader (A20, E820, protected mode transition)
│   ├── stage3/           # 32-bit Flat Protected Mode C runtime
│   │   ├── core/         # Entry point (entry.S), main orchestrator (main.c), printf
│   │   ├── memory/       # E820 memory map parser, physical bump/pool allocator
│   │   ├── bios/         # 16-bit BIOS thunk (bios_thunk.S), BIOS disk I/O, VBE video
│   │   ├── pci/          # PCI bus configuration scanner (ports 0xCF8 / 0xCFC)
│   │   ├── xhci/         # xHCI host controller driver (libpayload-derived)
│   │   ├── usb/          # USB enumeration, descriptor parsing, USB MSC block driver
│   │   ├── mtp/          # MTP initiator, packet framing, file streaming (AOSP/libmtp)
│   │   ├── adb/          # ADB authentication bridge, RSA auth, root UMS switch
│   │   ├── filesystem/   # ChaN FatFs, ISO 9660 reader (iso_reader.c)
│   │   ├── image/        # bzImage/MBR/GPT detector, multi-OS scanner (os_scanner.c)
│   │   ├── linux/        # 32-bit Linux boot protocol, boot_params setup, trampoline
│   │   ├── ui/           # Interactive color VGA & serial boot menu (menu.c)
│   │   └── debug/        # Dual SD disk logger (disk_log.c), serial (0x3F8), PC speaker
│   └── include/          # Shared bootloader header files (boot.h, boot_source.h, io.h)
├── tools/
│   ├── mkimage.py                  # Automated boot.img builder with FAT32 partition & logs
│   ├── qemu_test.py                # Automated headless test runner with assertion checking
│   ├── run_qemu.py                 # Interactive QEMU launcher with display & serial
│   ├── test_alpine_msc.py          # Automated test for Alpine Linux Direct Block Boot
│   ├── test_ubuntu_msc.py          # Automated test for Ubuntu Casper Direct Block Boot
│   ├── test_dynamic_msc_switch.py  # Test for runtime USB disconnect/reconnect & UMS switch
│   ├── test_phone_switch.py        # Automated ADB trigger & re-enumeration test
│   ├── test_physical_phone_qemu.py # Passthrough physical Android phone via USB to QEMU
│   ├── read_log.py                 # Extractor for BOOTLOG.TXT and raw LBA 256 backup logs
│   ├── phone_ums_enable.sh         # On-phone shell script to configure mass_storage gadget
│   └── phone_restore_mtp.sh        # On-phone shell script to restore MTP USB configuration
├── Makefile              # Native GNU Makefile (WSL / Linux)
├── build.py              # Cross-platform Python build orchestrator
├── ARCHITECTURE.md       # Detailed technical architecture specification
├── MEMORY_MAP.md         # Full physical memory layout & ownership
├── BOOT_IMAGE_FORMAT.md  # Disk sector layout, stage offsets, and log areas
├── TESTING.md            # QEMU & physical hardware test strategy
├── THIRD_PARTY.md        # Open-source license and source tracking
└── PROJECT_STATUS.md     # Phase progress and verification status matrix
```

---

## Build Requirements

* **Assembler**: NASM (>= 2.15)
* **Compiler**: GCC with 32-bit multilib (`gcc -m32`)
* **Linker**: GNU `ld -m elf_i386`
* **Python**: Python 3.8+
* **Emulator**: QEMU (`qemu-system-x86_64`)

### Building the Boot Image

On Windows / WSL / Linux:
```bash
# Build boot.img using Makefile:
make

# Or using the cross-platform Python build orchestrator:
python build.py
```

Outputs generated in `build/`:
* `stage1.bin` (exactly 512 bytes)
* `stage2.bin` (sector-aligned bootstrap)
* `stage3.bin` (32-bit flat C runtime binary)
* `boot.img` (final bootable 64 MiB disk image with verified sector layout and FAT32 partition)

### Running & Testing

```bash
# Run interactive QEMU session:
python build.py --run

# Run automated headless assertion test:
python tools/qemu_test.py

# Test Alpine Linux USB block boot:
python tools/test_alpine_msc.py

# Test Ubuntu Casper USB block boot:
python tools/test_ubuntu_msc.py

# Passthrough your physical connected Android smartphone to QEMU:
python tools/test_physical_phone_qemu.py
```

