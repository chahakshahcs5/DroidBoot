#!/usr/bin/env python3
"""
tools/qemu_test.py - Modular Automated QEMU Verification Test Suite
Powered by tools/qemu_harness.py
"""

import os
import sys
import time
import re

from tools.qemu_harness import QemuHarness, WORKSPACE_ROOT, BUILD_DIR, DEFAULT_BOOT_IMG

DEFAULT_UBUNTU_ISO = r"C:\Users\chaha\Downloads\ubuntu-26.04.1-desktop-amd64.iso"
DEFAULT_KALI_ISO = r"C:\Users\chaha\Downloads\kali-linux-2026.2-installer-amd64.iso"
DEFAULT_ALPINE_ISO = r"C:\Users\chaha\Downloads\alpine-standard-3.24.2-x86_64.iso"
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


def test_baseline_self_test(boot_img=DEFAULT_BOOT_IMG):
    print("\n" + "=" * 70)
    print("  TEST 1: Baseline Boot Protocol & Milestone Verification")
    print("=" * 70)

    harness = QemuHarness(boot_img=boot_img, memory="1024M")
    extra_args = ["-device", "usb-tablet,bus=xhci.0"]

    def trigger(conn, current_log, state, qmp):
        if not state.get("sent_choice") and "[MENU] Select option" in current_log:
            print("[*] Detected dynamic boot menu prompt! Sending selection 't' via COM1 serial...")
            time.sleep(0.1)
            conn.sendall(b"t\n")
            state["sent_choice"] = True

        if "Phase 10 Interactive Boot Menu Successfully Verified!" in current_log:
            time.sleep(0.3)
            return True
        return False

    captured_log = harness.run(extra_args=extra_args, interaction_fn=trigger, timeout_sec=25)

    with open(SERIAL_LOG, "w", encoding="utf-8", errors="ignore") as f:
        f.write(captured_log)

    all_passed = True
    for pattern in REQUIRED_LOG_PATTERNS:
        if pattern in captured_log:
            print(f"[PASS] Found: '{pattern}'")
        else:
            print(f"[FAIL] Missing expected pattern: '{pattern}'")
            all_passed = False

    return all_passed


def test_multiprofile_persistence(boot_img=DEFAULT_BOOT_IMG):
    if not os.path.exists(DEFAULT_UBUNTU_ISO):
        print(f"[*] Skipping Multi-Profile test (ISO not present at {DEFAULT_UBUNTU_ISO})")
        return True

    print("\n" + "=" * 70)
    print("  TEST 2: Multi-Profile Persistence Sub-Menu & Clean Session Test")
    print("=" * 70)

    harness = QemuHarness(boot_img=boot_img, memory="1024M")
    extra_args = [
        "-drive", f"id=phone_disk,file={DEFAULT_UBUNTU_ISO},format=raw,if=none,readonly=on",
        "-device", "usb-storage,bus=xhci.0,drive=phone_disk"
    ]

    def trigger(conn, current_log, state, qmp):
        if not state.get("sent_os") and "[MENU] Select option" in current_log:
            print("[*] Main boot menu detected! Selecting OS [1] (Ubuntu)...")
            time.sleep(0.1)
            conn.sendall(b"1\n")
            state["sent_os"] = True

        if not state.get("sent_prof") and "[PROFILE] Select persistence profile" in current_log:
            m = re.search(r"\[(\d+)\]\s+Clean Disposable Session", current_log)
            clean_opt = m.group(1) if m else "1"
            print(f"[*] Persistence Profile Sub-Menu detected! Selecting [{clean_opt}] (Clean Disposable Session)...")
            time.sleep(0.1)
            conn.sendall(f"{clean_opt}\n".encode("ascii"))
            state["sent_prof"] = True

        if "HANDING OFF EXECUTION TO LINUX" in current_log:
            time.sleep(0.3)
            return True
        return False

    captured_log = harness.run(extra_args=extra_args, interaction_fn=trigger, timeout_sec=30)

    checks = [
        ("PERSISTENCE PROFILE SELECTOR", "Sub-menu banner rendered"),
        ("Clean Disposable Session", "Clean disposable session option available"),
        ("[+] Create New Custom Profile...", "Dynamic profile creation option displayed"),
        ("Clean Disposable Session", "Clean session configured for kernel"),
        ("HANDING OFF EXECUTION TO LINUX", "Kernel handoff reached with clean session")
    ]

    all_passed = True
    for needle, desc in checks:
        if needle in captured_log:
            print(f"[PASS] {desc}: '{needle}'")
        else:
            print(f"[FAIL] Missing {desc}: '{needle}'")
            all_passed = False

    return all_passed


