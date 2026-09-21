#!/usr/bin/env python3
"""
read_bootlog.py - Extract and display BootManager boot logs from SD card / USB drive or image.

Supports:
  1. FAT32 file mode:     python read_bootlog.py BOOTLOG.TXT
  2. Boot session file:   python read_bootlog.py BOOT0001.LOG
  3. Raw device / image:  python read_bootlog.py /dev/sdX  (Linux)
                          python read_bootlog.py \\\\.\\PhysicalDriveN  (Windows)
                          python read_bootlog.py build/boot.img
  4. List all boot logs:  python read_bootlog.py --list build/boot.img
  5. Read specific boot:  python read_bootlog.py --boot 2 build/boot.img
"""

import sys
import struct
import os
import subprocess

RAW_LOG_MAGIC_1 = 0x544F4F42  # "BOOT"
RAW_LOG_MAGIC_2 = 0x21474F4C  # "LOG!"
RAW_LOG_LBA = 1024
RAW_LOG_SECTORS = 128  # 64 KiB
SECTOR_SIZE = 512
HEADER_SIZE = 512


def list_phone_logs():
    """List all available boot logs stored on connected Android phone."""
    print("[*] Querying boot logs catalog on Android phone via ADB...")
    cmd = ['adb', 'shell', 'ls -1 -t /sdcard/BootManager/logs 2>/dev/null']
    try:
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        logs = [line.strip() for line in res.stdout.splitlines() if line.strip().endswith('.log') or line.strip().endswith('.LOG')]
        print()
        print("=" * 72)
        print("  BOOTMANAGER PHONE LOG CATALOG (/sdcard/BootManager/)")
        print("=" * 72)
        print(f"  {'Filename':<40} {'Type'}")
        print("  " + "-" * 68)
        print(f"  {'boot.log':<40} Current Active / Latest Boot Log")
        for f in logs:
            print(f"  {f:<40} Historical Per-Boot Archive (logs/{f})")
        print("=" * 72)
        print()
        print("To view a specific log:")
        print("  python tools/read_bootlog.py --phone --file <filename>")
        print()
        return logs
    except Exception as e:
        print(f"[-] Failed to query logs on phone: {e}")
        return []


def read_phone_log(target_file=None):
    """Retrieve boot log directly from connected Android phone via ADB."""
    if target_file:
        phone_path = f"/sdcard/BootManager/logs/{target_file}" if not target_file.startswith('/') else target_file
        print(f"[*] Attempting to retrieve '{phone_path}' from Android phone via ADB...")
    else:
        phone_path = "/sdcard/BootManager/boot.log"
        print("[*] Attempting to retrieve latest boot log from Android phone via ADB...")

    # Try su cat first
    cmd1 = ['adb', 'shell', 'su', '-c', f'cat {phone_path}']
    try:
        res = subprocess.run(cmd1, capture_output=True, timeout=10)
        if res.returncode == 0 and len(res.stdout) > 0 and b'No such file' not in res.stdout and b'Permission denied' not in res.stdout:
            print(f"[+] Successfully fetched {phone_path} (root)")
            return {
                'log_length': len(res.stdout),
                'flush_count': 0,
                'boot_drive': 0,
                'error_count': 0,
                'log_start': 0,
                'total_written': len(res.stdout),
                'boot_count': 0,
                'session_name': f"{phone_path} (phone)",
                'text': res.stdout,
            }
    except Exception:
        pass

    # Try unprivileged cat
    cmd2 = ['adb', 'shell', 'cat', phone_path]
    try:
        res = subprocess.run(cmd2, capture_output=True, timeout=10)
        if res.returncode == 0 and len(res.stdout) > 0 and b'No such file' not in res.stdout and b'Permission denied' not in res.stdout:
            print(f"[+] Successfully fetched {phone_path}")
            return {
                'log_length': len(res.stdout),
                'flush_count': 0,
                'boot_drive': 0,
                'error_count': 0,
                'log_start': 0,
                'total_written': len(res.stdout),
                'boot_count': 0,
                'session_name': f"{phone_path} (phone)",
                'text': res.stdout,
            }
    except Exception:
        pass

    # Try /data/local/tmp/boot.log as fallback if latest requested
    if not target_file:
        cmd3 = ['adb', 'shell', 'cat', '/data/local/tmp/boot.log']
        try:
            res = subprocess.run(cmd3, capture_output=True, timeout=10)
            if res.returncode == 0 and len(res.stdout) > 0 and b'No such file' not in res.stdout and b'Permission denied' not in res.stdout:
                print("[+] Successfully fetched /data/local/tmp/boot.log")
                return {
                    'log_length': len(res.stdout),
                    'flush_count': 0,
                    'boot_drive': 0,
                    'error_count': 0,
                    'log_start': 0,
                    'total_written': len(res.stdout),
                    'boot_count': 0,
                    'session_name': '/data/local/tmp/boot.log (phone fallback)',
                    'text': res.stdout,
                }
        except Exception:
            pass

    print(f"[-] Failed to read boot log '{phone_path}' from phone.")
    print("    Ensure phone is connected via USB with USB Debugging enabled (run 'adb devices').")
    sys.exit(1)


