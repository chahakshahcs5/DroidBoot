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

def build(target="all"):
    if target == "prod":
        target = "release"
    os.makedirs(BUILD_DIR, exist_ok=True)
    
    stage1_asm = os.path.join(WORKSPACE_ROOT, "src", "stage1", "stage1.asm")
    stage2_asm = os.path.join(WORKSPACE_ROOT, "src", "stage2", "stage2.asm")
    stage1_bin = os.path.join(BUILD_DIR, "stage1.bin")
    stage2_bin = os.path.join(BUILD_DIR, "stage2.bin")
    mkimage_py = os.path.join(WORKSPACE_ROOT, "tools", "mkimage.py")

    # 1. Assemble Stage 1 and Stage 2 with native Windows NASM (or host nasm)
    nasm_bin = "nasm"
    print(f"[*] Assembling Stage 1 with {nasm_bin}...")
    run_cmd(f'{nasm_bin} -f bin "{stage1_asm}" -o "{stage1_bin}"')
    
    print(f"[*] Assembling Stage 2 with {nasm_bin}...")
    run_cmd(f'{nasm_bin} -f bin "{stage2_asm}" -o "{stage2_bin}"')

    # 2. Compile Stage 3 C runtime (Release and/or Debug) via WSL GCC/LD toolchain
    make_targets = []
    if target in ("all", "release"):
        make_targets.append("build/stage3.bin")
    if target in ("all", "debug"):
        make_targets.append("build/stage3-debug.bin")

    make_cmd_str = " ".join(make_targets)
    if sys.platform == "win32":
        print(f"[*] Compiling Stage 3 ({make_cmd_str}) via WSL GCC/LD toolchain...")
        wsl_path = WORKSPACE_ROOT.replace("\\", "/").replace("C:", "/mnt/c").replace("c:", "/mnt/c")
        run_cmd(f'wsl bash -l -c "cd {wsl_path} && make {make_cmd_str}"')
    else:
        run_cmd(f"make {make_cmd_str}")

    # 3. Generate boot images using tools/mkimage.py with host Python
    if target in ("all", "release"):
        stage3_bin = os.path.join(BUILD_DIR, "stage3.bin")
        boot_img = os.path.join(BUILD_DIR, "boot.img")
        print("[*] Generating Release boot image (build/boot.img)...")
        run_cmd(f'{sys.executable} "{mkimage_py}" --stage1 "{stage1_bin}" --stage2 "{stage2_bin}" --stage3 "{stage3_bin}" --output "{boot_img}" --size-mb 64')
        print(f"[+] Release build ready: {boot_img}")

    if target in ("all", "debug"):
        stage3_debug_bin = os.path.join(BUILD_DIR, "stage3-debug.bin")
        boot_debug_img = os.path.join(BUILD_DIR, "boot-debug.img")
        print("[*] Generating Debug boot image (build/boot-debug.img)...")
        run_cmd(f'{sys.executable} "{mkimage_py}" --stage1 "{stage1_bin}" --stage2 "{stage2_bin}" --stage3 "{stage3_debug_bin}" --output "{boot_debug_img}" --size-mb 64')
        print(f"[+] Debug build ready: {boot_debug_img}")

def run_qemu(xhci=False, headless=False, debug=False):
    target_img = os.path.join(BUILD_DIR, "boot-debug.img" if debug else "boot.img")
    if not os.path.exists(target_img):
        build("debug" if debug else "release")

    qemu_bin = "qemu-system-x86_64"
    args = [
        qemu_bin,
        "-snapshot",
        "-drive", f"file={target_img},format=raw,if=ide",
        "-serial", "stdio",
        "-m", "512M"
    ]
    if xhci:
        args.extend(["-device", "qemu-xhci,id=xhci"])
    if headless:
        args.extend(["-display", "none"])

    print(f"[*] Launching QEMU ({os.path.basename(target_img)}): {' '.join(args)}")
    subprocess.run(args, cwd=WORKSPACE_ROOT)

def main():
    parser = argparse.ArgumentParser(description="Bootloader build & test manager")
    parser.add_argument("--clean", action="store_true", help="Clean build directory")
    parser.add_argument("--target", choices=["all", "release", "debug", "prod"], default="all", help="Build target (default: all; 'prod' is an alias for 'release')")
    parser.add_argument("--run", action="store_true", help="Build and run in QEMU")
    parser.add_argument("--run-xhci", action="store_true", help="Build and run in QEMU with xHCI controller enabled")
    parser.add_argument("--debug", action="store_true", help="Run debug boot image in QEMU")
    parser.add_argument("--headless", action="store_true", help="Run QEMU headlessly without graphical window")
    args = parser.parse_args()

    if args.clean:
        if os.path.exists(BUILD_DIR):
            import shutil
            shutil.rmtree(BUILD_DIR)
            print(f"[+] Cleaned {BUILD_DIR}")
        return

    build(target=args.target)

    if args.run:
        run_qemu(xhci=False, headless=args.headless, debug=args.debug)
    elif args.run_xhci:
        run_qemu(xhci=True, headless=args.headless, debug=args.debug)

if __name__ == "__main__":
    main()

