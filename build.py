#!/usr/bin/env python3
"""
build.py - Unified cross-platform build orchestrator for Android -> Linux Bootloader
Works seamlessly on Windows (leveraging WSL for GCC/LD and native Windows for QEMU/NASM)
and Linux/WSL directly.
"""

import sys
import os
import subprocess
import argparse

WORKSPACE_ROOT = os.path.dirname(os.path.abspath(__file__))
BUILD_DIR = os.path.join(WORKSPACE_ROOT, "build")
TOOLS_DIR = os.path.join(WORKSPACE_ROOT, "tools")

def run_cmd(cmd, shell=True, check=True):
    print(f"[*] Executing: {cmd}")
    res = subprocess.run(cmd, shell=shell, cwd=WORKSPACE_ROOT)
    if check and res.returncode != 0:
        sys.exit(f"[-] Command failed with exit code {res.returncode}: {cmd}")
    return res.returncode

def build():
    os.makedirs(BUILD_DIR, exist_ok=True)
    
    stage1_asm = os.path.join(WORKSPACE_ROOT, "src", "stage1", "stage1.asm")
    stage2_asm = os.path.join(WORKSPACE_ROOT, "src", "stage2", "stage2.asm")
    stage1_bin = os.path.join(BUILD_DIR, "stage1.bin")
    stage2_bin = os.path.join(BUILD_DIR, "stage2.bin")
    stage3_bin = os.path.join(BUILD_DIR, "stage3.bin")
    boot_img = os.path.join(BUILD_DIR, "boot.img")

    # 1. Assemble Stage 1 and Stage 2 with native Windows NASM (or host nasm)
    nasm_bin = "nasm"
    print(f"[*] Assembling Stage 1 with {nasm_bin}...")
    run_cmd(f'{nasm_bin} -f bin "{stage1_asm}" -o "{stage1_bin}"')
    
    print(f"[*] Assembling Stage 2 with {nasm_bin}...")
    run_cmd(f'{nasm_bin} -f bin "{stage2_asm}" -o "{stage2_bin}"')

    # 2. Compile and Link Stage 3 C runtime using GCC/LD in WSL
    if sys.platform == "win32":
        print("[*] Compiling Stage 3 C runtime via WSL GCC/LD toolchain...")
        wsl_path = WORKSPACE_ROOT.replace("\\", "/").replace("C:", "/mnt/c").replace("c:", "/mnt/c")
        run_cmd(f'wsl bash -l -c "cd {wsl_path} && make build/stage3.bin"')
    else:
        run_cmd("make build/stage3.bin")

    # 3. Generate boot.img using tools/mkimage.py with host Python
    mkimage_py = os.path.join(WORKSPACE_ROOT, "tools", "mkimage.py")
    print("[*] Generating boot image with tools/mkimage.py...")
    run_cmd(f'{sys.executable} "{mkimage_py}" --stage1 "{stage1_bin}" --stage2 "{stage2_bin}" --stage3 "{stage3_bin}" --output "{boot_img}" --size-mb 64')

    print(f"[+] Build successful! Output: {boot_img}")

def run_qemu(xhci=False, headless=False):
    boot_img = os.path.join(BUILD_DIR, "boot.img")
    if not os.path.exists(boot_img):
        build()

    qemu_bin = "qemu-system-x86_64"
    args = [
        qemu_bin,
        "-snapshot",
        "-drive", f"file={boot_img},format=raw,if=ide",
        "-serial", "stdio",
        "-m", "512M"
    ]
    if xhci:
        args.extend(["-device", "qemu-xhci,id=xhci"])
    if headless:
        args.extend(["-display", "none"])

    print(f"[*] Launching QEMU: {' '.join(args)}")
    subprocess.run(args, cwd=WORKSPACE_ROOT)

def main():
    parser = argparse.ArgumentParser(description="Bootloader build & test manager")
    parser.add_argument("--clean", action="store_true", help="Clean build directory")
    parser.add_argument("--run", action="store_true", help="Build and run in QEMU")
    parser.add_argument("--run-xhci", action="store_true", help="Build and run in QEMU with xHCI controller enabled")
    parser.add_argument("--headless", action="store_true", help="Run QEMU headlessly without graphical window")
    args = parser.parse_args()

    if args.clean:
        if os.path.exists(BUILD_DIR):
            import shutil
            shutil.rmtree(BUILD_DIR)
            print(f"[+] Cleaned {BUILD_DIR}")
        return

    build()

    if args.run:
        run_qemu(xhci=False, headless=args.headless)
    elif args.run_xhci:
        run_qemu(xhci=True, headless=args.headless)

if __name__ == "__main__":
    main()