def read_raw_log(device_path):
    """Read raw log from LBA 1024..1151 of a block device or image file."""
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
        print(f"ERROR: Device or file '{device_path}' not found.")
        sys.exit(1)

    if len(data) < HEADER_SIZE:
        print(f"ERROR: Could not read {total_size} bytes from LBA {RAW_LOG_LBA}.")
        sys.exit(1)

    # Parse header
    magic1, magic2, log_length, flush_count, boot_drive, error_count, log_start, total_written, boot_count, session_name_raw = \
        struct.unpack_from('<IIIIIIIII16s', data, 0)

    if magic1 != RAW_LOG_MAGIC_1 or magic2 != RAW_LOG_MAGIC_2:
        print(f"ERROR: No valid boot log found at LBA {RAW_LOG_LBA}.")
        print(f"  Expected magic: BOOT LOG! (0x{RAW_LOG_MAGIC_1:08X} 0x{RAW_LOG_MAGIC_2:08X})")
        print(f"  Found:          0x{magic1:08X} 0x{magic2:08X}")
        sys.exit(1)

    session_name = session_name_raw.split(b'\x00')[0].decode('ascii', errors='ignore')
    if not session_name:
        session_name = f"BOOT{boot_count:04d}.LOG"

    max_text_bytes = (RAW_LOG_SECTORS - 1) * SECTOR_SIZE
    text_data = data[HEADER_SIZE : HEADER_SIZE + max_text_bytes]

    if log_length > len(text_data):
        log_length = len(text_data)

    if log_start > 0 and log_start < log_length:
        log_text = text_data[log_start:log_length] + text_data[:log_start]
    else:
        log_text = text_data[:log_length]

    return {
        'magic1': magic1,
        'magic2': magic2,
        'log_length': log_length,
        'flush_count': flush_count,
        'boot_drive': boot_drive,
        'error_count': error_count,
        'log_start': log_start,
        'total_written': total_written,
        'boot_count': boot_count,
        'session_name': session_name,
        'text': log_text,
    }


def read_fat_file(file_path):
    """Read log from a standalone file."""
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
        'boot_count': 0,
        'session_name': os.path.basename(file_path),
        'text': data,
    }


