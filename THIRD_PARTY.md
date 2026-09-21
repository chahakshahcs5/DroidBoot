# Third-Party Open-Source Components & Licensing

This document tracks all external open-source code, specifications, and reference implementations used in this project, in accordance with the project specification.

---

## Tracking Matrix

| Component | Upstream Project | License | Source / Repository URL | Files / Concepts Used | Modifications / Integration Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **xHCI Host Controller Driver** | coreboot / libpayload | **BSD 3-Clause** | `https://review.coreboot.org/cgit/coreboot.git/tree/payloads/libpayload/drivers/usb/xhci.c` | `drivers/usb/xhci.c`, `drivers/usb/xhci_rh.c`, `drivers/usb/xhci_private.h`, `include/usb/xhci.h` | Adapted freestanding MMIO access, replaced coreboot heap with bootloader 32-bit DMA/memory allocator, converted to synchronous polling for single-threaded boot runtime. |
| **USB Enumeration & Core** | coreboot / libpayload | **BSD 3-Clause** | `https://review.coreboot.org/cgit/coreboot.git/tree/payloads/libpayload/drivers/usb` | `drivers/usb/usb.c`, `drivers/usb/usbinit.c`, `include/usb/usb.h`, `include/usb/usbdescriptors.h` | Retained standard descriptor decoders, integrated class/subclass filter for MTP (`0x06/0x01/0x01`), ADB (`0xFF/0x42/0x01`), and MSC (`0x08/0x06/0x50`). |
| **USB Mass Storage (MSC / SCSI BOT)** | USB-IF & T10 SCSI Specifications | **Open Standard / Public Domain** | `https://www.usb.org/document-library/mass-storage-class-specification-overview-14` | Command Block Wrapper (CBW), Command Status Wrapper (CSW), SCSI SPC-2/SBC-2 (`TEST_UNIT_READY`, `INQUIRY`, `READ_CAPACITY_10`, `READ_10`) | Freestanding implementation supporting 2048-byte CD sectors and 512-byte disk sectors for direct OS streaming. |
| **Android ADB Wire Protocol** | Android Open Source Project (AOSP) | **Apache 2.0** | `https://android.googlesource.com/platform/packages/modules/adb/` | ADB message framing (`A_CNXN`, `A_AUTH`, `A_OPEN`, `A_OKAY`, `A_CLSE`, `A_WRTE`), token checksums, RSA key exchange | Freestanding C implementation for automated root shell bridge and UMS gadget switching. |
| **FAT Filesystem** | ChaN's FatFs (R0.15) | **ChaN / BSD-like** | `http://elm-chan.org/fsw/ff/00index_e.html` | `ff.c`, `ff.h`, `diskio.h`, `ffconf.h` | Configured for minimal read-only footprint (`FF_FS_READONLY = 1`), integrated with BIOS INT 13h block layer for SD boot. |
| **MTP Protocol Constants & Framing** | Android Open Source Project (AOSP) `media/mtp` | **Apache 2.0** | `https://android.googlesource.com/platform/frameworks/av/+/refs/heads/main/media/mtp/` | `mtp.h`, `MtpPacket.h` | Extracted wire packet headers, container types, opcode constants, and property codes into freestanding C structs. |
| **MTP Parsing Reference** | libmtp | **LGPL-2.1-or-later** | `https://github.com/libmtp/libmtp` | `src/ptp.h`, `src/ptp.c` | Referenced for response parsing logic (`StorageInfo`, `ObjectInfo`, object handle arrays, UTF-16 decoding). No POSIX/libusb dependencies ported. |
| **Linux Boot Protocol Specification** | Linux Kernel Source | **GPLv2** | `https://www.kernel.org/doc/html/latest/arch/x86/boot.html` (`Documentation/arch/x86/boot.rst`) | `arch/x86/include/uapi/asm/bootparam.h`, `arch/x86/boot/header.S` | Header magic (`0x53726448`), `struct boot_params`, `struct setup_header`, protected mode kernel entry requirements. |
| **mBFT (MEMDISK Boot Information Table)** | Syslinux / MEMDISK | **GPLv2** | `https://repo.or.cz/syslinux.git/blob/HEAD:/memdisk/memdisk.c` | `struct mBFT` ACPI table format | Deployed at `0x000E0000` to pass in-memory ISO physical coordinates to Linux `memdiskfind` and `phram`. |
| **Linux Loader Reference** | SeaBIOS | **LGPLv3 / GPLv3** | `https://github.com/coreboot/seabios` | `src/boot.c` (`boot_linux`) | Logic for populating `boot_params`, installing E820 table, aligning initramfs, jumping to kernel entry point. |
| **ext2/3/4 Partition Reader** | lwext4 | **BSD 2-Clause** | `https://github.com/gkostka/lwext4` | `src/ext4*.c`, `include/ext4*.h` | Lightweight read-only ext4 parser attached to the `BootSource` abstraction. |
| **BIOS Bootstrap Patterns** | Syslinux / Limine | **GPLv2+ / BSD 2-Clause** | `https://repo.or.cz/syslinux.git` / `https://github.com/limine-bootloader/limine` | `core/mbr.S`, `core/ldlinux.asm`, DAP INT 13h extensions | Clean MBR sector layout, Unreal Mode loading, preservation of BIOS drive register `DL`. |

---

## License Texts & Notices

All incorporated third-party files retain their original copyright and license headers. The full texts of the BSD 3-Clause, BSD 2-Clause, Apache 2.0, LGPL-2.1, and GPLv2 licenses are maintained in the repository under `licenses/`.

