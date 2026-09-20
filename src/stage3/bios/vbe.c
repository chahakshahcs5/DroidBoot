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

int vbe_set_mode(uint16_t mode) {
    // Bit 14 (0x4000) = Enable Linear Frame Buffer mode
    // Bit 15 (0x8000) = Preserve display memory (do not clear), leave 0 to clear
    uint16_t bx = (mode & 0x01FF) | 0x4000;
    uint16_t ax = bios_int10_call(0x4F02, bx, 0, 0, 0, 0);
    if (ax != 0x004F) {
        log_error("VBE", "Set Video Mode 0x%04X failed (AX=0x%04X)", mode, ax);
        return -1;
    }
    return 0;
}

int vbe_setup_linear_framebuffer(vbe_mode_info_t *out_selected_mode, uint16_t *out_mode_num) {
    // List of preferred standard VBE graphics modes
    static const uint16_t candidate_modes[] = {
        0x0118, // 1024x768x32/24 (Optimal balance: supported on all laptop BIOSes & external monitors)
        0x0115, // 800x600x32/24
        0x011B, // 1280x1024x32/24
        0x0112, // 640x480x32/24
        0x0117, // 1024x768x16
        0x0114  // 800x600x16
    };
    uint32_t num_candidates = sizeof(candidate_modes) / sizeof(candidate_modes[0]);

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
            if (out_selected_mode) {
                vbe_memcpy(out_selected_mode, &mode_info, sizeof(vbe_mode_info_t));
            }
            if (out_mode_num) {
                *out_mode_num = cand;
            }
            return 0;
        }
    }

    log_error("VBE", "No supported VBE linear framebuffer mode could be activated!");
    return -1;
}
