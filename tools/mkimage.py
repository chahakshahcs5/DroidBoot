#!/usr/bin/env python3
"""
tools/mkimage.py - Automated Boot Image Builder for Android -> Linux Bootloader
Conforms to BOOT_IMAGE_FORMAT.md.
Creates MBR partition table, raw backup log sectors (LBA 256), and
formats Partition 1 (LBA 2048) as FAT32 with pre-allocated BOOTLOG.TXT.
"""

import sys
import os
import struct
import argparse

SECTOR_SIZE = 512
STAGE2_MAX_SECTORS = 8          # 4 KiB allocated for Stage 2
RAW_LOG_LBA = 1024              # Raw backup log header sector
PARTITION_START_LBA = 2048      # 1 MiB alignment for partition 1

INITIAL_BOOTLOG_TEXT = (
    "======================================================================\r\n"
    "  ANDROID -> LINUX BOOTLOADER - PERSISTENT DEBUG LOG\r\n"
    "  Media: SD Card / USB Boot Disk (FAT32 Partition 1)\r\n"
    "======================================================================\r\n"
    "[BOOT] Persistent logging subsystem initialized.\r\n"
    "[STATUS] Ready for hardware boot. Power on target PC.\r\n"
).encode('ascii')

def build_mbr_partition(active: bool, ptype: int, start_lba: int, total_sectors: int) -> bytes:
    """Build a standard 16-byte MBR partition table entry with LBA addressing."""
    status = 0x80 if active else 0x00
    entry = bytearray(16)
    entry[0] = status
    entry[1:4] = b'\xfe\xff\xff'      # Start CHS (dummy)
    entry[4] = ptype                  # Partition type (0x0C = FAT32 LBA)
    entry[5:8] = b'\xfe\xff\xff'      # End CHS (dummy)
    struct.pack_into("<I", entry, 8, start_lba)
    struct.pack_into("<I", entry, 12, total_sectors)
    return bytes(entry)


