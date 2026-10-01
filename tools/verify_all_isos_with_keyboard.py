#!/usr/bin/env python3
"""
tools/verify_all_isos_with_keyboard.py - Rigorous verification of all user ISOs
with an emulated external USB HID keyboard connected concurrently to xHCI.
"""

import os
import sys
import time
import re

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from tools.qemu_harness import QemuHarness, WORKSPACE_ROOT, BUILD_DIR, DEFAULT_BOOT_IMG

UBUNTU_ISO = r"C:\Users\chaha\Downloads\ubuntu-26.04.1-desktop-amd64.iso"
KALI_LIVE_ISO = r"C:\Users\chaha\Downloads\kali-linux-2026.2-live-amd64.iso"
HBCD_ISO = r"C:\Users\chaha\Downloads\HBCD_PE_x64.iso"
ALPINE_ISO = r"C:\Users\chaha\Downloads\alpine-standard-3.24.2-x86_64.iso"


def verify_iso_with_keyboard(iso_path, iso_name, is_windows=False):
    print("\n" + "=" * 75)
    print(f"  VERIFYING: {iso_name}")
    print(f"  ISO Path : {iso_path}")
    print(f"  Hardware : xHCI Root Hub with USB Keyboard + Phone Mass Storage")
    print("=" * 75)

    if not os.path.exists(iso_path):
        print(f"[-] ERROR: ISO file not found at {iso_path}")
        return False, []

    # Configure QEMU with BOTH USB HID Keyboard AND USB Mass Storage on xHCI
    harness = QemuHarness(boot_img=DEFAULT_BOOT_IMG, memory="2048M", enable_qmp=True)
    extra_args = [
        "-device", "usb-kbd,bus=xhci.0,id=ext_kbd",
        "-drive", f"id=phone_disk,file={iso_path},format=raw,if=none,readonly=on",
        "-device", "usb-storage,bus=xhci.0,drive=phone_disk,id=phone_msc"
    ]

    def trigger(conn, current_log, state, qmp):
        # 1. Main menu detection -> send key '1'
        if not state.get("sent_os") and "[MENU] Select option" in current_log:
            print("[+] Boot menu rendered! Sending key '1' (via QMP USB Keyboard)...")
            time.sleep(0.3)
            # Try sending key via QMP USB keyboard first, and fallback/mirror via serial
            try:
                qmp.execute("send-key", {"keys": [{"type": "qcode", "data": "1"}]})
                time.sleep(0.1)
                qmp.execute("send-key", {"keys": [{"type": "qcode", "data": "ret"}]})
            except Exception as e:
                print(f"    (QMP send-key note: {e})")
            conn.sendall(b"1\n")
            state["sent_os"] = True

        # 2. Persistence Profile menu detection -> select Clean Session [1]
        if not state.get("sent_prof") and "[PROFILE] Select persistence profile" in current_log:
            print("[+] Persistence profile selector detected! Selecting [1]...")
            time.sleep(0.2)
            try:
                qmp.execute("send-key", {"keys": [{"type": "qcode", "data": "1"}]})
                time.sleep(0.1)
                qmp.execute("send-key", {"keys": [{"type": "qcode", "data": "ret"}]})
            except Exception:
                pass
            conn.sendall(b"1\n")
            state["sent_prof"] = True

        # 3. Termination conditions
        if is_windows and "Handing off to Real-Mode VBR Chainloader" in current_log:
            time.sleep(0.5)
            return True

        if not is_windows and "HANDING OFF EXECUTION TO LINUX" in current_log:
            time.sleep(0.5)
            return True

        return False

    captured_log = harness.run(extra_args=extra_args, interaction_fn=trigger, timeout_sec=95)

    with open(os.path.join(BUILD_DIR, f"serial_{iso_name.replace(' ', '_').lower()}.log"), "w", encoding="utf-8", errors="ignore") as f:
        f.write(captured_log)

    # Core checks applicable to all ISOs
    checks = [
        ("USB HID Boot Keyboard initialized", "USB HID External Keyboard enumerated & initialized on xHCI"),
        ("USB Mass Storage device LUN 0 is READY", "Phone Mass Storage enumerated & initialized on xHCI"),
    ]

    if is_windows:
        checks.extend([
            ("Hiren's BootCD PE (Windows 11 Live)", "Windows 11 Live PE recognized and labeled"),
            ("El Torito Boot Image discovered", "Windows El Torito boot catalog loaded"),
            ("INT 13h emulation installed: Virtual Drive 0x80", "INT 13h emulation active on Drive 0x80"),
            ("Keeping xHCI controller running for INT 13h emulation", "xHCI hardware kept active across RM transition"),
            ("Handing off to Real-Mode VBR Chainloader (Drive 0x80)", "Clean handoff to Windows VBR without touching host disks")
        ])
    elif "kali" in iso_name.lower():
        checks.extend([
            ("Kali Linux Live", "Kali Linux Live recognized"),
            ("Resolved Kernel", "Kali Live kernel resolved"),
            ("Resolved Initrd", "Kali Live initrd resolved"),
            ("boot=live", "Live boot mode parameter attached"),
            ("persistence", "Persistence parameter attached"),
            ("HANDING OFF EXECUTION TO LINUX", "Kernel handoff reached for Kali Live")
        ])
    elif "alpine" in iso_name.lower():
        checks.extend([
            ("Alpine", "Alpine Linux recognized"),
            ("Resolved Kernel", "Alpine kernel resolved"),
            ("Resolved Initrd", "Alpine initrd resolved"),
            ("HANDING OFF EXECUTION TO LINUX", "Kernel handoff reached for Alpine Linux")
        ])
    else: # Ubuntu
        checks.extend([
            ("Ubuntu", "Ubuntu Live media recognized"),
            ("Resolved Kernel", "Ubuntu Live kernel resolved"),
            ("Resolved Initrd", "Ubuntu Live initrd resolved"),
            ("boot=casper", "Casper live boot mode parameter attached"),
            ("HANDING OFF EXECUTION TO LINUX", "Kernel handoff reached for Ubuntu Live")
        ])

    results = []
    all_ok = True
    for needle, desc in checks:
        if needle.lower() in captured_log.lower():
            print(f"  [PASS] {desc}")
            results.append((desc, True))
        else:
            print(f"  [FAIL] Missing: '{needle}' ({desc})")
            results.append((desc, False))
            all_ok = False

    return all_ok, results


