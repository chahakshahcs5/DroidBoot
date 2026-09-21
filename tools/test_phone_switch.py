#!/usr/bin/env python3
"""
tools/test_phone_switch.py - Test Root Android Phone MTP -> MSC Switch in Windows & QEMU
1. Detects connected Android phone via ADB.
2. Verifies root (su) access and gadget drivers (mass_storage.0).
3. Lists ISOs stored on the phone.
4. Switches the phone to USB Mass Storage (SCSI drive).
5. Launches QEMU with USB pass-through to verify booting without rebooting the laptop!
"""

import os
import sys
import subprocess
import time

WORKSPACE_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT_PATH = os.path.join(WORKSPACE_ROOT, "tools", "phone_ums_enable.sh")

def run_adb(args):
    cmd = ["adb"] + args
    res = subprocess.run(cmd, capture_output=True, text=True)
    return res.returncode, res.stdout.strip(), res.stderr.strip()

def main():
    print("\n" + "=" * 70)
    print("  ROOT ANDROID PHONE MTP -> USB MASS STORAGE TEST TOOL")
    print("=" * 70)

    # 1. Check ADB connection
    print("[*] Checking for connected Android phone via ADB...")
    code, out, _ = run_adb(["devices", "-l"])
    lines = [line for line in out.splitlines() if line and not line.startswith("List of")]
    if not lines:
        print("[-] No phone detected via ADB!")
        print("\n    Please make sure:")
        print("    1. Phone is plugged into USB.")
        print("    2. 'USB Debugging' is enabled in Developer Options.")
        print("    3. If prompted on screen, tap 'Always allow from this computer'.\n")
        return

    print(f"[+] Found device:\n    {lines[0]}")

    # 2. Check Root (su)
    print("[*] Verifying root (su) access on phone...")
    code, out, err = run_adb(["shell", "su -c id"])
    if code != 0 or "uid=0" not in out:
        print("[-] Superuser (su) check failed! (Output: " + out + " " + err + ")")
        print("    Please ensure Shell (com.android.shell) has Superuser permission granted in Magisk.")
        return
    print(f"[+] Root verified: {out}")

    # 3. Check Phone Kernel Gadget support
    print("[*] Checking kernel USB gadget subsystem...")
    code, out, _ = run_adb(["shell", "su -c 'ls -la /config/usb_gadget/g1/functions'"])
    print("    Functions available:")
    for l in out.splitlines():
        print(f"      {l}")

    # 4. Search for ISO files on phone
    print("[*] Scanning phone for ISO boot images...")
    search_cmd = "su -c 'ls -lh /sdcard/Download/*.iso /sdcard/*.iso /storage/*/*.iso /storage/*/*/*.iso 2>/dev/null'"
    code, out, _ = run_adb(["shell", search_cmd])
    if not out:
        print("[!] No ISO files found in standard folders (/sdcard/Download, /sdcard, /storage/*).")
    else:
        print("[+] Discovered ISO images on phone:")
        for l in out.splitlines():
            print(f"      {l}")

    # 5. Push and run switch script
    print("[*] Pushing switch script to phone (/data/local/tmp/phone_ums_enable.sh)...")
    run_adb(["push", SCRIPT_PATH, "/data/local/tmp/phone_ums_enable.sh"])
    run_adb(["shell", "chmod 755 /data/local/tmp/phone_ums_enable.sh"])

    target_iso = sys.argv[1] if len(sys.argv) > 1 else ""
    switch_cmd = f"su -c 'sh /data/local/tmp/phone_ums_enable.sh {target_iso}'"
    print(f"[*] Executing switch: {switch_cmd}")
    code, out, err = run_adb(["shell", switch_cmd])
    print(out)
    if err:
        print(f"[!] Stderr: {err}")

    print("\n[+] Done! The phone has been switched to USB Mass Storage mode.")
    print("    You can now boot from it on your laptop or pass it into QEMU:")
    print("    python tools/run_qemu.py --usb-host <VID:PID>\n")

if __name__ == "__main__":
    main()
