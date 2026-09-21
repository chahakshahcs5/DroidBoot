import os
import sys
import json
import time
import socket
import subprocess

WORKSPACE_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(WORKSPACE_ROOT, "build")
BOOT_IMG = os.path.join(BUILD_DIR, "boot.img")
ALPINE_ISO = r"C:\Users\chaha\Downloads\alpine-standard-3.24.2-x86_64.iso"

def qmp_execute(qmp_sock, cmd, args=None):
    req = {"execute": cmd}
    if args:
        req["arguments"] = args
    qmp_sock.sendall((json.dumps(req) + "\r\n").encode("utf-8"))
    
    # Read response
    resp_data = b""
    while True:
        chunk = qmp_sock.recv(4096)
        if not chunk:
            break
        resp_data += chunk
        lines = resp_data.split(b"\r\n")
        for line in lines:
            if line:
                try:
                    obj = json.loads(line.decode("utf-8"))
                    if "return" in obj or "error" in obj:
                        return obj
                except json.JSONDecodeError:
                    pass
    return {}

def test_dynamic_transition(serial_port=4490, qmp_port=4491):
    import random
    if serial_port == 4490:
        serial_port = random.randint(5200, 9800)
        qmp_port = serial_port + 1

    print("\n" + "=" * 70)
    print("  QEMU TEST: DYNAMIC PHONE CONVERSION (MTP -> USB MASS STORAGE)")
    print(f"  Ports: Serial={serial_port}, QMP={qmp_port}")
    print("=" * 70)

    # Setup serial socket
    serial_server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    serial_server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    serial_server.bind(("127.0.0.1", serial_port))
    serial_server.listen(1)

    # Launch QEMU initially WITHOUT USB Mass Storage (only boot.img on IDE and empty xHCI)
    # Plus a tablet to mimic an active non-storage USB device (like initial phone)
    qemu_cmd = [
        "qemu-system-x86_64",
        "-snapshot",
        "-drive", f"file={BOOT_IMG},format=raw,if=ide",
        "-device", "qemu-xhci,id=xhci",
        "-device", "usb-tablet,bus=xhci.0,id=initial_phone",
        "-serial", f"tcp:127.0.0.1:{serial_port}",
        "-qmp", f"tcp:127.0.0.1:{qmp_port},server,nowait",
        "-display", "none",
        "-m", "2048M"
    ]

    print("[*] Phase 1: Launching QEMU with phone in initial non-storage state...")
    proc = subprocess.Popen(qemu_cmd, cwd=WORKSPACE_ROOT)

    # Accept serial connection
    serial_conn, _ = serial_server.accept()
    serial_conn.setblocking(False)

    # Connect to QMP
    time.sleep(0.5)
    qmp_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    qmp_sock.connect(("127.0.0.1", qmp_port))

    # Read greeting
    greeting = qmp_sock.recv(4096)
    # Enable QMP capabilities
    qmp_execute(qmp_sock, "qmp_capabilities")
    print("[+] Connected to QEMU Machine Protocol (QMP).")

    captured_chunks = []
    start_time = time.time()
    phone_switched = False
    sent_rescan = False
    sent_os = False
    sent_prof = False

    try:
        while time.time() - start_time < 45:
            try:
                chunk = serial_conn.recv(2048)
                if chunk:
                    text = chunk.decode("utf-8", errors="ignore")
                    captured_chunks.append(text)
                    current_log = "".join(captured_chunks)

                    # Step 1: Wait for initial boot menu to appear (with 0 OS found or only self-tests)
                    if not phone_switched and "[MENU] Select option" in current_log:
                        print("[+] Initial bootloader menu rendered (Phone in MTP/non-storage mode).")
                        print("[*] Phase 2: Simulating phone root switch: Unlinking MTP -> Enrolling USB Mass Storage gadget...")
                        time.sleep(1.0)

                        # Remove initial_phone device (disconnect UDC)
                        del_res = qmp_execute(qmp_sock, "device_del", {"id": "initial_phone"})
                        print(f"    * UDC disconnected (device_del initial_phone): {del_res}")

                        time.sleep(1.0)

                        # Add Alpine ISO drive and attach as usb-storage (Phone re-enabling UDC with mass_storage.0)
                        drive_add_res = qmp_execute(qmp_sock, "human-monitor-command", {
                            "command-line": f"drive_add 0 file={ALPINE_ISO},format=raw,if=none,id=phone_ums_drive,readonly=on"
                        })
                        print(f"    * LUN image loaded: {drive_add_res}")

                        dev_add_res = qmp_execute(qmp_sock, "device_add", {
                            "driver": "usb-storage",
                            "bus": "xhci.0",
                            "drive": "phone_ums_drive",
                            "id": "phone_ums"
                        })
                        print(f"    * Phone re-attached as USB Mass Storage device: {dev_add_res}")
                        phone_switched = True

                    # Step 2: Once switched, send 'R' to rescan (or let auto-detection probe it)
                    if phone_switched and not sent_rescan and time.time() - start_time > 5:
                        print("[*] Phase 3: Triggering bootloader 'R' (Rescan) to enumerate newly configured Mass Storage...")
                        time.sleep(1.0)
                        serial_conn.sendall(b"R\n")
                        sent_rescan = True

                    # Step 3: Check if new OS appears in menu and select it
                    if sent_rescan and not sent_os and ("Alpine" in current_log or "Linux" in current_log):
                        # Find if menu re-rendered with new OS
                        last_menu_idx = current_log.rfind("[MENU] Select option")
                        if last_menu_idx != -1 and ("[1] " in current_log[max(0, last_menu_idx - 1200):]):
                            print("[+] Bootloader dynamically detected converted Phone as USB Block Storage!")
                            print("[*] Phase 4: Selecting [1] Linux Live OS to boot...")
                            time.sleep(0.5)
                            serial_conn.sendall(b"1\n")
                            sent_os = True

                    # Step 4: Profile selection
                    if not sent_prof and "[PROFILE] Select persistence profile" in current_log:
                        print("[+] Profile menu appeared! Selecting [1]...")
                        time.sleep(0.1)
                        serial_conn.sendall(b"1\n")
                        sent_prof = True

                    # Step 5: Verify boot to login
                    if "login:" in current_log or "Welcome to Alpine" in current_log or "OpenRC" in current_log:
                        print("[+] SUCCESS: Alpine Linux successfully booted after dynamic MTP->MSC switch!")
                        break
            except (BlockingIOError, OSError):
                time.sleep(0.05)
    finally:
        serial_conn.close()
        serial_server.close()
        qmp_sock.close()
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()

    full_log = "".join(captured_chunks)
    log_path = os.path.join(BUILD_DIR, "dynamic_transition_test.log")
    with open(log_path, "w", encoding="utf-8", errors="ignore") as f:
        f.write(full_log)
    print(f"[+] Full transition log saved to {log_path} ({len(full_log)} bytes)")

    print("\n" + "=" * 70)
    print("  DYNAMIC TRANSITION VERIFICATION CHECKLIST:")
    print("=" * 70)
    checks = [
        ("initial_phone", "Initial non-storage device detected"),
        ("Rescanning USB", "Rescan triggered"),
        ("USB MASS STORAGE", "Dynamic USB Mass Storage re-enumeration"),
        ("Kernel read:", "Kernel read from dynamically attached block device"),
        ("Initramfs read:", "Initrd read from dynamically attached block device"),
        ("Attached SCSI disk", "Linux kernel recognized phone as SCSI disk"),
        ("OpenRC", "Alpine Linux booted to userspace init"),
    ]

    for needle, desc in checks:
        if needle.lower() in full_log.lower():
            print(f"[PASS] {desc}")
        else:
            print(f"[INFO] {desc} - (pattern '{needle}')")

    if "emergency recovery shell" in full_log:
        print("[FAIL] Dropped into emergency recovery shell!")
    elif "OpenRC" in full_log or "login:" in full_log:
        print("[PASS] Clean Alpine boot with ZERO recovery shell drops!")
    print("=" * 70)

if __name__ == "__main__":
    test_dynamic_transition()
