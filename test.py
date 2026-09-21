#!/usr/bin/env python3
"""
test.py - Unified Master Test Orchestrator for BootManager
Enables 95% pure software emulation in QEMU without physical phone or Windows driver issues.

Usage:
  python test.py                     # Run complete automated QEMU regression suite
  python test.py --suite baseline    # Test Stage 1-3 bootstrap, E820, PCI, xHCI, TUI menu
  python test.py --suite persistence # Test persistence sub-menu & clean session
  python test.py --suite sizing      # Test custom overlay sizing (2GB, 4GB, 8GB, 16GB)
  python test.py --suite ram         # Test in-RAM ISO streaming & phram parameter handoff
  python test.py --suite gadget      # Test dynamic phone mode-switching (MTP -> UMS via QMP)
  python test.py --debug             # Run tests against build/boot-debug.img
  python test.py --interactive       # Launch QEMU with graphical window & serial stdio
"""

import os
import sys
import argparse
import subprocess

WORKSPACE_ROOT = os.path.dirname(os.path.abspath(__file__))
BUILD_DIR = os.path.join(WORKSPACE_ROOT, "build")
BOOT_IMG_RELEASE = os.path.join(BUILD_DIR, "boot.img")
BOOT_IMG_DEBUG = os.path.join(BUILD_DIR, "boot-debug.img")

# Ensure tools directory is in Python module search path
sys.path.insert(0, WORKSPACE_ROOT)
from tools.qemu_test import (
    test_baseline_self_test,
    test_multiprofile_persistence,
    test_custom_capacity_selection,
    test_in_ram_iso_boot,
    test_dynamic_gadget_switch,
    test_sd_card_persistence,
    run_all_tests
)


def ensure_built(target_img):
    if not os.path.exists(target_img):
        print(f"[*] Boot image not found at {target_img}. Building...")
        target_name = "debug" if "debug" in target_img else "release"
        cmd = [sys.executable, os.path.join(WORKSPACE_ROOT, "build.py"), "--target", target_name]
        res = subprocess.run(cmd, cwd=WORKSPACE_ROOT)
        if res.returncode != 0:
            sys.exit(f"[-] Build failed with exit code {res.returncode}")


def run_interactive(target_img):
    ensure_built(target_img)
    qemu_cmd = [
        "qemu-system-x86_64",
        "-snapshot",
        "-drive", f"file={target_img},format=raw,if=ide",
        "-device", "qemu-xhci,id=xhci",
        "-serial", "stdio",
        "-m", "1024M"
    ]
    print(f"[*] Launching Interactive QEMU: {' '.join(qemu_cmd)}")
    subprocess.run(qemu_cmd, cwd=WORKSPACE_ROOT)


def main():
    parser = argparse.ArgumentParser(description="BootManager Unified Test Orchestrator")
    parser.add_argument(
        "--suite",
        choices=["all", "baseline", "persistence", "sizing", "ram", "gadget", "sdlog"],
        default="all",
        help="Test suite to execute (default: all)"
    )
    parser.add_argument(
        "--debug",
        action="store_true",
        help="Run tests against build/boot-debug.img instead of build/boot.img"
    )
    parser.add_argument(
        "--interactive",
        action="store_true",
        help="Launch interactive graphical QEMU session with serial stdio"
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="List all available test suites"
    )

    args = parser.parse_args()

    if args.list:
        print()
        print("Available Test Suites:")
        print("  all          - Complete 6-phase automated regression suite")
        print("  baseline     - Phase 1-10 bootstrap, E820, PCI, xHCI, TUI menu, self-test")
        print("  persistence  - Persistence sub-menu and clean disposable session")
        print("  sizing       - Dynamic custom persistence overlay capacity sizing (2G-16G)")
        print("  ram          - In-RAM ISO streaming and phram parameter kernel handoff")
        print("  gadget       - Dynamic phone gadget mode-switching (MTP -> UMS via QMP)")
        print("  sdlog        - Verify live disk block writeback to SD card (FAT32 & raw sectors)")
        print()
        return

    target_img = BOOT_IMG_DEBUG if args.debug else BOOT_IMG_RELEASE
    ensure_built(target_img)

    if args.interactive:
        run_interactive(target_img)
        return

    print(f"[*] Testing Target Image: {os.path.basename(target_img)}")
    suite = args.suite

    if suite == "all":
        success = run_all_tests(boot_img=target_img)
    elif suite == "baseline":
        success = test_baseline_self_test(boot_img=target_img)
    elif suite == "persistence":
        success = test_multiprofile_persistence(boot_img=target_img)
    elif suite == "sizing":
        success = test_custom_capacity_selection(boot_img=target_img)
    elif suite == "ram":
        success = test_in_ram_iso_boot(boot_img=target_img)
    elif suite == "gadget":
        success = test_dynamic_gadget_switch(boot_img=target_img)
    elif suite == "sdlog":
        success = test_sd_card_persistence(boot_img=target_img)
    else:
        success = False

    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