def test_custom_capacity_selection(boot_img=DEFAULT_BOOT_IMG):
    if not os.path.exists(DEFAULT_UBUNTU_ISO):
        print(f"[*] Skipping Custom Capacity test (ISO not present at {DEFAULT_UBUNTU_ISO})")
        return True

    print("\n" + "=" * 70)
    print("  TEST 3: Dynamic Profile Creation & Capacity Sizing Test")
    print("=" * 70)

    harness = QemuHarness(boot_img=boot_img, memory="1024M")
    extra_args = [
        "-drive", f"id=phone_disk,file={DEFAULT_UBUNTU_ISO},format=raw,if=none,readonly=on",
        "-device", "usb-storage,bus=xhci.0,drive=phone_disk"
    ]

    def trigger(conn, current_log, state, qmp):
        if not state.get("sent_os") and "[MENU] Select option" in current_log:
            print("[*] Main boot menu detected! Selecting OS [1] (Ubuntu)...")
            time.sleep(0.1)
            conn.sendall(b"1\n")
            state["sent_os"] = True

        if not state.get("sent_create") and "[PROFILE] Select persistence profile" in current_log:
            m = re.search(r"\[(\d+)\]\s+\[\+\] Create New Custom Profile", current_log)
            create_opt = m.group(1) if m else "2"
            print(f"[*] Profile sub-menu detected! Selecting [{create_opt}] (Create New Custom Profile)...")
            time.sleep(0.1)
            conn.sendall(f"{create_opt}\n".encode("ascii"))
            state["sent_create"] = True

        if not state.get("sent_size") and "[SIZE] Select persistence capacity" in current_log:
            print("[*] Capacity Size Selector detected! Selecting [3] (8 GB Developer)...")
            time.sleep(0.1)
            conn.sendall(b"3\n")
            state["sent_size"] = True

        if not state.get("sent_name") and "[PROFILE] Enter custom profile name/label" in current_log:
            print("[*] Profile Label prompt detected! Sending label 'devwork'...")
            time.sleep(0.1)
            conn.sendall(b"devwork\n")
            state["sent_name"] = True

        if "HANDING OFF EXECUTION TO LINUX" in current_log:
            time.sleep(0.3)
            return True
        return False

    captured_log = harness.run(extra_args=extra_args, interaction_fn=trigger, timeout_sec=30)

    checks = [
        ("PERSISTENCE OVERLAY CAPACITY SELECTOR", "Capacity selector rendered"),
        ("2 GB  (Light", "2 GB option listed"),
        ("4 GB  (Standard", "4 GB option listed"),
        ("8 GB  (Developer", "8 GB option listed"),
        ("16 GB (Heavy Workstation", "16 GB option listed"),
        ("Allocated 8 GB developer overlay capacity", "8 GB capacity registered"),
        ("devwork (8192 MB)", "Custom profile attached"),
        ("devwork_8192MB.casper-rw", "ISO-scoped sparse overlay filename generated"),
        ("HANDING OFF EXECUTION TO LINUX", "Kernel handoff reached with 8 GB overlay")
    ]

    all_passed = True
    for needle, desc in checks:
        if needle in captured_log:
            print(f"[PASS] {desc}: '{needle}'")
        else:
            print(f"[FAIL] Missing {desc}: '{needle}'")
            all_passed = False

    return all_passed


