#include "vbe.h"
#include "../core/printf.h"

static void vbe_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

static void vbe_memset(void *dst, uint8_t val, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = val;
}

int vbe_get_controller_info(vbe_info_block_t *out_info) {
    vbe_info_block_t *low_buf = (vbe_info_block_t *)VBE_INFO_BLOCK_PHYS;
    vbe_memset(low_buf, 0, sizeof(vbe_info_block_t));

    // Request VBE 2.0+ info
    low_buf->signature[0] = 'V';
    low_buf->signature[1] = 'B';
    low_buf->signature[2] = 'E';
    low_buf->signature[3] = '2';

    uint16_t ax = bios_int10_call(0x4F00, 0, 0, 0, 0x0000, (uint16_t)VBE_INFO_BLOCK_PHYS);
    if (ax != 0x004F) {
        log_error("VBE", "Get Controller Info failed (AX=0x%04X)", ax);
        return -1;
    }

    if (low_buf->signature[0] != 'V' || low_buf->signature[1] != 'E' ||
        low_buf->signature[2] != 'S' || low_buf->signature[3] != 'A') {
        log_error("VBE", "Invalid VBE signature: '%.4s'", low_buf->signature);
        return -2;
    }

    log_info("VBE", "VBE Controller: Version %u.%u, Total Memory: %u KB",
             (low_buf->version >> 8) & 0xFF, low_buf->version & 0xFF,
             (uint32_t)low_buf->total_memory * 64);

    if (out_info) {
        vbe_memcpy(out_info, low_buf, sizeof(vbe_info_block_t));
    }
    return 0;
}

int vbe_get_mode_info(uint16_t mode, vbe_mode_info_t *out_info) {
    vbe_mode_info_t *low_buf = (vbe_mode_info_t *)VBE_MODE_INFO_PHYS;
    vbe_memset(low_buf, 0, sizeof(vbe_mode_info_t));

    uint16_t ax = bios_int10_call(0x4F01, 0, mode, 0, 0x0000, (uint16_t)VBE_MODE_INFO_PHYS);
    if (ax != 0x004F) {
        return -1;
    }

    if (out_info) {
        vbe_memcpy(out_info, low_buf, sizeof(vbe_mode_info_t));
    }
    return 0;
}

static bool vbe_active = false;
static vbe_mode_info_t active_mode;

bool vbe_is_active(void) {
    return vbe_active;
}

const vbe_mode_info_t *vbe_get_active_mode(void) {
    return vbe_active ? &active_mode : 0;
}

int vbe_restore_text_mode(void) {
    if (!vbe_active) return 0;
    // Set standard VGA 80x25 16-color text mode (Mode 03h)
    uint16_t ax = bios_int10_call(0x0003, 0, 0, 0, 0, 0);
    vbe_active = false;
    log_info("VBE", "Restored standard VGA text mode (0x03, ret=0x%04X)", ax);
    return 0;
}

int vbe_set_mode(uint16_t mode) {
    // Bit 14 (0x4000) = Enable Linear Frame Buffer mode
    // Bit 15 (0x8000) = Preserve display memory (do not clear), leave 0 to clear
    uint16_t bx = (mode & 0x3FFF) | 0x4000;
    uint16_t ax = bios_int10_call(0x4F02, bx, 0, 0, 0, 0);
    if (ax != 0x004F) {
        log_error("VBE", "Set Video Mode 0x%04X failed (AX=0x%04X)", mode, ax);
        return -1;
    }
    return 0;
}

