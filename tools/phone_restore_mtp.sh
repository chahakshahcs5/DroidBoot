#!/system/bin/sh
# tools/phone_restore_mtp.sh - Restore phone back to normal MTP + ADB mode

echo "[*] Restoring phone to normal MTP + ADB gadget mode..."

# 1. Release any attached image from mass_storage LUNs
for lun_file in /config/usb_gadget/g1/functions/mass_storage.*/lun*/file /sys/class/android_usb/android0/f_mass_storage/lun*/file; do
    if [ -f "$lun_file" ]; then
        echo "" > "$lun_file" 2>/dev/null
    fi
done

# 2. ConfigFS gadget restoration
if [ -d /config/usb_gadget/g1 ]; then
    GADGET_DIR="/config/usb_gadget/g1"
    CFG=$(ls -d "$GADGET_DIR/configs/"*.* 2>/dev/null | head -n 1)
    UDC=$(cat "$GADGET_DIR/UDC" 2>/dev/null)
    [ -z "$UDC" ] && UDC=$(ls /sys/class/udc 2>/dev/null | head -n 1)

    # Disconnect gadget from UDC
    echo "" > "$GADGET_DIR/UDC" 2>/dev/null
    rm -f "$CFG/f"* 2>/dev/null

    # Restore Vendor/Product IDs for Xiaomi MTP+ADB (or device defaults)
    echo "0x2717" > "$GADGET_DIR/idVendor" 2>/dev/null
    echo "0xFF48" > "$GADGET_DIR/idProduct" 2>/dev/null
    [ -d "$CFG/strings/0x409" ] && echo "mtp_adb" > "$CFG/strings/0x409/configuration" 2>/dev/null

    # Re-link MTP and ADB functions
    [ -d "$GADGET_DIR/functions/mtp.gs0" ] && ln -s "$GADGET_DIR/functions/mtp.gs0" "$CFG/f1"
    [ -d "$GADGET_DIR/functions/ffs.adb" ] && ln -s "$GADGET_DIR/functions/ffs.adb" "$CFG/f2"

    # Reconnect gadget
    echo "$UDC" > "$GADGET_DIR/UDC" 2>/dev/null
    setprop sys.usb.state mtp,adb 2>/dev/null
    setprop sys.usb.config mtp,adb 2>/dev/null
    svc usb setFunctions mtp 2>/dev/null

    echo "[+] Restored to MTP + ADB successfully!"

# 3. Legacy sysfs gadget restoration
elif [ -d /sys/class/android_usb/android0 ]; then
    SYS_DIR="/sys/class/android_usb/android0"
    echo 0 > "$SYS_DIR/enable" 2>/dev/null
    echo "mtp,adb" > "$SYS_DIR/functions" 2>/dev/null
    echo 1 > "$SYS_DIR/enable" 2>/dev/null
    setprop sys.usb.state mtp,adb 2>/dev/null
    setprop sys.usb.config mtp,adb 2>/dev/null
    echo "[+] Restored legacy gadget to MTP + ADB!"
fi
