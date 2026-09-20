#!/usr/bin/env python3
"""
tools/qemu_test.py - Automated Headless QEMU Verification for Bootloader
Launches QEMU, captures COM1 serial UART output, and asserts boot phase milestones.
"""

import os
import sys
import time
import subprocess

WORKSPACE_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(WORKSPACE_ROOT, "build")
BOOT_IMG = os.path.join(BUILD_DIR, "boot.img")
SERIAL_LOG = os.path.join(BUILD_DIR, "serial.log")

REQUIRED_LOG_PATTERNS = [
    "ANDROID -> LINUX BOOTLOADER (LEGACY BIOS)",
    "Stage 3 C Runtime Active at 0x00100000 (32-bit Flat Protected Mode)",
    "[STAGE3] Boot Drive preserved: 0x80 (Hard Disk / SD)",
    "[MEMORY] E820 System Memory Map",
    "[MEMORY] Total Usable RAM:",
    "[PCI] Scanning PCI bus for host controllers...",
    "[PCI] USB Controller:",
    "xHCI (USB 3.0+)",
    "[PCI] Primary xHCI Host Controller selected",
    "[STAGE3] Initializing xHCI Host Controller hardware...",
    "[XHCI] Initializing xHCI Controller at MMIO Base",
    "[XHCI] Controller Reset OK, Hardware Ready.",
    "[XHCI] Command & Event Rings Initialized.",
    "[XHCI] Controller Running (USBCMD.RS=1, USBSTS.HCH=0).",
    "[XHCI] Root Hub Ports Powered",
    "[XHCI] NO-OP Command Test: SUCCESS",
    "[STAGE3] Phase 3 xHCI Controller Initialization Successfully Verified!",
    "[STAGE3] Probing USB device on Port",
    "[USB] Device Descriptor: VID=",
    "[USB] Device Configuration 1 Activated.",
    "[STAGE3] Phase 4 USB Enumeration Successfully Verified!",
    "[STAGE3] Phase 1 Legacy BIOS Bootstrap Successfully Verified!",
    "Boot Menu Selection:",
    "[1] Boot Linux from Android Phone (MTP /Download/bzImage)",
    "[2] Boot Linux from SD Card (FAT32 Partition)",
    "[3] Hardware Diagnostics & PCI / USB / Memory Inspection",
    "[4] Linux 32-bit Boot Protocol Self-Test & Handoff Simulation",
    "[TEST] Phase 8 Image Detection: SUCCESS",
    "[TEST] Phase 9 Kernel Header Check: SUCCESS",
    "[TEST] Phase 9 Boot Params Setup: SUCCESS",
    "Phase 8 Image Detection Successfully Verified!",
    "Phase 9 Linux 32-bit Boot Protocol Successfully Verified!",
    "Phase 10 Interactive Boot Menu Successfully Verified!"
]

def main():
    if not os.path.exists(BOOT_IMG):
        sys.exit(f"[-] Boot image {BOOT_IMG} not found! Run build first.")

    if os.path.exists(SERIAL_LOG):
        os.remove(SERIAL_LOG)

    qemu_cmd = [
        "qemu-system-x86_64",
        "-snapshot",
        "-drive", f"file={BOOT_IMG},format=raw,if=ide",
        "-device", "qemu-xhci,id=xhci",
        "-device", "usb-tablet,bus=xhci.0",
        "-serial", f"file:{SERIAL_LOG}",
        "-display", "none",
        "-m", "512M"
    ]

    print(f"[*] Launching QEMU headless verification: {' '.join(qemu_cmd)}")
    proc = subprocess.Popen(qemu_cmd, cwd=WORKSPACE_ROOT)

    # Allow bootloader to execute through all stages (15s MTP wait + menu)
    timeout_sec = 25
    start_time = time.time()
    success = False
    captured_log = ""

    try:
        while time.time() - start_time < timeout_sec:
            time.sleep(0.5)
            if os.path.exists(SERIAL_LOG):
                with open(SERIAL_LOG, "r", errors="ignore") as f:
                    captured_log = f.read()
                if "Phase 10 Interactive Boot Menu Successfully Verified!" in captured_log:
                    success = True
                    break
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()

    print("\n--- CAPTURED SERIAL LOG ---")
    print(captured_log.strip())
    print("---------------------------\n")

    # Verify patterns
    all_passed = True
    for pattern in REQUIRED_LOG_PATTERNS:
        if pattern in captured_log:
            print(f"[PASS] Found: '{pattern}'")
        else:
            print(f"[FAIL] Missing expected pattern: '{pattern}'")
            all_passed = False

    if all_passed and success:
        print("\n[+] ========================================================")
        print("[+] ALL AUTOMATED QEMU VERIFICATION ASSERTIONS PASSED!")
        print("[+] Phases 1, 3, 4, 8, 9, 10 VERIFIED IN QEMU!")
        print("[+] ========================================================\n")
        sys.exit(0)
    else:
        print("\n[-] Automated QEMU test verification FAILED!")
        sys.exit(1)

if __name__ == "__main__":
    main()
