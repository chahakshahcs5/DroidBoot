"""
tools/flash_sd.py - Safely flash boot.img or boot-debug.img to SD card
"""
import sys
import os
import subprocess

def is_admin():
    try:
        import ctypes
        return ctypes.windll.shell32.IsUserAnAdmin()
    except Exception:
        return False

def flash(image_path, drive_num=2):
    target = rf"\\.\PhysicalDrive{drive_num}"
    print(f"[*] Target device: {target}")
    print(f"[*] Image to flash: {image_path}")
    
    if not os.path.exists(image_path):
        print(f"[-] Image file does not exist: {image_path}")
        return False

    with open(image_path, "rb") as src:
        data = src.read()
    
    print(f"[*] Writing {len(data):,} bytes ({len(data) // (1024*1024)} MiB) to {target}...")
    try:
        with open(target, "r+b") as dst:
            dst.seek(0)
            dst.write(data)
            dst.flush()
        print("[+] SUCCESS: SD card bootloader updated cleanly!")
        return True
    except Exception as e:
        print(f"[-] Write failed: {e}")
        return False

if __name__ == "__main__":
    img = sys.argv[1] if len(sys.argv) > 1 else os.path.join("build", "boot-debug.img")
    drive = int(sys.argv[2]) if len(sys.argv) > 2 else 2

    if not is_admin():
        print(f"[*] Requesting Administrator elevation to flash PhysicalDrive{drive}...")
        script = os.path.abspath(__file__)
        img_abs = os.path.abspath(img)
        cmd = f'Start-Process python -ArgumentList \'"{script}" "{img_abs}" {drive}\' -Verb RunAs -Wait'
        res = subprocess.run(["powershell", "-Command", cmd])
        sys.exit(res.returncode)
    else:
        success = flash(img, drive)
        if not success:
            sys.exit(1)
