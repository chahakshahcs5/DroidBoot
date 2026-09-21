#!/system/bin/sh
# tools/phone_ums_enable.sh - Convert Rooted Android Phone (whyred/Xiaomi/any)
# from MTP to USB Mass Storage (SCSI block device) for direct ISO booting!

echo "=========================================================="
echo "  ANDROID ROOT USB MASS STORAGE (UMS) ENABLER"
echo "=========================================================="

# 1. Check Root
if [ "$(id -u)" != "0" ]; then
    echo "[-] Error: This script must be run as root (su)!"
    exit 1
fi

# 2. Locate ISO Image on phone storage
ISO_TARGET="$1"
if [ -z "$ISO_TARGET" ]; then
    echo "[*] No ISO path specified. Searching common storage locations..."
    for d in /sdcard/Download /sdcard/Downloads /sdcard/ISO /sdcard/ISOs /sdcard/Ventoy /sdcard /storage/emulated/0/Download /storage/emulated/0 /storage/* /storage/*/Download; do
        found=$(ls "$d/"*.iso "$d/"*.img 2>/dev/null | head -n 1)
        if [ -n "$found" ]; then
            ISO_TARGET="$found"
            break
        fi
    done
fi

if [ -z "$ISO_TARGET" ] || [ ! -f "$ISO_TARGET" ]; then
    echo "[-] No ISO image found! Please specify the path:"
    echo "    su -c \"sh $0 /sdcard/Download/alpine-standard-3.24.2-x86_64.iso\""
    exit 2
fi

echo "[+] Target ISO image: $ISO_TARGET"
echo "    Size: $(ls -lh "$ISO_TARGET" | awk '{print $5}')"

# 3. Detect USB Gadget Framework (ConfigFS or legacy Sysfs)
if [ -d /config/usb_gadget/g1 ]; then
    GADGET_DIR="/config/usb_gadget/g1"
    echo "[+] Android ConfigFS gadget subsystem detected at $GADGET_DIR"

    FUNC_NAME="mass_storage.0"
    if [ ! -d "$GADGET_DIR/functions/$FUNC_NAME" ]; then
        mkdir -p "$GADGET_DIR/functions/$FUNC_NAME" 2>/dev/null
        if [ ! -d "$GADGET_DIR/functions/$FUNC_NAME" ]; then
            FUNC_NAME="mass_storage.usb0"
            mkdir -p "$GADGET_DIR/functions/$FUNC_NAME" 2>/dev/null
        fi
    fi

    if [ ! -d "$GADGET_DIR/functions/$FUNC_NAME" ]; then
        echo "[-] Kernel lacks CONFIG_USB_CONFIGFS_MASS_STORAGE driver!"
        echo "    Please install Magisk module: 'USB Mass Storage Enabler' or DriveDroid."
        exit 3
    fi

    LUN="$GADGET_DIR/functions/$FUNC_NAME/lun.0"
    mkdir -p "$LUN" 2>/dev/null

    echo "[*] Configuring LUN for 512-byte SCSI block emulation..."
    echo "$ISO_TARGET" > "$LUN/file"
    echo 1 > "$LUN/ro"
    echo 0 > "$LUN/cdrom"

    ACT_FILE=$(cat "$LUN/file" 2>/dev/null)
    if [ -z "$ACT_FILE" ]; then
        echo "[-] Failed to attach ISO to LUN file!"
        exit 4
    fi
    echo "[+] LUN attached successfully to: $ACT_FILE"

    CFG=$(ls -d "$GADGET_DIR/configs/"*.* 2>/dev/null | head -n 1)
    UDC=$(cat "$GADGET_DIR/UDC" 2>/dev/null)
    [ -z "$UDC" ] && UDC=$(ls /sys/class/udc 2>/dev/null | head -n 1)

    echo "[*] Cycling UDC to switch from MTP to USB Mass Storage..."
    echo "" > "$GADGET_DIR/UDC"
    rm -f "$CFG/f"* 2>/dev/null
    ln -s "$GADGET_DIR/functions/$FUNC_NAME" "$CFG/f1"
    [ -d "$GADGET_DIR/functions/ffs.adb" ] && ln -s "$GADGET_DIR/functions/ffs.adb" "$CFG/f2"
    echo "$UDC" > "$GADGET_DIR/UDC"

    setprop sys.usb.state mass_storage,adb 2>/dev/null || setprop sys.usb.state mass_storage 2>/dev/null

    echo "=========================================================="
    echo "  SUCCESS: PHONE IS NOW IN USB MASS STORAGE (UMS) MODE!"
    echo "  * Emulating: $ISO_TARGET"
    echo "  * The laptop will now detect the phone as a USB Disk (/dev/sdb)"
    echo "=========================================================="

elif [ -d /sys/class/android_usb/android0 ]; then
    echo "[+] Legacy android_usb sysfs gadget detected."
    SYS_DIR="/sys/class/android_usb/android0"

    echo 0 > "$SYS_DIR/enable"
    LUN="$SYS_DIR/f_mass_storage/lun"
    [ ! -d "$LUN" ] && LUN="$SYS_DIR/f_mass_storage/lun0"

    echo "$ISO_TARGET" > "$LUN/file"
    echo 1 > "$LUN/ro"
    echo 0 > "$LUN/cdrom"
    echo "mass_storage,adb" > "$SYS_DIR/functions"
    echo 1 > "$SYS_DIR/enable"

    echo "=========================================================="
    echo "  SUCCESS: PHONE IS NOW IN USB MASS STORAGE (UMS) MODE!"
    echo "=========================================================="
else
    echo "[-] Unknown USB gadget framework on this phone."
    exit 5
fi