def create_fat32_partition(total_sectors: int, hidden_lba: int, adb_key_data: bytes = None) -> bytes:
    """Creates a FAT32 filesystem containing pre-allocated BOOTLOG.TXT, BOOTCNT.DAT, per-boot historical logs, and optional ADBKEY.PUB."""
    bytes_per_sector = 512
    sectors_per_cluster = 8          # 4 KiB clusters
    reserved_sectors = 32
    num_fats = 2
    root_cluster = 2
    fat_size = 256                   # 256 sectors per FAT = 128 KiB
    
    fat1_lba = reserved_sectors
    fat2_lba = fat1_lba + fat_size
    data_start_lba = fat2_lba + fat_size

    part_bytes = bytearray(total_sectors * bytes_per_sector)

    # 1. Volume Boot Record (VBR at Sector 0 and Backup at Sector 6)
    vbr = bytearray(512)
    vbr[0:3] = b'\xeb\x58\x90'
    vbr[3:11] = b'MSWIN4.1'
    struct.pack_into("<H", vbr, 11, bytes_per_sector)
    vbr[13] = sectors_per_cluster
    struct.pack_into("<H", vbr, 14, reserved_sectors)
    vbr[16] = num_fats
    struct.pack_into("<H", vbr, 17, 0)
    struct.pack_into("<H", vbr, 19, 0)
    vbr[21] = 0xF8                  # Fixed disk / media descriptor
    struct.pack_into("<H", vbr, 22, 0)
    struct.pack_into("<H", vbr, 24, 63)
    struct.pack_into("<H", vbr, 26, 255)
    struct.pack_into("<I", vbr, 28, hidden_lba)
    struct.pack_into("<I", vbr, 32, total_sectors)
    struct.pack_into("<I", vbr, 36, fat_size)
    struct.pack_into("<H", vbr, 40, 0)
    struct.pack_into("<H", vbr, 42, 0)
    struct.pack_into("<I", vbr, 44, root_cluster)
    struct.pack_into("<H", vbr, 48, 1)  # FSInfo sector
    struct.pack_into("<H", vbr, 50, 6)  # Backup VBR sector
    vbr[64] = 0x80                      # BIOS drive number
    vbr[66] = 0x29                      # Extended boot signature
    struct.pack_into("<I", vbr, 67, 0x55AA1234)
    vbr[71:82] = b'BOOTLOADER '
    vbr[82:90] = b'FAT32   '
    vbr[510] = 0x55
    vbr[511] = 0xAA

    part_bytes[0:512] = vbr
    part_bytes[6*512:7*512] = vbr

    # 2. FSInfo (Sector 1 and Sector 7)
    fsinfo = bytearray(512)
    struct.pack_into("<I", fsinfo, 0, 0x41615252)   # "RRaA"
    struct.pack_into("<I", fsinfo, 484, 0x61417272) # "rrAa"
    total_data_sectors = total_sectors - data_start_lba
    total_clusters = total_data_sectors // sectors_per_cluster
    free_clusters = max(0, total_clusters - 182)
    struct.pack_into("<I", fsinfo, 488, free_clusters)
    struct.pack_into("<I", fsinfo, 492, 182)
    struct.pack_into("<I", fsinfo, 508, 0xAA550000)

    part_bytes[1*512:2*512] = fsinfo
    part_bytes[7*512:8*512] = fsinfo

    # 3. FAT1 and FAT2
    # Cluster 0: 0x0FFFFFF8, Cluster 1: 0x0FFFFFFF
    # Cluster 2 (Root dir): 0x0FFFFFFF
    # Cluster 3..17: Next cluster pointer
    # Cluster 18: 0x0FFFFFFF (End of BOOTLOG.TXT chain, 64 KiB)
    # Cluster 19: 0x0FFFFFFF (BOOTCNT.DAT - Monotonic boot session counter)
    # Clusters 20..179: 10 dedicated boot session logs (BOOT0001.LOG..BOOT0010.LOG, 16 clusters = 64 KiB each)
    # Cluster 180: Optional ADBKEY.PUB (if present)
    fat_table = bytearray(fat_size * bytes_per_sector)
    struct.pack_into("<I", fat_table, 0*4, 0x0FFFFFF8)
    struct.pack_into("<I", fat_table, 1*4, 0x0FFFFFFF)
    struct.pack_into("<I", fat_table, 2*4, 0x0FFFFFFF)
    for c in range(3, 18):
        struct.pack_into("<I", fat_table, c*4, c + 1)
    struct.pack_into("<I", fat_table, 18*4, 0x0FFFFFFF)

    # Cluster 19 (BOOTCNT.DAT)
    struct.pack_into("<I", fat_table, 19*4, 0x0FFFFFFF)

    # Clusters 20..179 (10 dedicated per-boot historical log files, 16 clusters = 64 KiB each)
    for b in range(10):
        start_c = 20 + b * 16
        for c in range(start_c, start_c + 15):
            struct.pack_into("<I", fat_table, c*4, c + 1)
        struct.pack_into("<I", fat_table, (start_c + 15)*4, 0x0FFFFFFF)

    # Cluster 180 (Optional ADBKEY.PUB)
    if adb_key_data:
        struct.pack_into("<I", fat_table, 180*4, 0x0FFFFFFF)

    part_bytes[fat1_lba*512:(fat1_lba + fat_size)*512] = fat_table
    part_bytes[fat2_lba*512:(fat2_lba + fat_size)*512] = fat_table

    # 4. Root Directory (Cluster 2)
    root_dir_offset = data_start_lba * 512
    root_dir = bytearray(sectors_per_cluster * 512)
    dir_idx = 0

    # Volume ID entry
    ent0 = bytearray(32)
    ent0[0:11] = b'BOOTLOADER '
    ent0[11] = 0x08 # ATTR_VOLUME_ID
    root_dir[dir_idx:dir_idx+32] = ent0
    dir_idx += 32

    # BOOTLOG.TXT entry (Always holds the latest boot log)
    ent1 = bytearray(32)
    ent1[0:11] = b'BOOTLOG TXT'
    ent1[11] = 0x20 # ATTR_ARCHIVE
    struct.pack_into("<H", ent1, 20, 0)             # First cluster HI
    struct.pack_into("<H", ent1, 26, 3)             # First cluster LO (Cluster 3)
    struct.pack_into("<I", ent1, 28, len(INITIAL_BOOTLOG_TEXT)) # File size
    root_dir[dir_idx:dir_idx+32] = ent1
    dir_idx += 32

    # BOOTCNT.DAT entry (Cluster 19, holds monotonic boot counter)
    ent_cnt = bytearray(32)
    ent_cnt[0:11] = b'BOOTCNT DAT'
    ent_cnt[11] = 0x20 # ATTR_ARCHIVE
    struct.pack_into("<H", ent_cnt, 20, 0)
    struct.pack_into("<H", ent_cnt, 26, 19)         # Cluster 19
    struct.pack_into("<I", ent_cnt, 28, 4)          # 4 bytes (uint32)
    root_dir[dir_idx:dir_idx+32] = ent_cnt
    dir_idx += 32

    # 10 Dedicated Per-Boot Historical Log Slots (BOOT0001.LOG .. BOOT0010.LOG)
    for b in range(10):
        slot_num = b + 1
        name_83 = f"BOOT{slot_num:04d}LOG".encode('ascii')
        ent_b = bytearray(32)
        ent_b[0:11] = name_83
        ent_b[11] = 0x20 # ATTR_ARCHIVE
        start_c = 20 + b * 16
        struct.pack_into("<H", ent_b, 20, 0)
        struct.pack_into("<H", ent_b, 26, start_c)
        init_size = len(INITIAL_BOOTLOG_TEXT) if b == 0 else 0
        struct.pack_into("<I", ent_b, 28, init_size)
        root_dir[dir_idx:dir_idx+32] = ent_b
        dir_idx += 32

    # ADBKEY.PUB entry (Cluster 180) if present
    if adb_key_data:
        ent2 = bytearray(32)
        ent2[0:11] = b'ADBKEY  PUB'
        ent2[11] = 0x20 # ATTR_ARCHIVE
        struct.pack_into("<H", ent2, 20, 0)
        struct.pack_into("<H", ent2, 26, 180)       # Cluster 180
        struct.pack_into("<I", ent2, 28, len(adb_key_data))
        root_dir[dir_idx:dir_idx+32] = ent2
        dir_idx += 32

    part_bytes[root_dir_offset:root_dir_offset + len(root_dir)] = root_dir

    # 5. Populate BOOTLOG.TXT cluster 3
    file_offset = (data_start_lba + (3 - 2) * sectors_per_cluster) * 512
    part_bytes[file_offset:file_offset + len(INITIAL_BOOTLOG_TEXT)] = INITIAL_BOOTLOG_TEXT

    # 6. Populate BOOTCNT.DAT cluster 19 (Initial boot counter = 1)
    cnt_offset = (data_start_lba + (19 - 2) * sectors_per_cluster) * 512
    struct.pack_into("<I", part_bytes, cnt_offset, 1)

    # 7. Populate BOOT0001.LOG cluster 20
    b1_offset = (data_start_lba + (20 - 2) * sectors_per_cluster) * 512
    part_bytes[b1_offset:b1_offset + len(INITIAL_BOOTLOG_TEXT)] = INITIAL_BOOTLOG_TEXT

    # 8. Populate ADBKEY.PUB cluster 180 if present
    if adb_key_data:
        key_offset = (data_start_lba + (180 - 2) * sectors_per_cluster) * 512
        part_bytes[key_offset:key_offset + len(adb_key_data)] = adb_key_data

    return bytes(part_bytes)


