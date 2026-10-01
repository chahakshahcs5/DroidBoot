#ifndef USB_HID_H
#define USB_HID_H

#include <stdint.h>
#include <stdbool.h>
#include "../xhci/xhci.h"
#include "usb.h"

// USB HID Boot Keyboard definitions
#define USB_HID_INTF_CLASS      0x03    // HID class
#define USB_HID_INTF_SUBCLASS   0x01    // Boot interface subclass
#define USB_HID_PROTOCOL_KBD    0x01    // Keyboard protocol
#define USB_HID_SET_PROTOCOL    0x0B    // SET_PROTOCOL request
#define USB_HID_BOOT_PROTOCOL   0x00    // Boot protocol value

// TUI key codes (must match menu.h definitions)
#define HID_KEY_UP       0x100
#define HID_KEY_DOWN     0x101
#define HID_KEY_LEFT     0x102
#define HID_KEY_RIGHT    0x103
#define HID_KEY_ESC      0x104
#define HID_KEY_ENTER    '\n'
#define HID_KEY_BACKSPACE '\b'

// Initialize the USB HID keyboard driver.
// Call after USB enumeration detects a HID boot keyboard device.
// Returns 0 on success, -1 on failure.
int usb_keyboard_init(xhci_controller_t *ctrl, usb_device_t *dev);

// Non-blocking poll for keyboard input.
// Returns 0 and sets *out_key if a key was pressed, -1 if no key available.
int usb_keyboard_poll(int *out_key);

// Check if a USB HID keyboard has been initialized and is available.
bool usb_keyboard_is_available(void);

// Attempt to detect and initialize a HID boot keyboard on the given device.
// Called during USB enumeration for each device.
// Returns true if the device is a keyboard and was initialized.
bool usb_keyboard_try_init(xhci_controller_t *ctrl, usb_device_t *dev);

#endif // USB_HID_H
