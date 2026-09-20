#ifndef UI_MENU_H
#define UI_MENU_H

#include <stdint.h>
#include <stdbool.h>
#include "../../include/boot.h"
#include "../xhci/xhci.h"
#include "../usb/usb.h"
#include "../mtp/mtp.h"

typedef enum boot_choice {
    BOOT_CHOICE_NONE = 0,
    BOOT_CHOICE_ANDROID_MTP,
    BOOT_CHOICE_USB_MSC,
    BOOT_CHOICE_SD_FAT,
    BOOT_CHOICE_DIAGNOSTICS,
    BOOT_CHOICE_TEST_PROTOCOL
} boot_choice_t;

void menu_render(boot_info_t *boot_info, xhci_controller_t *xhci,
                 usb_device_t *usb_dev, mtp_session_t *mtp_session);
boot_choice_t menu_wait_selection(uint32_t timeout_seconds, bool has_mtp, bool has_msc);
void menu_show_diagnostics(boot_info_t *boot_info, xhci_controller_t *xhci,
                          usb_device_t *usb_dev, mtp_session_t *mtp_session);

#endif // UI_MENU_H
