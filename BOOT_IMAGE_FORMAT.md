# Boot Image Format & Disk Layout

This document defines the exact on-disk sector layout of `boot.img` generated automatically by `tools/mkimage.py`.

---

## Disk Sector Layout

Standard sector size = 512 bytes (`0x200`).

```
Sector (LBA)        Byte Offset Range         Contents
---------------------------------------------------------------------------------
LBA 0               0x000000 - 0x0001FF       Stage 1 (MBR Boot Sector - 512 Bytes)
                    Offset 0x000 - 0x1BD:     Stage 1 Assembly Code
                    Offset 0x1BE - 0x1FD:     Standard MBR Partition Table (64 B)
                    Offset 0x1FE - 0x1FF:     Boot Signature (0x55, 0xAA)

LBA 1 .. 8          0x000200 - 0x0011FF       Stage 2 Bootstrap Loader (8 Sectors = 4 KiB)
                    Offset 0x000 - 0x003:     Stage 2 Magic ("STG2" = 0x32475453)
                    Offset 0x004 - 0x007:     Stage 3 Start LBA (LBA 9)
                    Offset 0x008 - 0x00B:     Stage 3 Sector Count (Calculated)
                    Offset 0x00C - 0x00F:     Stage 3 Memory Load Address (0x00100000)

LBA 9 .. 264        0x001200 - 0x0211FF       Stage 3 Main C Runtime Binary (up to 128 KiB+)
                    Flat binary linked at 0x00100000 (.text, .rodata, .data)

LBA 265 .. 2047     0x021200 - 0x0FFFFF       Reserved Boot Alignment Padding (to 1 MiB boundary)

LBA 2048 .. END     0x100000 - END            FAT32 Partition (SD Boot Source Filesystem)
                                              Contains optional local boot files & test files.
```

---

## Image Generation Constraints

1. **Deterministic Build**: Running `mkimage.py` on unchanged inputs produces byte-for-byte identical output.
2. **Alignment & Padding**: Every stage is strictly padded to a 512-byte sector boundary.
3. **Overlap Prevention**: The builder calculates exact byte lengths, verifies that Stage 1 does not exceed 446 code bytes (leaving MBR partition table and signature intact), verifies Stage 2 fits in its allocated sectors, and updates the header embedded at the start of Stage 2 with the exact sector count of Stage 3.
4. **Zero Manual Editing**: Manual hex editing of the boot image is strictly forbidden.