def test_in_ram_iso_boot(boot_img=DEFAULT_BOOT_IMG):
    if not os.path.exists(DEFAULT_ALPINE_ISO):
        print(f"[*] Skipping In-RAM ISO test (ISO not present at {DEFAULT_ALPINE_ISO})")
        return True

    print("\n" + "=" * 70)
    print("  TEST 4: In-RAM ISO Detection & Persistence Handoff Test (Alpine)")
    print("=" * 70)

    harness = QemuHarness(boot_img=boot_img, memory="2048M")
    extra_args = [
        "-device", f"loader,file={DEFAULT_ALPINE_ISO},addr=0x10000000,force-raw=on"
    ]

    def trigger(conn, current_log, state, qmp):
        if not state.get("sent_os") and "[MENU] Select option" in current_log:
            print("[*] Main boot menu detected! Selecting OS [1] (Alpine Linux Standard)...")
            time.sleep(0.1)
            conn.sendall(b"1\n")
            state["sent_os"] = True

        if not state.get("sent_prof") and "[PROFILE] Select persistence profile" in current_log:
            m = re.search(r"\[(\d+)\]\s+\[\+\] Create New Custom Profile", current_log)
            create_opt = m.group(1) if m else "2"
            print(f"[*] Profile sub-menu detected! Selecting [{create_opt}] (Create New Custom Profile)...")
            time.sleep(0.1)
            conn.sendall(f"{create_opt}\n".encode("ascii"))
            state["sent_prof"] = True

        if not state.get("sent_size") and "[SIZE] Select persistence capacity" in current_log:
            print("[*] Capacity Size Selector detected! Selecting [1] (2 GB Light)...")
            time.sleep(0.1)
            conn.sendall(b"1\n")
            state["sent_size"] = True

        if not state.get("sent_name") and "[PROFILE] Enter custom profile name/label" in current_log:
            print("[*] Profile Label prompt detected! Sending Enter for default timestamp...")
            time.sleep(0.1)
            conn.sendall(b"\n")
            state["sent_name"] = True

        if "HANDING OFF TO IN-RAM LINUX WITH SD PERSISTENCE" in current_log:
            time.sleep(0.3)
            return True
        return False

    captured_log = harness.run(extra_args=extra_args, interaction_fn=trigger, timeout_sec=30)

    checks = [
        ("Preloaded In-RAM ISO detected at 0x10000000", "In-RAM ISO detected at 0x10000000"),
        ("Alpine Linux Standard", "Alpine Linux identified in ISO9660"),
        ("Phone MTP Streamed / In-RAM Cache", "RAM storage type recognized"),
        ("PERSISTENCE PROFILE SELECTOR: Alpine Linux Standard", "Profile menu triggered for In-RAM OS"),
        ("HANDING OFF TO IN-RAM LINUX WITH SD PERSISTENCE", "Kernel handoff reached for In-RAM Linux"),
        ("phram=iso,0x10000000", "phram kernel parameter attached"),
        ("memdisk=yes", "memdiskfind parameter attached"),
        ("Persistence", "Persistence profile attached to handoff")
    ]

    all_passed = True
    for needle, desc in checks:
        if needle in captured_log:
            print(f"[PASS] {desc}: '{needle}'")
        else:
            print(f"[FAIL] Missing {desc}: '{needle}'")
            all_passed = False

    return all_passed


