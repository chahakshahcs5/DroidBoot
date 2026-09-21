#!/usr/bin/env python3
"""
tools/read_log.py - Inspection utility for Android -> Linux Bootloader persistent logs
Reads persistent boot logs from a disk image (boot.img) or physical storage media (SD card / USB drive).
Supports both FAT32 BOOTLOG.TXT and raw fallback sectors (LBA 256..383).
"""

import sys
import os
import struct
import argparse
import time

SECTOR_SIZE = 512
RAW_LOG_LBA = 1024
PARTITION_START_LBA = 2048

def read_raw_log(f) -> tuple[dict, str]:
    """Reads the raw backup log from LBA 256..383."""
    f.seek(RAW_LOG_LBA * SECTOR_SIZE)
    hdr_data = f.read(512)
    if len(hdr_data) < 512:
        return None, "Incomplete read at LBA 256"

    magic1, magic2, log_len, flush_count, boot_drive, error_count = struct.unpack_from("<IIIIII", hdr_data, 0)
    if magic1 != 0x544F4F42 or magic2 != 0x21474F4C: # "BOOTLOG!"
        return None, f"Raw log magic mismatch: 0x{magic1:08X} 0x{magic2:08X}"

    if log_len > 65536 * 2:
        log_len = 65536

    # Text starts at LBA 257
    f.seek((RAW_LOG_LBA + 1) * SECTOR_SIZE)
    log_text_bytes = f.read(log_len)
    try:
        text = log_text_bytes.decode('utf-8', errors='replace')
    except Exception:
        text = log_text_bytes.decode('latin1', errors='replace')

    stats = {
        "source": f"Raw Sectors (LBA {RAW_LOG_LBA}..)",
        "magic": "BOOTLOG!",
        "log_length": log_len,
        "flush_count": flush_count,
        "error_count": error_count,
        "boot_drive": f"0x{boot_drive:02X}",
    }
    return stats, text

def read_fat32_log(f) -> tuple[dict, str]:
    """Attempts to parse FAT32 partition 1 and extract BOOTLOG.TXT."""
    # 1. Read MBR
    f.seek(0)
    mbr = f.read(512)
    if len(mbr) < 512 or mbr[510] != 0x55 or mbr[511] != 0xAA:
        return None, "Invalid MBR signature"

    part1_lba = struct.unpack_from("<I", mbr, 0x1BE + 8)[0]
    if part1_lba == 0:
        part1_lba = PARTITION_START_LBA

    # 2. Read VBR
    f.seek(part1_lba * SECTOR_SIZE)
    vbr = f.read(512)
    if len(vbr) < 512:
        return None, "Failed to read VBR"

    bps = struct.unpack_from("<H", vbr, 11)[0]
    spc = vbr[13]
    reserved = struct.unpack_from("<H", vbr, 14)[0]
    num_fats = vbr[16]
    fat_size = struct.unpack_from("<I", vbr, 36)[0]
    root_cluster = struct.unpack_from("<I", vbr, 44)[0]

    if bps != 512 or spc == 0 or fat_size == 0 or root_cluster < 2:
        return None, "Invalid FAT32 BPB parameters"

    fat_start_lba = part1_lba + reserved
    data_start_lba = fat_start_lba + (num_fats * fat_size)
    root_dir_lba = data_start_lba + (root_cluster - 2) * spc

    # 3. Read Root Directory
    f.seek(root_dir_lba * SECTOR_SIZE)
    root_data = f.read(spc * SECTOR_SIZE)

    target_83 = b'BOOTLOG TXT'
    file_cluster = None
    file_size = 0

    for i in range(0, len(root_data), 32):
        ent = root_data[i:i+32]
        if ent[0] == 0x00:
            break
        if ent[0] == 0xE5 or ent[11] == 0x0F:
            continue
        if ent[0:11] == target_83:
            clus_hi = struct.unpack_from("<H", ent, 20)[0]
            clus_lo = struct.unpack_from("<H", ent, 26)[0]
            file_cluster = (clus_hi << 16) | clus_lo
            file_size = struct.unpack_from("<I", ent, 28)[0]
            break

    if file_cluster is None:
        return None, "BOOTLOG.TXT not found in FAT32 root directory"

    # 4. Read File Content
    # If file_size is small or unupdated, read at least 64KB or file_size
    bytes_to_read = max(file_size, 4096)
    if bytes_to_read > 65536:
        bytes_to_read = 65536

    file_lba = data_start_lba + (file_cluster - 2) * spc
    f.seek(file_lba * SECTOR_SIZE)
    raw_content = f.read(bytes_to_read)

    if file_size > 0 and file_size <= len(raw_content):
        raw_content = raw_content[:file_size]
    else:
        # Strip trailing nulls if file size was not updated
        raw_content = raw_content.rstrip(b'\x00')

    try:
        text = raw_content.decode('utf-8', errors='replace')
    except Exception:
        text = raw_content.decode('latin1', errors='replace')

    stats = {
        "source": f"FAT32 Partition 1 (/BOOTLOG.TXT)",
        "partition_lba": part1_lba,
        "file_cluster": file_cluster,
        "file_lba": file_lba,
        "file_size": f"{len(text)} bytes (dir size: {file_size} bytes)",
    }
    return stats, text