def list_and_extract_fat_logs(device_path, target_boot=None):
    """Scans Partition 1 FAT32 filesystem in device or image file for all log files."""
    try:
        with open(device_path, 'rb') as f:
            # Read MBR
            mbr = f.read(512)
            if len(mbr) < 512 or mbr[510:512] != b'\x55\xAA':
                return None

            part1_lba = struct.unpack_from('<I', mbr, 0x1BE + 8)[0]
            if part1_lba == 0 or part1_lba > 10000000:
                part1_lba = 2048

            f.seek(part1_lba * 512)
            vbr = f.read(512)
            if len(vbr) < 512 or vbr[510:512] != b'\x55\xAA':
                return None

            bytes_per_sec, spc, res_sec, nfats, total_sec16, media, fat_sz16 = \
                struct.unpack_from('<HBHBHBH', vbr, 11)
            fat_sz32, root_cl = struct.unpack_from('<II', vbr, 36)[:2]
            root_cl = struct.unpack_from('<I', vbr, 44)[0]

            fat_sz = fat_sz32 if fat_sz32 > 0 else fat_sz16
            fat_start = part1_lba + res_sec
            data_start = fat_start + (nfats * fat_sz)
            root_lba = data_start + (root_cl - 2) * spc

            # Read root directory
            f.seek(root_lba * 512)
            root_dir = f.read(spc * 512)

            logs = []
            matched_content = None

            for i in range(0, len(root_dir), 32):
                ent = root_dir[i:i+32]
                if ent[0] == 0x00:
                    break
                if ent[0] == 0xE5 or ent[11] == 0x0F:
                    continue

                name_83 = ent[:11]
                attr = ent[11]
                if attr & 0x18:
                    continue

                first_cl = (struct.unpack_from('<H', ent, 20)[0] << 16) | struct.unpack_from('<H', ent, 26)[0]
                size = struct.unpack_from('<I', ent, 28)[0]

                clean_name = name_83[:8].decode('ascii', errors='ignore').rstrip() + '.' + \
                             name_83[8:11].decode('ascii', errors='ignore').rstrip()

                if clean_name.startswith("BOOT") and (clean_name.endswith(".LOG") or clean_name.endswith(".TXT")):
                    logs.append((clean_name, first_cl, size))

                    # Check if this matches requested target_boot
                    if isinstance(target_boot, int):
                        target_name = f"BOOT{target_boot:04d}.LOG"
                    elif target_boot:
                        target_name = str(target_boot).upper()
                    else:
                        target_name = None

                    if target_name and clean_name == target_name:
                        # Extract this file
                        file_lba = data_start + (first_cl - 2) * spc
                        f.seek(file_lba * 512)
                        matched_content = f.read(size)

            return logs, matched_content
    except Exception as e:
        return None