def test_dynamic_gadget_switch(boot_img=DEFAULT_BOOT_IMG):
    if not os.path.exists(DEFAULT_ALPINE_ISO):
        print(f"[*] Skipping Dynamic Gadget Switch test (ISO not present at {DEFAULT_ALPINE_ISO})")
        return True

    print("\n" + "=" * 70)
    print("  TEST 5: Dynamic Phone Gadget Mode-Switching (MTP -> UMS via QMP)")
    print("=" * 70)

    harness = QemuHarness(boot_img=boot_img, memory="2048M", enable_qmp=True)
    extra_args = ["-device", "usb-tablet,bus=xhci.0,id=initial_phone"]

    def trigger(conn, current_log, state, qmp):
        # Step 1: Wait for initial boot menu (phone in MTP / non-storage mode)
        if not state.get("phone_switched") and "[MENU] Select option" in current_log:
            print("[+] Initial boot menu rendered (Phone in MTP mode).")
            print("[*] Simulating phone root switch: Unlinking MTP -> Enrolling USB Mass Storage gadget...")
            time.sleep(0.5)

            # Disconnect initial phone device
            qmp.execute("device_del", {"id": "initial_phone"})
            time.sleep(0.5)

            # Re-attach as USB Mass Storage
            qmp.execute("human-monitor-command", {
                "command-line": f"drive_add 0 file={DEFAULT_ALPINE_ISO},format=raw,if=none,id=phone_ums_drive,readonly=on"
            })
            qmp.execute("device_add", {
                "driver": "usb-storage",
                "bus": "xhci.0",
                "drive": "phone_ums_drive",
                "id": "phone_ums"
            })
            print("[+] Phone re-attached as USB Mass Storage device!")
            state["phone_switched"] = True

        # Step 2: Trigger bootloader rescan
        if state.get("phone_switched") and not state.get("sent_rescan"):
            print("[*] Triggering bootloader 'R' to rescan for newly attached Mass Storage...")
            time.sleep(0.5)
            conn.sendall(b"R\n")
            state["sent_rescan"] = True

        # Step 3: Select detected OS
        if state.get("sent_rescan") and not state.get("sent_os") and ("Alpine" in current_log or "Linux" in current_log):
            print("[+] Bootloader dynamically detected converted Phone as USB Block Storage! Selecting [1]...")
            time.sleep(0.5)
            conn.sendall(b"1\n")
            state["sent_os"] = True

        # Step 4: Profile selection
        if state.get("sent_os") and not state.get("sent_prof") and "[PROFILE] Select persistence profile" in current_log:
            print("[+] Profile menu appeared! Selecting [1] (Clean Session)...")
            time.sleep(0.1)
            conn.sendall(b"1\n")
            state["sent_prof"] = True

        if "HANDING OFF EXECUTION TO LINUX" in current_log:
            time.sleep(0.3)
            return True
        return False

    captured_log = harness.run(extra_args=extra_args, interaction_fn=trigger, timeout_sec=35)

    gadget_log_path = os.path.join(BUILD_DIR, "gadget_test.log")
    with open(gadget_log_path, "w", encoding="utf-8", errors="ignore") as f:
        f.write(captured_log)

    checks = [
        ("Rescanning", "Bootloader storage rescan triggered"),
        ("HANDING OFF EXECUTION TO LINUX", "Kernel handoff reached after dynamic switch")
    ]

    all_passed = True
    for needle, desc in checks:
        if needle in captured_log:
            print(f"[PASS] {desc}: '{needle}'")
        else:
            print(f"[FAIL] Missing {desc}: '{needle}'")
            all_passed = False

    return all_passed


def test_sd_card_persistence(boot_img=None):
    import shutil
    from tools.read_bootlog import list_and_extract_fat_logs, read_raw_log

    print("\n" + "=" * 70)
    print("  TEST 6: SD Card Live Logging Persistence (FAT32 & Raw Sectors)")
    print("=" * 70)

    debug_img = os.path.join(BUILD_DIR, "boot-debug.img")
    target_img = boot_img or debug_img
    if "debug" not in os.path.basename(target_img):
        print(f"[*] Note: '{os.path.basename(target_img)}' is Release build (disk logging disabled by design).")
        print(f"[*] Automatically testing SD Card Live Persistence against '{os.path.basename(debug_img)}'...")
        target_img = debug_img

    test_img = os.path.join(BUILD_DIR, "test_sd_persist.img")
    shutil.copyfile(target_img, test_img)

    harness = QemuHarness(boot_img=test_img, memory="1024M", snapshot=False)

    def trigger(conn, current_log, state, qmp):
        if "[MENU] Select option" in current_log:
            time.sleep(1.0)
            return True
        return False

    harness.run(extra_args=["-device", "usb-tablet,bus=xhci.0"], interaction_fn=trigger, timeout_sec=25)

    fat_info = list_and_extract_fat_logs(test_img)
    if not fat_info or not fat_info[0]:
        print("[-] FAILED: Could not parse FAT32 partition on SD card.")
        if os.path.exists(test_img): os.remove(test_img)
        return False

    files = {fname: sz for fname, cl, sz in fat_info[0]}
    fat_bootlog = list_and_extract_fat_logs(test_img, target_boot="BOOTLOG.TXT")
    content = fat_bootlog[1].decode("ascii", errors="replace") if fat_bootlog and fat_bootlog[1] else ""

    checks = [
        ("BOOTLOG.TXT" in files and files["BOOTLOG.TXT"] > 500, "FAT32 BOOTLOG.TXT populated on SD card"),
        ("BOOT0001.LOG" in files and files["BOOT0001.LOG"] > 500, "FAT32 BOOT0001.LOG session log created on SD card"),
        ("Stage 1 MBR Boot Sector" in content, "Stage 1 MBR milestone verified in SD card log"),
        ("CPU Mode: 32-bit Flat Protected Mode" in content, "Protected mode milestone verified in SD card log"),
        ("E820 System Memory Map Probed" in content, "E820 memory map verified in SD card log"),
        ("Primary xHCI Host Controller selected" in content, "xHCI discovery verified in SD card log")
    ]

    all_passed = True
    for condition, desc in checks:
        if condition:
            print(f"[PASS] {desc}")
        else:
            print(f"[FAIL] {desc}")
            all_passed = False

    raw_res = read_raw_log(test_img)
    if raw_res and raw_res.get("log_length", 0) > 0 and raw_res.get("magic1") == 0x544F4F42:
        print(f"[PASS] Raw backup sectors verified! Total written: {raw_res['total_written']:,} bytes")
    else:
        print(f"[FAIL] Raw backup sectors at LBA 1024 missing or invalid")
        all_passed = False

    if os.path.exists(test_img):
        os.remove(test_img)

    return all_passed