def watch_mode(image_path, use_raw, interval):
    """Continuously polls the disk image/device for new log content, like 'tail -f'."""
    print(f"[*] Watching '{image_path}' for new log content (poll every {interval}s, Ctrl+C to stop)...")
    print("=" * 70)

    last_length = 0
    last_text = ""
    first_read = True

    try:
        while True:
            try:
                with open(image_path, "rb") as f:
                    if use_raw:
                        stats, text = read_raw_log(f)
                    else:
                        stats, text = read_fat32_log(f)
                        if not stats:
                            stats, text = read_raw_log(f)

                    if not stats:
                        if first_read:
                            print(f"[*] No log data found yet ({text}). Waiting...")
                            first_read = False
                        time.sleep(interval)
                        continue

                    current_length = len(text)

                    if first_read:
                        # Print header and full content on first successful read
                        print(f"  Source: {stats['source']}")
                        if 'flush_count' in stats:
                            print(f"  Flushes: {stats['flush_count']} | Errors: {stats.get('error_count', 'N/A')}")
                        print("=" * 70)
                        print(text, end='')
                        last_text = text
                        last_length = current_length
                        first_read = False
                    elif current_length > last_length:
                        # Print only new content
                        new_content = text[last_length:]
                        print(new_content, end='', flush=True)
                        last_text = text
                        last_length = current_length
                    elif text != last_text:
                        # Content changed but didn't grow (re-written from scratch)
                        print("\n--- LOG RESET DETECTED ---")
                        print(text, end='')
                        last_text = text
                        last_length = current_length

            except PermissionError:
                if first_read:
                    print(f"[*] Device busy or permission denied. Retrying...")
                    first_read = False
            except OSError as e:
                if first_read:
                    print(f"[*] I/O error: {e}. Retrying...")
                    first_read = False

            time.sleep(interval)

    except KeyboardInterrupt:
        print(f"\n\n[*] Watch mode stopped. Last log size: {last_length} bytes.")

def main():
    parser = argparse.ArgumentParser(description="Read persistent boot logs from bootloader media")
    parser.add_argument("image", help="Path to boot.img or drive device (e.g. \\\\.\\PhysicalDrive1 or /dev/sdb)")
    parser.add_argument("--raw", action="store_true", help="Force reading raw sectors at LBA 256")
    parser.add_argument("--output", "-o", help="Save log output to a file")
    parser.add_argument("--watch", "-w", action="store_true",
                        help="Watch mode: continuously poll for new log content (like 'tail -f')")
    parser.add_argument("--interval", type=float, default=1.0,
                        help="Poll interval in seconds for watch mode (default: 1.0)")
    args = parser.parse_args()

    if not os.path.exists(args.image) and not args.image.startswith("\\\\.\\"):
        sys.exit(f"[-] Error: File or device '{args.image}' not found.")

    # Watch mode: continuous polling
    if args.watch:
        watch_mode(args.image, args.raw, args.interval)
        return

    try:
        f = open(args.image, "rb")
    except PermissionError:
        sys.exit(f"[-] Permission denied opening '{args.image}'. Run as Administrator/root if reading physical disks.")

    with f:
        stats = None
        text = None

        if not args.raw:
            stats, text = read_fat32_log(f)
            if not stats:
                print(f"[*] Note: FAT32 read skipped ({text}), falling back to raw sectors...")
                stats, text = read_raw_log(f)
        else:
            stats, text = read_raw_log(f)

        if not stats:
            sys.exit(f"[-] Error: Could not read boot log: {text}")

        print("=" * 70)
        print("  PERSISTENT BOOTLOADER LOG EXTRACTOR")
        print("=" * 70)
        for k, v in stats.items():
            print(f"  {k:15}: {v}")
        print("=" * 70)
        print()

        if args.output:
            with open(args.output, "w", encoding="utf-8") as out_f:
                out_f.write(text)
            print(f"[+] Successfully wrote {len(text)} bytes to '{args.output}'")
        else:
            print(text)

if __name__ == "__main__":
    main()
