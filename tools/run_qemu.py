#!/usr/bin/env python3
"""
tools/run_qemu.py - Unified Desktop QEMU Test Runner for Bootloader
Tests both Pathway 1 (In-RAM Alpine Boot with SD Persistence)
and Pathway 2 (Rooted Phone / USB Mass Storage On-Demand Boot for Ubuntu 6GB)
in an interactive graphical desktop window without rebooting your laptop!
"""

import os
import sys
import argparse
import subprocess
import time

WORKSPACE_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(WORKSPACE_ROOT, "build")
BOOT_IMG = os.path.join(BUILD_DIR, "boot.img")
SERIAL_LOG = os.path.join(BUILD_DIR, "serial.log")

DEFAULT_UBUNTU_ISO = r"C:\Users\chaha\Downloads\ubuntu-26.04.1-desktop-amd64.iso"
DEFAULT_ALPINE_ISO = r"C:\Users\chaha\Downloads\alpine-standard-3.24.2-x86_64.iso"

def check_build():
    needs_build = not os.path.exists(BOOT_IMG)
    if not needs_build:
        img_mtime = os.path.getmtime(BOOT_IMG)
        for root, _, files in os.walk(os.path.join(WORKSPACE_ROOT, "src")):
            for f in files:
                if f.endswith((".c", ".h", ".asm", ".ld")):
                    p = os.path.join(root, f)
                    if os.path.getmtime(p) > img_mtime:
                        needs_build = True
                        break
            if needs_build:
                break

    if needs_build:
        print(f"[*] Source changes detected or boot image missing. Building now...")
        res = subprocess.run([sys.executable, os.path.join(WORKSPACE_ROOT, "build.py")], cwd=WORKSPACE_ROOT)
        if res.returncode != 0:
            sys.exit("[-] Build failed! Please resolve errors before running QEMU.")

def build_qemu_command(mode, iso_path, memory, headless, usb_host):
    check_build()

    if os.path.exists(SERIAL_LOG):
        try:
            os.remove(SERIAL_LOG)
        except OSError:
            pass

    cmd = [
        "qemu-system-x86_64",
        "-drive", f"file={BOOT_IMG},format=raw,if=ide",
        "-device", "qemu-xhci,id=xhci",
        "-m", memory,
        "-serial", "stdio"
    ]

    if headless:
        cmd.extend(["-display", "none"])

    if usb_host:
        # Pass-through physical USB device (e.g. your physical Android phone)
        # Supports:
        #   1. VID only (e.g. 2717 or 18d1) - preserves connection across dynamic PID changes!
        #   2. VID:PID (e.g. 18d1:4ee2)
        #   3. hostbus:hostport (e.g. 1:2)
        if ":" in usb_host:
            parts = usb_host.split(":")
            if len(parts) == 2:
                vid, pid = parts[0], parts[1]
                cmd.extend(["-device", f"usb-host,bus=xhci.0,vendorid=0x{vid},productid=0x{pid}"])
                print(f"[+] Attached physical USB device (VID:0x{vid} PID:0x{pid}) to xHCI pass-through.")
        elif "." in usb_host:
            parts = usb_host.split(".")
            cmd.extend(["-device", f"usb-host,bus=xhci.0,hostbus={parts[0]},hostport={parts[1]}"])
            print(f"[+] Attached physical USB port (Bus {parts[0]} Port {parts[1]}) to xHCI pass-through.")
        else:
            # Vendor ID only: vital for dynamic gadget switching where PID changes!
            cmd.extend(["-device", f"usb-host,bus=xhci.0,vendorid=0x{usb_host}"])
            print(f"[+] Attached physical USB vendor (VID:0x{usb_host}) to xHCI pass-through (survives dynamic PID switch).")

    elif mode in ["block", "ubuntu"]:
        # Pathway 2: Rooted Phone / USB Mass Storage emulation (Ubuntu 6GB)
        if not os.path.exists(iso_path):
            print(f"[-] Ubuntu ISO not found at {iso_path}!")
            print("    Please provide a valid ISO with --iso <path>.")
            sys.exit(1)

        print(f"[+] Mimicking Rooted Android Phone (USB Mass Storage) with: {iso_path}")
        print("    * Bootloader will read ONLY vmlinuz (17MB) and initrd (95MB) in ~1-2 sec.")
        print("    * Linux kernel will mount the 6GB OS directly from the block device.")
        print("    * RAM consumed by OS image: 0 MB!")
        cmd.extend([
            "-drive", f"id=phone_disk,file={iso_path},format=raw,if=none,readonly=on",
            "-device", "usb-storage,bus=xhci.0,drive=phone_disk"
        ])

    elif mode in ["alpine-block"]:
        # Pathway 2 with Alpine ISO on USB block storage
        if not os.path.exists(iso_path):
            print(f"[-] Alpine ISO not found at {iso_path}!")
            sys.exit(1)

        print(f"[+] Mimicking Rooted Android Phone (USB Mass Storage) with Alpine: {iso_path}")
        cmd.extend([
            "-drive", f"id=phone_disk,file={iso_path},format=raw,if=none,readonly=on",
            "-device", "usb-storage,bus=xhci.0,drive=phone_disk"
        ])

    elif mode in ["dual"]:
        # Dual mode: Attach Ubuntu as USB Mass Storage block device AND preload Alpine into RAM
        if not os.path.exists(DEFAULT_UBUNTU_ISO):
            print(f"[-] Ubuntu ISO not found at {DEFAULT_UBUNTU_ISO}!")
            sys.exit(1)
        if not os.path.exists(DEFAULT_ALPINE_ISO):
            print(f"[-] Alpine ISO not found at {DEFAULT_ALPINE_ISO}!")
            sys.exit(1)

        print("[+] Mimicking Dual Connected Storages:")
        print(f"    1. USB Mass Storage (Rooted Phone): {DEFAULT_UBUNTU_ISO} (Approach 1: Direct Block)")
        print(f"    2. In-RAM Simulation (MTP Phone)   : {DEFAULT_ALPINE_ISO} (Approach 2: In-RAM + SD Persistence)")
        cmd.extend([
            "-drive", f"id=phone_disk,file={DEFAULT_UBUNTU_ISO},format=raw,if=none,readonly=on",
            "-device", "usb-storage,bus=xhci.0,drive=phone_disk",
            "-device", "loader,file=" + DEFAULT_ALPINE_ISO + ",addr=0x10000000,force-raw=on"
        ])

    elif mode in ["ram", "mtp"]:
        # Pathway 1: In-RAM Boot + SD Card Persistence (Alpine Linux ~370MB)
        if not os.path.exists(iso_path):
            print(f"[-] ISO not found at {iso_path}!")
            sys.exit(1)

        iso_size_mb = os.path.getsize(iso_path) // (1024 * 1024)
        print(f"[+] Preloading ISO into RAM simulation (256MB boundary, 0x10000000): {iso_path} ({iso_size_mb} MB)")
        print("    * Bootloader configures phram=iso,0x10000000,<len> and mBFT at 0x000E0000.")
        print("    * E820 memory table reserves ISO region (Type 2 RESERVED).")
        print("    * Alpine Linux creates /dev/mtdblock0 in RAM.")
        print("    * Persistence writes to SD Card FAT32 partition via apkovl=sda1:.")

        # In QEMU, preloading an image directly to physical memory at 0x10000000
        # Also attach as read-only cdrom so Alpine's nlplug-findfs can mount modloop after handoff
        cmd.extend([
            "-device", "loader,file=" + iso_path + ",addr=0x10000000,force-raw=on",
            "-drive", f"file={iso_path},media=cdrom,readonly=on"
        ])

    return cmd