def test_kali_msc_boot(boot_img=DEFAULT_BOOT_IMG):
    if not os.path.exists(DEFAULT_KALI_ISO):
        print(f"[*] Skipping Kali Linux test (ISO not present at {DEFAULT_KALI_ISO})")
        return True

    print("\n" + "=" * 70)
    print("  TEST 7: Kali Linux 2026.2 Installer UMS Block Boot & Handoff Test")
    print("=" * 70)

    harness = QemuHarness(boot_img=boot_img, memory="2048M")
    extra_args = [
        "-drive", f"id=phone_disk,file={DEFAULT_KALI_ISO},format=raw,if=none,readonly=on",
        "-device", "usb-storage,bus=xhci.0,drive=phone_disk"
    ]

    def trigger(conn, current_log, state, qmp):
        if not state.get("sent_os") and "[MENU] Select option" in current_log:
            print("[*] Main boot menu detected! Selecting OS [1] (Kali Linux)...")
            time.sleep(0.1)
            conn.sendall(b"1\n")
            state["sent_os"] = True

        if not state.get("sent_prof") and "[PROFILE] Select persistence profile" in current_log:
            print("[*] Persistence Profile menu detected! Selecting Clean Disposable Session [1]...")
            time.sleep(0.1)
            conn.sendall(b"1\n")
            state["sent_prof"] = True

        if "HANDING OFF EXECUTION TO LINUX" in current_log:
            time.sleep(0.3)
            return True
        return False

    captured_log = harness.run(extra_args=extra_args, interaction_fn=trigger, timeout_sec=30)

    checks = [
        ("Volume ID: 'Kali Linux amd64 1'", "Kali ISO Volume ID detected"),
        ("Kali Linux (Graphical Install)", "Kali Linux Graphical Install entry recognized"),
        ("Resolved Kernel : '/install.amd/vmlinuz'", "Kali kernel resolved from ISO"),
        ("Resolved Initrd : '/install.amd/gtk/initrd.gz'", "Kali GTK initrd resolved from ISO"),
        ("Distro Type    : Kali Linux Installer", "Distro Type identified as Kali Linux Installer"),
        ("HANDING OFF EXECUTION TO LINUX", "Kernel handoff reached for Kali Linux")
    ]

    all_passed = True
    for needle, desc in checks:
        if needle in captured_log:
            print(f"[PASS] {desc}: '{needle}'")
        else:
            print(f"[FAIL] Missing {desc}: '{needle}'")
            all_passed = False

    return all_passed


