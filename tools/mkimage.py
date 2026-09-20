#!/usr/bin/env python3
"""
tools/mkimage.py - Automated Boot Image Builder for Android -> Linux Bootloader
Conforms to BOOT_IMAGE_FORMAT.md.
"""

import sys
import os
import struct
import argparse

SECTOR_SIZE = 512
STAGE2_MAX_SECTORS = 8          # 4 KiB allocated for Stage 2
PARTITION_START_LBA = 2048      # 1 MiB alignment for partition 1

def build_mbr_partition(active: bool, ptype: int, start_lba: int, total_sectors: int) -> bytes:
    """Build a standard 16-byte MBR partition table entry with LBA addressing."""
    status = 0x80 if active else 0x00
    # CHS dummy values for LBA-only BIOS (0xFE, 0xFF, 0xFF)
    entry = bytearray(16)
    entry[0] = status
    entry[1:4] = b'\xfe\xff\xff'      # Start CHS (dummy)
    entry[4] = ptype                  # Partition type
    entry[5:8] = b'\xfe\xff\xff'      # End CHS (dummy)
    struct.pack_into("<I", entry, 8, start_lba)
    struct.pack_into("<I", entry, 12, total_sectors)
    return bytes(entry)

def main():
    parser = argparse.ArgumentParser(description="Build boot.img from stage1, stage2, and stage3 binaries.")
    parser.add_argument("--stage1", required=True, help="Path to stage1.bin")
    parser.add_argument("--stage2", required=True, help="Path to stage2.bin")
    parser.add_argument("--stage3", required=True, help="Path to stage3.bin")
    parser.add_argument("--output", required=True, help="Output boot.img path")
    parser.add_argument("--size-mb", type=int, default=64, help="Total image size in MiB (default: 64)")
    args = parser.parse_args()

    # Read stages
    with open(args.stage1, "rb") as f:
        stage1_data = bytearray(f.read())
    with open(args.stage2, "rb") as f:
        stage2_data = bytearray(f.read())
    with open(args.stage3, "rb") as f:
        stage3_data = bytearray(f.read())

    print(f"[*] Input sizes: Stage1={len(stage1_data)}B, Stage2={len(stage2_data)}B, Stage3={len(stage3_data)}B")

    # Validate Stage 1
    if len(stage1_data) != SECTOR_SIZE:
        if len(stage1_data) < SECTOR_SIZE:
            stage1_data.extend(b'\x00' * (SECTOR_SIZE - len(stage1_data)))
        else:
            sys.exit(f"[-] Error: Stage 1 size {len(stage1_data)} exceeds 512 bytes!")

    # Verify / set boot signature (0x55, 0xAA)
    stage1_data[510] = 0x55
    stage1_data[511] = 0xAA

    # Validate Stage 2
    stage2_sectors = (len(stage2_data) + SECTOR_SIZE - 1) // SECTOR_SIZE
    if stage2_sectors == 0:
        stage2_sectors = 1
    if stage2_sectors > STAGE2_MAX_SECTORS:
        sys.exit(f"[-] Error: Stage 2 ({len(stage2_data)} bytes = {stage2_sectors} sectors) exceeds max {STAGE2_MAX_SECTORS} sectors!")
    
    # Pad Stage 2 to exact sector boundary
    stage2_padded_len = stage2_sectors * SECTOR_SIZE
    stage2_data.extend(b'\x00' * (stage2_padded_len - len(stage2_data)))

    # Calculate Stage 3 layout
    stage3_sectors = (len(stage3_data) + SECTOR_SIZE - 1) // SECTOR_SIZE
    if stage3_sectors == 0:
        stage3_sectors = 1
    stage3_start_lba = 1 + stage2_sectors
    stage3_padded_len = stage3_sectors * SECTOR_SIZE
    stage3_data.extend(b'\x00' * (stage3_padded_len - len(stage3_data)))

    print(f"[*] Stage 2: LBA 1..{stage2_sectors} ({stage2_sectors} sectors)")
    print(f"[*] Stage 3: LBA {stage3_start_lba}..{stage3_start_lba + stage3_sectors - 1} ({stage3_sectors} sectors, load=0x00100000)")

    # Verify Stage 3 does not collide with partition 1
    if (stage3_start_lba + stage3_sectors) > PARTITION_START_LBA:
        sys.exit(f"[-] Error: Stage 3 extends past partition start LBA {PARTITION_START_LBA}!")

    # Patch Stage 1 header fields at known offset:
    # Offset 0x1B0: uint32 stage2_start_lba, uint16 stage2_sectors
    # (Before partition table at 0x1BE)
    struct.pack_into("<IH", stage1_data, 0x1B0, 1, stage2_sectors)

    # Patch Stage 2 header fields at offset 4:
    # Offset 0x00..0x03: jmp short entry, nop, nop
    # Offset 0x04: uint32 magic ("STG2" = 0x32475453)
    # Offset 0x08: uint32 stage3_start_lba
    # Offset 0x0C: uint32 stage3_sectors
    # Offset 0x10: uint32 stage3_load_addr (0x00100000)
    struct.pack_into("<IIII", stage2_data, 4, 0x32475453, stage3_start_lba, stage3_sectors, 0x00100000)

    # Construct MBR partition table at offset 446 (0x1BE) in Stage 1
    total_image_sectors = (args.size_mb * 1024 * 1024) // SECTOR_SIZE
    partition1_sectors = total_image_sectors - PARTITION_START_LBA
    p1 = build_mbr_partition(active=True, ptype=0x0C, start_lba=PARTITION_START_LBA, total_sectors=partition1_sectors)
    stage1_data[0x1BE:0x1CE] = p1
    # Partitions 2, 3, 4 empty
    stage1_data[0x1CE:0x1FE] = b'\x00' * 48

    # Create output image
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "wb") as out:
        # Write Stage 1 (LBA 0)
        out.write(stage1_data)
        # Write Stage 2 (LBA 1..N)
        out.write(stage2_data)
        # Write Stage 3 (LBA stage3_start_lba..)
        out.write(stage3_data)
        
        # Current written size
        written = len(stage1_data) + len(stage2_data) + len(stage3_data)
        target_size = args.size_mb * 1024 * 1024
        if written < target_size:
            # Pad up to target image size
            out.write(b'\x00' * (target_size - written))

    print(f"[+] Successfully generated boot image: {args.output} ({args.size_mb} MiB, {total_image_sectors} sectors)")

if __name__ == "__main__":
    main()