def main():
    parser = argparse.ArgumentParser(description="Desktop QEMU Bootloader Runner")
    parser.add_argument("--mode", choices=["block", "ubuntu", "alpine-block", "ram", "mtp", "dual"], default="block",
                        help="Boot mode: 'block' (Rooted phone UMS), 'mtp' (In-RAM Alpine), or 'dual' (Both storages attached)")
    parser.add_argument("--iso", default="",
                        help="Path to ISO image (defaults to Downloads folder)")
    parser.add_argument("--memory", default="2048M",
                        help="RAM size for VM (default: 2048M)")
    parser.add_argument("--headless", action="store_true",
                        help="Run without graphical display window")
    parser.add_argument("--profile", choices=["work", "personal", "clean", ""], default="",
                        help="Target persistence profile to select at runtime (e.g. 'work', 'personal', 'clean')")
    parser.add_argument("--usb-host", default="",
                        help="Pass-through real physical USB phone (VID:PID, e.g. 2717:ff40)")
    parser.add_argument("--clean", action="store_true",
                        help="Wipe build artifacts and regenerate a clean, pristine boot.img before launching")

    args = parser.parse_args()

    if args.clean:
        print("[*] Cleaning build directory and resetting boot image...")
        for fname in ["serial.log", "serial_persist.log", "ubuntu_boot.log", "screen.ppm", "screen.png"]:
            fpath = os.path.join(BUILD_DIR, fname)
            if os.path.exists(fpath):
                try:
                    os.remove(fpath)
                except OSError:
                    pass
        if os.path.exists(BOOT_IMG):
            try:
                os.remove(BOOT_IMG)
            except OSError:
                pass
        print("[+] Removed stale logs and boot image. Fresh image will be generated now.")

    # Determine default ISO based on mode
    iso_path = args.iso
    if not iso_path:
        if args.mode in ["block", "ubuntu"]:
            iso_path = DEFAULT_UBUNTU_ISO
        else:
            iso_path = DEFAULT_ALPINE_ISO

    qemu_cmd = build_qemu_command(args.mode, iso_path, args.memory, args.headless, args.usb_host)

    print("\n" + "=" * 70)
    print("  LAUNCHING DESKTOP QEMU TEST ENVIRONMENT")
    print(f"  Mode       : {args.mode.upper()}")
    print(f"  Target ISO : {iso_path}")
    if args.profile:
        print(f"  Profile    : {args.profile.upper()} (Select corresponding profile number in sub-menu)")
    print(f"  Memory     : {args.memory}")
    print(f"  Display    : {'Headless' if args.headless else 'Interactive Graphical Window'}")
    print("=" * 70 + "\n")

    print("[*] Running command:")
    print(" ".join(qemu_cmd))
    print("\n[!] A graphical QEMU window will now open on your desktop.")
    print("    Press Ctrl+Alt+G to release mouse if captured, or close the window to exit.\n")

    try:
        proc = subprocess.Popen(qemu_cmd, cwd=WORKSPACE_ROOT)
        proc.wait()
    except KeyboardInterrupt:
        print("\n[*] Terminating QEMU...")
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
    except Exception as e:
        print(f"[-] Error launching QEMU: {e}")

if __name__ == "__main__":
    main()
