#!/system/bin/sh
# tools/phone_restore_mtp.sh - Restore phone back to normal MTP + ADB mode

echo "[*] Restoring phone to normal MTP + ADB gadget mode..."
if [ -d /config/usb_gadget/g1 ]; then
    GADGET_DIR="/config/usb_gadget/g1"
    CFG=$(ls -d "$GADGET_DIR/configs/"*.* 2>/dev/null | head -n 1)
    UDC=$(cat "$GADGET_DIR/UDC" 2>/dev/null)
    [ -z "$UDC" ] && UDC=$(ls /sys/class/udc 2>/dev/null | head -n 1)

    echo "" > "$GADGET_DIR/UDC" 2>/dev/null
    rm -f "$CFG/f"* 2>/dev/null
    [ -d "$GADGET_DIR/functions/mtp.gs0" ] && ln -s "$GADGET_DIR/functions/mtp.gs0" "$CFG/f1"
    [ -d "$GADGET_DIR/functions/ffs.adb" ] && ln -s "$GADGET_DIR/functions/ffs.adb" "$CFG/f2"
    echo "$UDC" > "$GADGET_DIR/UDC" 2>/dev/null
    setprop sys.usb.state mtp,adb 2>/dev/null
    echo "[+] Restored to MTP + ADB successfully!"
fi