def create_synthetic_windows_iso(output_path):
    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    SECTOR_SIZE = 2048
    TOTAL_SECTORS = 2048
    iso = bytearray(TOTAL_SECTORS * SECTOR_SIZE)

    # Sector 0: Boot sector (VBR)
    # x86 infinite loop: jmp $ (EB FE) + padding + 0x55 0xAA
    iso[0:2] = b"\xeb\xfe"
    iso[510:512] = b"\x55\xaa"

    # Sector 16: Primary Volume Descriptor (PVD)
    pvd_offset = 16 * SECTOR_SIZE
    iso[pvd_offset + 0] = 0x01 # Type = 1 (PVD)
    iso[pvd_offset + 1:pvd_offset + 6] = b"CD001"
    iso[pvd_offset + 6] = 0x01 # Version = 1
    iso[pvd_offset + 8:pvd_offset + 16] = b"WIN_TEST"
    vol_id = b"ESD-ISO_WIN11".ljust(32, b" ")
    iso[pvd_offset + 40:pvd_offset + 72] = vol_id
    iso[pvd_offset + 80:pvd_offset + 84] = TOTAL_SECTORS.to_bytes(4, "little")
    iso[pvd_offset + 84:pvd_offset + 88] = TOTAL_SECTORS.to_bytes(4, "big")
    iso[pvd_offset + 128:pvd_offset + 130] = SECTOR_SIZE.to_bytes(2, "little")
    iso[pvd_offset + 130:pvd_offset + 132] = SECTOR_SIZE.to_bytes(2, "big")

    # Root Directory Record at offset 156 (34 bytes)
    root_rec = bytearray(34)
    root_rec[0] = 34
    root_rec[2:6] = (18).to_bytes(4, "little")
    root_rec[6:10] = (18).to_bytes(4, "big")
    root_rec[10:14] = SECTOR_SIZE.to_bytes(4, "little")
    root_rec[14:18] = SECTOR_SIZE.to_bytes(4, "big")
    root_rec[25] = 0x02
    root_rec[32] = 1
    root_rec[33] = 0x00
    iso[pvd_offset + 156:pvd_offset + 190] = root_rec

    # Sector 17: Volume Descriptor Set Terminator
    term_offset = 17 * SECTOR_SIZE
    iso[term_offset + 0] = 0xFF
    iso[term_offset + 1:term_offset + 6] = b"CD001"
    iso[term_offset + 6] = 0x01

    # Sector 18: Root Directory
    root_dir_offset = 18 * SECTOR_SIZE
    pos = root_dir_offset
    iso[pos:pos + 34] = root_rec
    pos += 34
    parent_rec = bytearray(root_rec)
    parent_rec[33] = 0x01
    iso[pos:pos + 34] = parent_rec
    pos += 34

    # Entry 3: 'BOOTMGR;1'
    name = b"BOOTMGR;1"
    rlen = 33 + len(name)
    if rlen % 2 != 0:
        rlen += 1
    bm_rec = bytearray(rlen)
    bm_rec[0] = rlen
    bm_rec[2:6] = (20).to_bytes(4, "little")
    bm_rec[6:10] = (20).to_bytes(4, "big")
    bm_rec[10:14] = (409600).to_bytes(4, "little")
    bm_rec[14:18] = (409600).to_bytes(4, "big")
    bm_rec[25] = 0x00
    bm_rec[32] = len(name)
    bm_rec[33:33 + len(name)] = name
    iso[pos:pos + rlen] = bm_rec

    with open(output_path, "wb") as f:
        f.write(iso)
    return output_path


