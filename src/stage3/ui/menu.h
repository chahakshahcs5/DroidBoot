#ifndef UI_MENU_H
#define UI_MENU_H

#include <stdint.h>
#include <stdbool.h>
#include "../../include/boot.h"
#include "../xhci/xhci.h"
#include "../usb/usb.h"
#include "../mtp/mtp.h"
#include "../image/os_scanner.h"

typedef enum {
    MENU_ACTION_NONE = 0,
    MENU_ACTION_BOOT_OS,     // Boot discovered OS at os_index
    MENU_ACTION_DIAGNOSTICS, // Hardware Diagnostics
    MENU_ACTION_SELF_TEST,   // Linux 32-bit Boot Protocol Self-Test
    MENU_ACTION_RESCAN,      // Rescan USB and storage devices
    MENU_ACTION_SWITCH_UMS   // Switch Rooted Android phone to USB Mass Storage (UMS)
} menu_action_type_t;

typedef struct {
    menu_action_type_t type;
    uint32_t           os_index; // Index into os_registry_t (0..count-1)
} menu_selection_t;

void menu_render(boot_info_t *boot_info, xhci_controller_t *xhci,
                 usb_device_t *usb_dev, mtp_session_t *mtp_session,
                 const os_registry_t *registry);

menu_selection_t menu_wait_selection(const os_registry_t *registry);

int      menu_select_phone_image(const os_registry_t *registry);
int      menu_select_persistence_profile(os_entry_t *entry);
uint64_t menu_prompt_profile_size(void);

void menu_show_diagnostics(boot_info_t *boot_info, xhci_controller_t *xhci,
                           usb_device_t *usb_dev, mtp_session_t *mtp_session);

int menu_get_char(void);

#endif // UI_MENU_H