def main():
    if len(sys.argv) < 2:
        print("Usage: python read_bootlog.py [OPTIONS] [device_or_file]")
        print()
        print("Options:")
        print("  --phone      Fetch and display boot log directly from connected phone via ADB")
        print("  --list       List all available boot logs found on device/image")
        print("  --boot N     Extract and display boot log for session N (e.g. --boot 1)")
        print("  --raw        Force raw device mode (LBA 1024)")
        print("  --file       Force file mode")
        print("  --save OUT   Save extracted log to file")
        print()
        print("Examples:")
        print("  python read_bootlog.py --phone")
        print("  python read_bootlog.py build/boot.img")
        print("  python read_bootlog.py --list build/boot.img")
        print("  python read_bootlog.py --boot 2 build/boot.img")
        print("  python read_bootlog.py BOOTLOG.TXT")
        sys.exit(1)

    do_phone = '--phone' in sys.argv
    do_list = '--list' in sys.argv
    boot_num = None
    if '--boot' in sys.argv:
        idx = sys.argv.index('--boot')
        if idx + 1 < len(sys.argv):
            try:
                boot_num = int(sys.argv[idx + 1])
            except ValueError:
                pass

    save_path = None
    if '--save' in sys.argv:
        idx = sys.argv.index('--save')
        if idx + 1 < len(sys.argv):
            save_path = sys.argv[idx + 1]

    force_raw = '--raw' in sys.argv
    force_file = '--file' in sys.argv

    if do_phone:
        if do_list:
            list_phone_logs()
            sys.exit(0)
        target_phone_file = None
        if '--file' in sys.argv:
            idx = sys.argv.index('--file')
            if idx + 1 < len(sys.argv):
                target_phone_file = sys.argv[idx + 1]
        result = read_phone_log(target_phone_file)
        # Jump directly to log display
        target = None
    else:
        # Find target path (first argument not starting with -- and not argument value)
        args = sys.argv[1:]
        target = None
        skip = False
        for a in args:
            if skip:
                skip = False
                continue
            if a in ('--boot', '--save'):
                skip = True
                continue
            if not a.startswith('--'):
                target = a
                break

        if not target:
            print("ERROR: No device or file path provided.")
            sys.exit(1)

    # If --list requested:
    if do_list:
        fat_info = list_and_extract_fat_logs(target)
        if fat_info and fat_info[0]:
            print()
            print("=" * 72)
            print(f"  BOOTMANAGER LOG CATALOG: {target}")
            print("=" * 72)
            print(f"  {'Filename':<16} {'Size (Bytes)':<14} {'Description'}")
            print("  " + "-" * 68)
            for fname, cl, sz in fat_info[0]:
                desc = "Current Active / Latest Log" if fname == "BOOTLOG.TXT" else "Historical Per-Boot Archive"
                print(f"  {fname:<16} {sz:<14,} {desc}")
            print("=" * 72)
            sys.exit(0)
        else:
            print(f"[-] No FAT32 boot logs catalog could be read from '{target}'.")
            sys.exit(1)

    # If --boot N requested:
    if boot_num is not None:
        fat_info = list_and_extract_fat_logs(target, target_boot=boot_num)
        if fat_info and fat_info[1] is not None:
            content = fat_info[1]
            result = {
                'log_length': len(content),
                'flush_count': 0,
                'boot_drive': 0,
                'error_count': 0,
                'log_start': 0,
                'total_written': len(content),
                'boot_count': boot_num,
                'session_name': f"BOOT{boot_num:04d}.LOG",
                'text': content,
            }
        else:
            print(f"[-] Boot session {boot_num} not found or empty in '{target}'.")
            sys.exit(1)
    elif not do_phone:
        # Default read
        is_raw = force_raw
        if not force_raw and not force_file:
            if target.startswith('/dev/') or target.startswith('\\\\.\\') or target.startswith('//./'):
                is_raw = True
            elif target.endswith('.img') or target.endswith('.bin'):
                is_raw = True
            elif target.upper().endswith('.TXT') or target.upper().endswith('.LOG'):
                is_raw = False
            else:
                is_raw = True

        if is_raw:
            result = read_raw_log(target)
        else:
            result = read_fat_file(target)

    # Display Header Info
    print()
    print("=" * 72)
    print("  BOOTMANAGER BOOT LOG")
    print("=" * 72)
    print(f"  Session File  : {result.get('session_name', 'BOOTLOG.TXT')}")
    if result.get('boot_count', 0) > 0:
        print(f"  Boot Session  : #{result['boot_count']}")
    print(f"  Log Length    : {result['log_length']:,} bytes")
    print(f"  Total Written : {result['total_written']:,} bytes")
    if result.get('flush_count', 0) > 0:
        print(f"  Flush Count   : {result['flush_count']}")
        print(f"  Error Count   : {result['error_count']}")
        print(f"  Boot Drive    : 0x{result['boot_drive']:02X}")
    print("=" * 72)
    print()

    # Decode and print log text
    try:
        text = result['text'].decode('ascii', errors='replace')
    except AttributeError:
        text = str(result['text'])

    text = text.replace('\r\n', '\n').replace('\r', '\n').rstrip('\x00').rstrip('\n')
    print(text)
    print()
    print("=" * 72)
    print(f"  END OF LOG ({result['log_length']:,} bytes)")
    print("=" * 72)

    if save_path:
        with open(save_path, 'w', encoding='utf-8') as f:
            f.write(text)
        print(f"\n[+] Log successfully saved to: {save_path}")


if __name__ == '__main__':
    main()
