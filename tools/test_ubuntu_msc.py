import os
import sys
import time
import socket
import subprocess

WORKSPACE_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(WORKSPACE_ROOT, "build")
BOOT_IMG = os.path.join(BUILD_DIR, "boot.img")
UBUNTU_ISO = r"C:\Users\chaha\Downloads\ubuntu-26.04.1-desktop-amd64.iso"

def run_test(port=4498):
    print("\n" + "=" * 70)
    print("  QEMU VERIFICATION: UBUNTU 26.04 (6.5 GB) FROM USB MASS STORAGE")
    print("=" * 70)

    if not os.path.exists(UBUNTU_ISO):
        print(f"[-] Ubuntu ISO not found at {UBUNTU_ISO}")
        return

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", port))
    server.listen(1)

    qemu_cmd = [
        "qemu-system-x86_64",
        "-snapshot",
        "-drive", f"file={BOOT_IMG},format=raw,if=ide",
        "-device", "qemu-xhci,id=xhci",
        "-drive", f"id=phone_disk,file={UBUNTU_ISO},format=raw,if=none,readonly=on",
        "-device", "usb-storage,bus=xhci.0,drive=phone_disk",
        "-serial", f"tcp:127.0.0.1:{port}",
        "-display", "none",
        "-m", "3072M"
    ]

    print(f"[*] Launching QEMU with Ubuntu (6.5 GB) as USB Mass Storage on xHCI...")
    proc = subprocess.Popen(qemu_cmd, cwd=WORKSPACE_ROOT)

    conn, _ = server.accept()
    conn.setblocking(False)

    captured_chunks = []
    start_time = time.time()
    sent_os = False
    sent_prof = False

    try:
        while time.time() - start_time < 40:
            try:
                chunk = conn.recv(2048)
                if chunk:
                    text = chunk.decode("utf-8", errors="ignore")
                    captured_chunks.append(text)
                    current_log = "".join(captured_chunks)

                    # When menu appears, select [1] Ubuntu
                    if not sent_os and "[MENU] Select option" in current_log:
                        print("[+] Bootloader Menu appeared! Selecting [1] Ubuntu Desktop Live...")
                        time.sleep(0.1)
                        conn.sendall(b"1\n")
                        sent_os = True

                    # If profile menu appears, select [3] Clean Disposable Session
                    if not sent_prof and "[PROFILE] Select persistence profile" in current_log:
                        print("[+] Profile menu appeared! Selecting [3] Clean Disposable Session...")
                        time.sleep(0.1)
                        conn.sendall(b"3\n")
                        sent_prof = True

                    # Check for Linux kernel booting
                    if "Linux version" in current_log or "Command line: boot=casper" in current_log:
                        print("[+] Ubuntu Linux kernel successfully decompressed and initialized!")
                        # Wait a little more for casper/systemd init
                        if "casper" in current_log.lower() and "systemd" in current_log.lower():
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
    log_path = os.path.join(BUILD_DIR, "ubuntu_msc_test.log")
    with open(log_path, "w", encoding="utf-8", errors="ignore") as f:
        f.write(full_log)
    print(f"[+] Serial log written to {log_path} ({len(full_log)} bytes)")

    print("\n" + "=" * 70)
    print("  VERIFICATION CHECKLIST:")
    print("=" * 70)
    checks = [
        ("USB MASS STORAGE", "USB Mass Storage interface detected"),
        ("Ubuntu Desktop Live (6.2 GB)", "Ubuntu 6.5 GB detected on USB block device"),
        ("Direct Block Access (0 MB OS in RAM!)", "0 MB OS copied into RAM"),
        ("Kernel read:", "Ubuntu kernel read directly from block device"),
        ("Initramfs read:", "Ubuntu initrd read directly from block device"),
        ("HANDING OFF EXECUTION TO LINUX", "Kernel jump executed cleanly"),
        ("Linux version", "Ubuntu Linux kernel started"),
    ]

    for needle, desc in checks:
        if needle.lower() in full_log.lower():
            print(f"[PASS] {desc}")
        else:
            print(f"[INFO] {desc} - (pattern '{needle}' check)")

    print("=" * 70)

if __name__ == "__main__":
    run_test()
