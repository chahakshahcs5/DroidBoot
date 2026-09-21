#!/usr/bin/env python3
"""
read_bootlog.py - Extract and display BootManager boot logs from SD card.

Supports two modes:
  1. FAT32 file mode:  python read_bootlog.py BOOTLOG.TXT
  2. Raw device mode:  python read_bootlog.py /dev/sdX  (Linux)
                       python read_bootlog.py \\.\PhysicalDriveN  (Windows)

In raw mode, reads LBA 256-383 (raw log sectors) and parses the
disk_log_header_t structure to extract the log text, handling
circular buffer wrap-around.
"""

import sys
import struct
import os

# disk_log_header_t layout (512 bytes total):
#   uint32_t magic1        (offset 0)   "BOOT" = 0x544F4F42
#   uint32_t magic2        (offset 4)   "LOG!" = 0x21474F4C
#   uint32_t log_length    (offset 8)
#   uint32_t flush_count   (offset 12)
#   uint32_t boot_drive    (offset 16)
#   uint32_t error_count   (offset 20)
#   uint32_t log_start     (offset 24)  circular buffer start offset
#   uint32_t total_written (offset 28)  total bytes ever written
#   uint8_t  reserved[480]

RAW_LOG_MAGIC_1 = 0x544F4F42  # "BOOT"
RAW_LOG_MAGIC_2 = 0x21474F4C  # "LOG!"
RAW_LOG_LBA = 1024
RAW_LOG_SECTORS = 128  # 64 KiB
SECTOR_SIZE = 512
HEADER_SIZE = 512


def read_raw_log(device_path):
    """Read raw log from LBA 256-383 of a block device."""
    offset = RAW_LOG_LBA * SECTOR_SIZE
    total_size = RAW_LOG_SECTORS * SECTOR_SIZE

    try:
        with open(device_path, 'rb') as f:
            f.seek(offset)
            data = f.read(total_size)
    except PermissionError:
        print(f"ERROR: Permission denied reading '{device_path}'.")
        print("  Linux:   Run with sudo")
        print("  Windows: Run as Administrator")
        sys.exit(1)
    except FileNotFoundError:
        print(f"ERROR: Device '{device_path}' not found.")
        sys.exit(1)

    if len(data) < HEADER_SIZE:
        print(f"ERROR: Could not read {total_size} bytes from LBA {RAW_LOG_LBA}.")
        sys.exit(1)

    # Parse header
    magic1, magic2, log_length, flush_count, boot_drive, error_count, log_start, total_written = \
        struct.unpack_from('<IIIIIIII', data, 0)

    if magic1 != RAW_LOG_MAGIC_1 or magic2 != RAW_LOG_MAGIC_2:
        print(f"ERROR: No valid boot log found at LBA {RAW_LOG_LBA}.")
        print(f"  Expected magic: BOOT LOG! (0x{RAW_LOG_MAGIC_1:08X} 0x{RAW_LOG_MAGIC_2:08X})")
        print(f"  Found:          0x{magic1:08X} 0x{magic2:08X}")
        sys.exit(1)

    # Extract log text (starts at offset 512 in the raw data)
    max_text_bytes = (RAW_LOG_SECTORS - 1) * SECTOR_SIZE
    text_data = data[HEADER_SIZE : HEADER_SIZE + max_text_bytes]

    if log_length > len(text_data):
        log_length = len(text_data)

    # Handle circular buffer
    if log_start > 0 and log_start < log_length:
        # Circular: data wraps around. Read from log_start to end, then start to log_start
        log_text = text_data[log_start:log_length] + text_data[:log_start]
    else:
        log_text = text_data[:log_length]

    return {
        'log_length': log_length,
        'flush_count': flush_count,
        'boot_drive': boot_drive,
        'error_count': error_count,
        'log_start': log_start,
        'total_written': total_written,
        'text': log_text,
    }


def read_fat_file(file_path):
    """Read log from a FAT32 BOOTLOG.TXT file."""
    try:
        with open(file_path, 'rb') as f:
            data = f.read()
    except FileNotFoundError:
        print(f"ERROR: File '{file_path}' not found.")
        sys.exit(1)

    return {
        'log_length': len(data),
        'flush_count': 0,
        'boot_drive': 0,
        'error_count': 0,
        'log_start': 0,
        'total_written': len(data),
        'text': data,
    }


def main():
    if len(sys.argv) < 2:
        print("Usage: python read_bootlog.py <device_or_file>")
        print()
        print("Examples:")
        print("  python read_bootlog.py BOOTLOG.TXT              # Read FAT32 log file")
        print("  python read_bootlog.py /dev/sdb                  # Read raw LBA (Linux)")
        print("  python read_bootlog.py \\\\.\\PhysicalDrive1        # Read raw LBA (Windows)")
        print()
        print("Options:")
        print("  --raw       Force raw device mode")
        print("  --file      Force file mode")
        print("  --save OUT  Save extracted log to file")
        sys.exit(1)

    target = sys.argv[1]
    force_raw = '--raw' in sys.argv
    force_file = '--file' in sys.argv
    save_path = None
    if '--save' in sys.argv:
        idx = sys.argv.index('--save')
        if idx + 1 < len(sys.argv):
            save_path = sys.argv[idx + 1]

    # Auto-detect mode
    is_raw = force_raw
    if not force_raw and not force_file:
        # Heuristic: if it looks like a device path, use raw mode
        if target.startswith('/dev/') or target.startswith('\\\\.\\') or target.startswith('//./'):
            is_raw = True
        elif target.upper().endswith('.TXT') or target.upper().endswith('.LOG'):
            is_raw = False
        else:
            # Check if it's a block device
            try:
                stat = os.stat(target)
                import stat as stat_mod
                if stat_mod.S_ISBLK(stat.st_mode):
                    is_raw = True
            except (OSError, AttributeError):
                pass

    if is_raw:
        print(f"Reading raw boot log from device: {target}")
        print(f"  LBA range: {RAW_LOG_LBA}-{RAW_LOG_LBA + RAW_LOG_SECTORS - 1}")
        result = read_raw_log(target)
    else:
        print(f"Reading boot log from file: {target}")
        result = read_fat_file(target)

    # Display header info
    print()
    print("=" * 72)
    print("  BOOTMANAGER BOOT LOG")
    print("=" * 72)
    if is_raw:
        print(f"  Log Length    : {result['log_length']:,} bytes")
        print(f"  Total Written : {result['total_written']:,} bytes")
        print(f"  Flush Count   : {result['flush_count']}")
        print(f"  Error Count   : {result['error_count']}")
        print(f"  Boot Drive    : 0x{result['boot_drive']:02X}")
        print(f"  Circular Start: {result['log_start']}")
    print("=" * 72)
    print()

    # Decode and display log text
    try:
        text = result['text'].decode('ascii', errors='replace')
    except AttributeError:
        text = str(result['text'])

    # Clean up: replace \r\n with \n for display
    text = text.replace('\r\n', '\n').replace('\r', '\n')

    # Remove trailing nulls
    text = text.rstrip('\x00').rstrip('\n')

    print(text)
    print()
    print("=" * 72)
    print(f"  END OF LOG ({result['log_length']:,} bytes)")
    print("=" * 72)

    if save_path:
        with open(save_path, 'w', encoding='utf-8') as f:
            f.write(text)
        print(f"\nLog saved to: {save_path}")


if __name__ == '__main__':
    main()