int vbe_setup_linear_framebuffer(vbe_mode_info_t *out_selected_mode, uint16_t *out_mode_num) {
    vbe_info_block_t ctrl_info;
    int has_ctrl = (vbe_get_controller_info(&ctrl_info) == 0);

    // List of candidate VBE graphics modes to test.
    // 0x0118 (1024x768x32/24) is universally supported by laptop VBIOSes and HDMI displays at 60Hz.
    static const uint16_t candidate_modes[] = {
        0x0118, // 1024x768x32/24 (Optimal balance: supported on all laptop BIOSes & external monitors)
        0x0115, // 800x600x32/24
        0x011B, // 1280x1024x32/24
        0x0112, // 640x480x32/24
        0x0117, // 1024x768x16
        0x0114  // 800x600x16
    };
    uint32_t num_candidates = sizeof(candidate_modes) / sizeof(candidate_modes[0]);

    // 1. VESA DPMS: Force all connected display outputs to fully ON state (Power State 00h)
    bios_int10_call(0x4F10, 0x0001, 0, 0, 0, 0);

    // 2. VESA DDC: Probe Display Data Channel to trigger HDMI hotplug / EDID read on VBIOS
    bios_int10_call(0x4F15, 0x0000, 0, 0, 0, 0);

    // 3. Intel/OEM VBIOS Extension: Request simultaneous display on External HDMI/DVI + CRT + LCD
    // AX=5F35h (Set Display Device), CX=0x000F (Bit 0: CRT, Bit 1: TV, Bit 2: LCD, Bit 3: DVI/HDMI)
    bios_int10_call(0x5F35, 0x0001, 0x000F, 0, 0, 0);

    // Check dynamic mode list from VBE Controller Info first (to find widescreen modes like 1920x1080)
    if (has_ctrl && ctrl_info.video_mode_ptr != 0) {
        uint32_t mode_list_phys = ((ctrl_info.video_mode_ptr >> 16) << 4) +
                                  (ctrl_info.video_mode_ptr & 0xFFFF);
        if (mode_list_phys < 0x100000) {
            const uint16_t *dyn_modes = (const uint16_t *)mode_list_phys;
            for (uint32_t i = 0; dyn_modes[i] != 0xFFFF && i < 128; i++) {
                uint16_t cand = dyn_modes[i];
                vbe_mode_info_t mi;
                if (vbe_get_mode_info(cand, &mi) != 0) continue;
                if (!(mi.attributes & 0x0001)) continue; // Supported
                if (!(mi.attributes & 0x0010)) continue; // Graphics
                if (!(mi.attributes & 0x0080)) continue; // Linear Framebuffer
                if (mi.phys_base_ptr == 0) continue;
                if (mi.bits_per_pixel < 16) continue;

                // If 1920x1080 (Full HD @ 60Hz) is natively supported by the laptop VBIOS:
                if (mi.x_res == 1920 && mi.y_res == 1080 && mi.bits_per_pixel >= 24) {
                    log_info("VBE", "Found native Full HD VBE mode 0x%04X (1920x1080x%u, LFB 0x%08X)",
                             cand, mi.bits_per_pixel, mi.phys_base_ptr);
                    if (vbe_set_mode(cand) == 0) {
                        vbe_active = true;
                        vbe_memcpy(&active_mode, &mi, sizeof(vbe_mode_info_t));
                        if (out_selected_mode) vbe_memcpy(out_selected_mode, &mi, sizeof(vbe_mode_info_t));
                        if (out_mode_num) *out_mode_num = cand;
                        return 0;
                    }
                }
            }
        }
    }

    // Try standard candidate modes
    vbe_mode_info_t mode_info;
    for (uint32_t i = 0; i < num_candidates; i++) {
        uint16_t cand = candidate_modes[i];
        if (vbe_get_mode_info(cand, &mode_info) != 0) continue;

        // Check required capabilities:
        // bit 0: Mode supported by hardware
        // bit 4: Graphics mode (1)
        // bit 7: Linear Frame Buffer supported (1)
        if (!(mode_info.attributes & 0x0001)) continue;
        if (!(mode_info.attributes & 0x0010)) continue;
        if (!(mode_info.attributes & 0x0080)) continue;
        if (mode_info.phys_base_ptr == 0) continue;
        if (mode_info.x_res == 0 || mode_info.y_res == 0) continue;
        if (mode_info.bits_per_pixel < 16) continue;

        log_info("VBE", "Found compatible VBE mode 0x%04X: %ux%ux%u (Pitch %u B, LFB 0x%08X)",
                 cand, mode_info.x_res, mode_info.y_res, mode_info.bits_per_pixel,
                 mode_info.bytes_per_scanline, mode_info.phys_base_ptr);

        // Attempt mode switch
        if (vbe_set_mode(cand) == 0) {
            log_info("VBE", "Activated VBE Linear Framebuffer mode 0x%04X (%ux%ux%u)!",
                     cand, mode_info.x_res, mode_info.y_res, mode_info.bits_per_pixel);
            vbe_active = true;
            vbe_memcpy(&active_mode, &mode_info, sizeof(vbe_mode_info_t));
            if (out_selected_mode) {
                vbe_memcpy(out_selected_mode, &mode_info, sizeof(vbe_mode_info_t));
            }
            if (out_mode_num) {
                *out_mode_num = cand;
            }
            return 0;
        }
    }

    log_warn("VBE", "No supported VBE linear framebuffer mode could be activated, falling back to VGA text mode");
    vbe_active = false;
    return -1;
}