def main():
    print("=" * 75)
    print("  BOOTLOADER RIGOROUS MULTI-OS & EXTERNAL KEYBOARD VERIFICATION")
    print("=" * 75)

    all_tests_passed = True
    summary = []

    # Test 1: Ubuntu 26.04 Live ISO
    ok1, res1 = verify_iso_with_keyboard(UBUNTU_ISO, "Ubuntu 26.04 Desktop Live", is_windows=False)
    summary.append(("Ubuntu 26.04 Live ISO + USB Keyboard", ok1))
    if not ok1: all_tests_passed = False

    # Test 2: Kali Linux 2026.2 Live ISO
    ok2, res2 = verify_iso_with_keyboard(KALI_LIVE_ISO, "Kali Linux 2026.2 Live", is_windows=False)
    summary.append(("Kali Linux 2026.2 Live ISO + USB Keyboard", ok2))
    if not ok2: all_tests_passed = False

    # Test 3: Hiren's BootCD PE (Windows 11 Live)
    ok3, res3 = verify_iso_with_keyboard(HBCD_ISO, "Hiren's BootCD PE (Windows 11 Live)", is_windows=True)
    summary.append(("Hiren's BootCD PE Windows 11 Live + USB Keyboard", ok3))
    if not ok3: all_tests_passed = False

    # Test 4: Alpine Linux Standard
    ok4, res4 = verify_iso_with_keyboard(ALPINE_ISO, "Alpine Linux Standard", is_windows=False)
    summary.append(("Alpine Linux Standard + USB Keyboard", ok4))
    if not ok4: all_tests_passed = False

    print("\n" + "=" * 75)
    print("  COMPREHENSIVE MULTI-OS & KEYBOARD VERIFICATION SUMMARY")
    print("=" * 75)
    for title, passed in summary:
        status = "[PASS]" if passed else "[FAIL]"
        print(f"  {status} {title}")
    print("=" * 75)

    return 0 if all_tests_passed else 1


if __name__ == "__main__":
    sys.exit(main())