def main():
    parser = argparse.ArgumentParser(description="Build boot.img from stage1, stage2, and stage3 binaries.")
    parser.add_argument("--stage1", required=True, help="Path to stage1.bin")
    parser.add_argument("--stage2", required=True, help="Path to stage2.bin")
    parser.add_argument("--stage3", required=True, help="Path to stage3.bin")
    parser.add_argument("--output", required=True, help="Output boot.img path")
    parser.add_argument("--size-mb", type=int, default=64, help="Total image size in MiB (default: 64)")
    parser.add_argument("--adb-key", default=os.path.expanduser("~/.android/adbkey.pub"),
                        help="Path to ADB public key (default: ~/.android/adbkey.pub)")
    args = parser.parse_args()

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

    stage1_data[510] = 0x55
    stage1_data[511] = 0xAA

    # Validate Stage 2
    stage2_sectors = (len(stage2_data) + SECTOR_SIZE - 1) // SECTOR_SIZE
    if stage2_sectors == 0:
        stage2_sectors = 1
    if stage2_sectors > STAGE2_MAX_SECTORS:
        sys.exit(f"[-] Error: Stage 2 ({len(stage2_data)} bytes = {stage2_sectors} sectors) exceeds max {STAGE2_MAX_SECTORS} sectors!")
    
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

    if (stage3_start_lba + stage3_sectors) > RAW_LOG_LBA:
        sys.exit(f"[-] Error: Stage 3 extends past raw log LBA {RAW_LOG_LBA}!")

    # Patch Stage 1 header fields at offset 0x1B0
    struct.pack_into("<IH", stage1_data, 0x1B0, 1, stage2_sectors)

    # Patch Stage 2 header fields at offset 4
    struct.pack_into("<IIII", stage2_data, 4, 0x32475453, stage3_start_lba, stage3_sectors, 0x00100000)

    # Construct MBR partition table at offset 446 (0x1BE) in Stage 1
    total_image_sectors = (args.size_mb * 1024 * 1024) // SECTOR_SIZE
    partition1_sectors = total_image_sectors - PARTITION_START_LBA
    p1 = build_mbr_partition(active=True, ptype=0x0C, start_lba=PARTITION_START_LBA, total_sectors=partition1_sectors)
    stage1_data[0x1BE:0x1CE] = p1
    stage1_data[0x1CE:0x1FE] = b'\x00' * 48

    # Create raw backup log header at LBA 256
    raw_log_header = bytearray(512)
    struct.pack_into("<IIIII", raw_log_header, 0, 0x544F4F42, 0x21474F4C, len(INITIAL_BOOTLOG_TEXT), 0, 0x80)

    adb_key_data = None
    if os.path.exists(args.adb_key):
        try:
            with open(args.adb_key, "rb") as kf:
                adb_key_data = kf.read().strip()
            print(f"[+] Loaded dynamic host ADB key ({len(adb_key_data)} B) from: {args.adb_key}")
        except Exception as e:
            print(f"[!] Warning reading ADB key: {e}")

    # Generate FAT32 filesystem for Partition 1
    print(f"[*] Formatting Partition 1 (LBA {PARTITION_START_LBA}..{total_image_sectors - 1}) as FAT32 with BOOTLOG.TXT...")
    fat32_data = create_fat32_partition(partition1_sectors, hidden_lba=PARTITION_START_LBA, adb_key_data=adb_key_data)

    # Create output image
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "wb") as out:
        # 1. Stage 1 (LBA 0)
        out.write(stage1_data)
        
        # 2. Stage 2 (LBA 1..N)
        out.write(stage2_data)
        
        # 3. Stage 3 (LBA stage3_start_lba..)
        out.write(stage3_data)
        
        # Pad up to LBA 256
        curr_pos = out.tell()
        raw_lba_pos = RAW_LOG_LBA * SECTOR_SIZE
        if curr_pos < raw_lba_pos:
            out.write(b'\x00' * (raw_lba_pos - curr_pos))
        
        # 4. Raw log header (LBA 256) and initial text (LBA 257)
        out.write(raw_log_header)
        out.write(INITIAL_BOOTLOG_TEXT)
        
        # Pad up to Partition 1 (LBA 2048)
        curr_pos = out.tell()
        part1_pos = PARTITION_START_LBA * SECTOR_SIZE
        if curr_pos < part1_pos:
            out.write(b'\x00' * (part1_pos - curr_pos))
        
        # 5. FAT32 Partition 1
        out.write(fat32_data)

    print(f"[+] Successfully generated boot image: {args.output} ({args.size_mb} MiB, {total_image_sectors} sectors)")
    print(f"[+] Partition 1: FAT32 with pre-allocated BOOTLOG.TXT (LBA {PARTITION_START_LBA}..)")
    print(f"[+] Raw backup log: LBA {RAW_LOG_LBA}..{RAW_LOG_LBA + 127}")

if __name__ == "__main__":
    main()
