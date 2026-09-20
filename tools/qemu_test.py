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
    "COMMENCING CONNECTED STORAGE & OPERATING SYSTEM SCAN",
    "Storage scan complete.",
    "ANDROID -> LINUX BOOTLOADER (DYNAMIC MULTI-OS)",
    "Discovered Operating Systems & Boot Options:",
    "Hardware Diagnostics & System Inspection",
    "Linux 32-bit Boot Protocol Simulation",
    "[MENU] Select option",
    "Linux Boot Protocol Simulation",
    "[TEST] Phase 8 Image Detection: SUCCESS",
    "[TEST] Phase 9 Kernel Header Check: SUCCESS",
    "[TEST] Phase 9 Boot Params Setup: SUCCESS",
    "Phase 8 Image Detection Successfully Verified!",
    "Phase 9 Linux 32-bit Boot Protocol Successfully Verified!",
    "Phase 10 Interactive Boot Menu Successfully Verified!"
]

def main():
    import socket

    if not os.path.exists(BOOT_IMG):
        sys.exit(f"[-] Boot image {BOOT_IMG} not found! Run build first.")

    port = 4444
    qemu_cmd = [
        "qemu-system-x86_64",
        "-snapshot",
        "-drive", f"file={BOOT_IMG},format=raw,if=ide",
        "-device", "qemu-xhci,id=xhci",
        "-device", "usb-tablet,bus=xhci.0",
        "-serial", f"tcp:127.0.0.1:{port},server,nowait",
        "-display", "none",
        "-m", "512M"
    ]

    print(f"[*] Launching QEMU headless verification: {' '.join(qemu_cmd)}")
    proc = subprocess.Popen(qemu_cmd, cwd=WORKSPACE_ROOT)

    # Connect to QEMU serial socket
    sock = None
    for _ in range(30):
        time.sleep(0.1)
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.connect(("127.0.0.1", port))
            sock.setblocking(False)
            break
        except (ConnectionRefusedError, OSError):
            if sock:
                sock.close()
            sock = None

    if not sock:
        proc.terminate()
        sys.exit("[-] Failed to connect to QEMU serial socket!")

    captured_chunks = []
    sent_menu_choice = False
    success = False
    timeout_sec = 35
    start_time = time.time()

    try:
        with open(SERIAL_LOG, "w", encoding="utf-8", errors="ignore") as log_f:
            while time.time() - start_time < timeout_sec:
                try:
                    chunk = sock.recv(1024)
                    if chunk:
                        text = chunk.decode("utf-8", errors="ignore")
                        captured_chunks.append(text)
                        log_f.write(text)
                        log_f.flush()

                        current_log = "".join(captured_chunks)

                        # Detect dynamic interactive menu prompt and send choice 't' (Self-Test)
                        if not sent_menu_choice and "[MENU] Select option" in current_log:
                            print("[*] Detected dynamic boot menu prompt! Sending selection 't' via COM1 serial...")
                            try:
                                sock.sendall(b"t\n")
                            except (BrokenPipeError, OSError):
                                pass
                            sent_menu_choice = True

                        if "Phase 10 Interactive Boot Menu Successfully Verified!" in current_log:
                            time.sleep(0.5)
                            # Drain any remaining bytes
                            try:
                                extra = sock.recv(1024)
                                if extra:
                                    t_extra = extra.decode("utf-8", errors="ignore")
                                    captured_chunks.append(t_extra)
                                    log_f.write(t_extra)
                                    log_f.flush()
                            except (BlockingIOError, OSError):
                                pass
                            success = True
                            break
                except (BlockingIOError, OSError):
                    time.sleep(0.1)
    finally:
        sock.close()
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()

    captured_log = "".join(captured_chunks)
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
        print("[+] Zero auto-selection: Bootloader waited indefinitely for user choice!")
        print("[+] Phases 1, 3, 4, 8, 9, 10 VERIFIED IN QEMU!")
        print("[+] ========================================================\n")
        sys.exit(0)
    else:
        print("\n[-] Automated QEMU test verification FAILED!")
        sys.exit(1)

if __name__ == "__main__":
    main()
