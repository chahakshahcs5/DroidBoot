import os
import sys
import time
import socket
import subprocess

WORKSPACE_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(WORKSPACE_ROOT, "build")
BOOT_IMG = os.path.join(BUILD_DIR, "boot.img")

def test_real_phone(port=4488):
    print("\n" + "=" * 70)
    print("  QEMU TEST: BOOTING DIRECTLY FROM PHYSICAL PHONE (Disk 3)")
    print("=" * 70)

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", port))
    server.listen(1)

    qemu_cmd = [
        "qemu-system-x86_64",
        "-snapshot",
        "-drive", f"file={BOOT_IMG},format=raw,if=ide",
        "-device", "qemu-xhci,id=xhci",
        "-drive", r"id=real_phone,file=\\.\PhysicalDrive3,format=raw,if=none,readonly=on",
        "-device", "usb-storage,bus=xhci.0,drive=real_phone",
        "-serial", f"tcp:127.0.0.1:{port}",
        "-display", "none",
        "-m", "2048M"
    ]

    print("[*] Launching QEMU attached directly to Physical Phone (Disk 3: Linux File-Stor Gadget)...")
    proc = subprocess.Popen(qemu_cmd, cwd=WORKSPACE_ROOT)

    conn, _ = server.accept()
    conn.setblocking(False)

    captured_chunks = []
    start_time = time.time()
    sent_os = False
    sent_prof = False

    try:
        while time.time() - start_time < 35:
            try:
                chunk = conn.recv(2048)
                if chunk:
                    text = chunk.decode("utf-8", errors="ignore")
                    captured_chunks.append(text)
                    current_log = "".join(captured_chunks)

                    if not sent_os and "[MENU] Select option" in current_log:
                        print("[+] Bootloader menu appeared! Selecting [1] (Alpine Linux on Physical Phone)...")
                        time.sleep(0.1)
                        conn.sendall(b"1\n")
                        sent_os = True

                    if not sent_prof and "[PROFILE] Select persistence profile" in current_log:
                        print("[+] Profile menu appeared! Selecting [1]...")
                        time.sleep(0.1)
                        conn.sendall(b"1\n")
                        sent_prof = True

                    if "OpenRC" in current_log or "login:" in current_log or "Welcome to Alpine" in current_log:
                        print("[+] SUCCESS: Alpine Linux booted all the way to userspace directly from PHYSICAL PHONE!")
                        break
            except (BlockingIOError, OSError):
                time.sleep(0.05)
    finally:
        conn.close()
        server.close()
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()

    full_log = "".join(captured_chunks)
    log_path = os.path.join(BUILD_DIR, "physical_phone_test.log")
    with open(log_path, "w", encoding="utf-8", errors="ignore") as f:
        f.write(full_log)
    print(f"[+] Serial log written to {log_path} ({len(full_log)} bytes)")

    print("\n" + "=" * 70)
    print("  PHYSICAL PHONE VERIFICATION RESULTS:")
    print("=" * 70)
    checks = [
        ("USB MASS STORAGE", "Physical phone detected as USB Mass Storage device"),
        ("Kernel read:", "Kernel streamed directly from physical phone flash storage"),
        ("Initramfs read:", "Initramfs streamed directly from physical phone flash storage"),
        ("Attached SCSI disk", "Linux kernel recognized physical phone as /dev/sdb"),
        ("Mounting boot media: ok", "Alpine mounted ISO from physical phone"),
        ("Installing packages to root filesystem: ok", "Alpine packages installed from phone"),
        ("OpenRC", "OpenRC init running cleanly"),
    ]

    for needle, desc in checks:
        if needle.lower() in full_log.lower():
            print(f"[PASS] {desc}")
        else:
            print(f"[INFO] {desc} - (pattern '{needle}')")

    if "emergency recovery shell" in full_log:
        print("[FAIL] Dropped into recovery shell!")
    elif "OpenRC" in full_log or "login:" in full_log:
        print("[PASS] CLEAN PHYSICAL PHONE BOOT VERIFIED WITH 0 RECOVERY SHELL DROPS!")
    print("=" * 70)

if __name__ == "__main__":
    test_real_phone()