def test_windows_chainload_boot(boot_img=DEFAULT_BOOT_IMG, win_iso_path=None):
    print("\n" + "=" * 70)
    print("  TEST 8: Windows 10/11 Installation Media & VBR Chainloader Test")
    print("=" * 70)

    if not win_iso_path:
        candidate_paths = [
            r"C:\Users\chaha\Downloads\Win11_25H2_English_x64_v2.iso",
            r"C:\Users\chaha\Downloads\windows.iso"
        ]
        for cp in candidate_paths:
            if os.path.exists(cp):
                win_iso_path = cp
                break

    if not win_iso_path or not os.path.exists(win_iso_path):
        win_iso_path = os.path.join(BUILD_DIR, "test_windows.iso")
        create_synthetic_windows_iso(win_iso_path)
        print(f"[*] Generated synthetic Windows 10/11 Test ISO at: {win_iso_path}")
    else:
        print(f"[*] Testing against genuine Windows Installation ISO: {win_iso_path}")

    harness = QemuHarness(boot_img=boot_img, memory="1024M")
    extra_args = [
        "-drive", f"id=win_disk,file={win_iso_path},format=raw,if=none,readonly=on",
        "-device", "usb-storage,bus=xhci.0,drive=win_disk"
    ]

    def trigger(conn, current_log, state, qmp):
        if not state.get("sent_os") and "[MENU] Select option" in current_log:
            print("[*] Main boot menu detected! Selecting Windows OS option...")
            time.sleep(0.1)
            m = re.search(r"\[(\d+)\]\s+Windows", current_log)
            win_opt = m.group(1) if m else "1"
            conn.sendall(f"{win_opt}\n".encode("ascii"))
            state["sent_os"] = True

        if not state.get("sent_prof") and "[PROFILE] Select persistence profile" in current_log:
            print("[*] Persistence Profile menu detected! Selecting Clean Disposable Session [1]...")
            time.sleep(0.1)
            conn.sendall(b"1\n")
            state["sent_prof"] = True

        if "Handing off to Real-Mode VBR Chainloader" in current_log:
            time.sleep(0.3)
            return True
        return False

    captured_log = harness.run(extra_args=extra_args, interaction_fn=trigger, timeout_sec=25)

    with open(os.path.join(BUILD_DIR, "serial_windows.log"), "w", encoding="utf-8", errors="ignore") as f:
        f.write(captured_log)

    checks = [
        ("BOOTABLE WINDOWS MEDIA DETECTED ON USB BLOCK DEVICE!", "Windows bootable media detected"),
        ("Microsoft Windows Setup / WinPE", "Identified as Microsoft Windows Setup / WinPE"),
        ("Real-Mode VBR Chainload", "Mode selected as Real-Mode VBR Chainload"),
        ("Boot sector loaded at 0x00007C00 with valid signature.", "Boot sector loaded and verified"),
        ("Handing off to Real-Mode VBR Chainloader", "Handoff to Real-Mode VBR Chainloader reached")
    ]

    all_passed = True
    for needle, desc in checks:
        if needle in captured_log:
            print(f"[PASS] {desc}: '{needle}'")
        else:
            print(f"[FAIL] Missing {desc}: '{needle}'")
            all_passed = False

    return all_passed


def run_all_tests(boot_img=DEFAULT_BOOT_IMG):
    print("=" * 70)
    print("  BOOTLOADER AUTOMATED REGRESSION SUITE")
    print(f"  Target Image: {boot_img}")
    print("=" * 70)

    results = []
    results.append(("Baseline Boot Protocol & Self-Test", test_baseline_self_test(boot_img)))
    results.append(("Multi-Profile Persistence Sub-Menu", test_multiprofile_persistence(boot_img)))
    results.append(("Dynamic Custom Profile Sizing", test_custom_capacity_selection(boot_img)))
    results.append(("In-RAM ISO Detection & Handoff", test_in_ram_iso_boot(boot_img)))
    results.append(("Kali Linux 2026.2 UMS Block Boot", test_kali_msc_boot(boot_img)))
    results.append(("Windows 10/11 VBR Chainloader", test_windows_chainload_boot(boot_img)))
    results.append(("Dynamic Phone Gadget Mode-Switch (QMP)", test_dynamic_gadget_switch(boot_img)))
    results.append(("SD Card Live Logging Persistence", test_sd_card_persistence(boot_img)))

    print("\n" + "=" * 70)
    print("  FINAL REGRESSION TEST RESULTS:")
    print("=" * 70)
    all_passed = True
    for name, ok in results:
        status = "[PASS]" if ok else "[FAIL]"
        print(f"  {status} {name}")
        if not ok:
            all_passed = False

    print("=" * 70)
    return all_passed


if __name__ == "__main__":
    success = run_all_tests()
    sys.exit(0 if success else 1)
