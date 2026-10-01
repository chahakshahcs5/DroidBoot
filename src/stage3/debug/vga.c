#include "vga.h"
#include "font8x16.h"
#include "../bios/vbe.h"
#include "../../include/io.h"

// Legacy VGA text buffer dimensions (always 80x25 for QEMU/test compat)
#define VGA_TEXT_WIDTH  80
#define VGA_TEXT_HEIGHT 25
#define VGA_BUFFER ((volatile uint16_t *)0xB8000)

// Active console geometry (runtime-configurable)
static uint8_t console_cols = CONSOLE_DEFAULT_COLS;
static uint8_t console_rows = CONSOLE_DEFAULT_ROWS;

static uint8_t cursor_x = 0;
static uint8_t cursor_y = 0;
static uint8_t current_color = 0x07; // Light grey on black

// Shadow buffer in RAM for fast, flicker-free redraws.
// Sized to CONSOLE_MAX for static allocation; active area is [console_rows][console_cols].
static uint16_t vga_shadow[CONSOLE_MAX_ROWS][CONSOLE_MAX_COLS];


// VBE Linear Framebuffer parameters
static uint32_t fb_base = 0;
static uint32_t fb_width = 0;
static uint32_t fb_height = 0;
static uint32_t fb_pitch = 0;
static uint8_t  fb_bpp = 0;
static uint32_t fb_scale = 1;
static uint32_t fb_margin_x = 0;
static uint32_t fb_margin_y = 0;
static bool     fb_enabled = false;

// Precalculated row start pointers for fast VRAM access
static uint8_t *fb_row_ptrs[CONSOLE_MAX_ROWS * 16 * 2]; // max scanlines: rows * 16 * scale(2)

// Standard 16-color VGA to 24/32-bit RGB palette lookup table
static const uint32_t vga_to_rgb[16] = {
    0x000000, // 0: Black
    0x0000AA, // 1: Blue
    0x00AA00, // 2: Green
    0x00AAAA, // 3: Cyan
    0xAA0000, // 4: Red
    0xAA00AA, // 5: Magenta
    0xAA5500, // 6: Brown
    0xAAAAAA, // 7: Light Grey
    0x555555, // 8: Dark Grey
    0x5555FF, // 9: Light Blue
    0x55FF55, // 10: Light Green
    0x55FFFF, // 11: Light Cyan
    0xFF5555, // 12: Light Red
    0xFF55FF, // 13: Light Magenta
    0xFFFF55, // 14: Yellow
    0xFFFFFF  // 15: White
};

static void update_cursor(void) {
    // Update legacy VGA hardware cursor (clamped to 80x25)
    uint8_t cx = (cursor_x < VGA_TEXT_WIDTH) ? cursor_x : (VGA_TEXT_WIDTH - 1);
    uint8_t cy = (cursor_y < VGA_TEXT_HEIGHT) ? cursor_y : (VGA_TEXT_HEIGHT - 1);
    uint16_t pos = cy * VGA_TEXT_WIDTH + cx;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

// Precalculate row scanline pointers for fast VRAM access
static void fb_precalc_row_ptrs(void) {
    if (!fb_enabled || fb_base == 0) return;
    uint32_t total_scanlines = (uint32_t)console_rows * 16 * fb_scale;
    for (uint32_t sl = 0; sl < total_scanlines && sl < sizeof(fb_row_ptrs)/sizeof(fb_row_ptrs[0]); sl++) {
        fb_row_ptrs[sl] = (uint8_t *)fb_base + (fb_margin_y + sl) * fb_pitch + fb_margin_x * (fb_bpp / 8);
    }
}

// Recalculate margins, scale, and row pointers for current geometry
static void fb_recalc_geometry(void) {
    if (!fb_enabled) return;

    // Scale factor: 2x for high-res (>=1600x900), 1x otherwise
    if (fb_width >= 1600 && fb_height >= 900) {
        fb_scale = 2;
    } else {
        fb_scale = 1;
    }

    // Validate that requested geometry fits on screen; shrink if needed
    uint32_t console_pixel_w = (uint32_t)console_cols * 8 * fb_scale;
    uint32_t console_pixel_h = (uint32_t)console_rows * 16 * fb_scale;

    while (console_pixel_w > fb_width && console_cols > 40) {
        console_cols--;
        console_pixel_w = (uint32_t)console_cols * 8 * fb_scale;
    }
    while (console_pixel_h > fb_height && console_rows > 10) {
        console_rows--;
        console_pixel_h = (uint32_t)console_rows * 16 * fb_scale;
    }

    // Determine margins: fullscreen feel (tiny bezel) if >=90% coverage, else center
    uint32_t h_gap = fb_width - console_pixel_w;
    uint32_t v_gap = fb_height - console_pixel_h;

    // Center margins; enforce 4-pixel alignment so scanlines are always DWORD-aligned
    fb_margin_x = (h_gap / 2) & ~3;
    fb_margin_y = v_gap / 2;

    fb_precalc_row_ptrs();
}

// ============================================================================
// FRAMEBUFFER PIXEL / CHARACTER RENDERING
// ============================================================================

static inline void fb_put_pixel(uint32_t x, uint32_t y, uint32_t rgb) {
    if (x >= fb_width || y >= fb_height || fb_base == 0) return;
    uint8_t *pixel = (uint8_t *)fb_base + y * fb_pitch;
    if (fb_bpp == 32) {
        *(uint32_t *)(pixel + (x << 2)) = rgb;
    } else if (fb_bpp == 24) {
        uint8_t *p = pixel + x * 3;
        p[0] = (uint8_t)(rgb & 0xFF);         // B
        p[1] = (uint8_t)((rgb >> 8) & 0xFF);  // G
        p[2] = (uint8_t)((rgb >> 16) & 0xFF); // R
    } else if (fb_bpp == 16) {
        uint16_t r = (uint16_t)((rgb >> 19) & 0x1F);
        uint16_t g = (uint16_t)((rgb >> 10) & 0x3F);
        uint16_t b = (uint16_t)((rgb >> 3) & 0x1F);
        *(uint16_t *)(pixel + (x << 1)) = (uint16_t)((r << 11) | (g << 5) | b);
    }
}

static void fb_draw_char_cell(uint8_t col, uint8_t row, char c, uint8_t color_attr) {
    if (!fb_enabled || fb_base == 0) return;
    uint8_t fg = color_attr & 0x0F;
    uint8_t bg = (color_attr >> 4) & 0x0F;
    uint32_t fg_rgb = vga_to_rgb[fg];
    uint32_t bg_rgb = vga_to_rgb[bg];

    const uint8_t *glyph = &font_8x16[((uint8_t)c) * 16];
    uint32_t char_pixel_x = col * (8 * fb_scale);
    uint32_t char_byte_offset = char_pixel_x * (fb_bpp / 8);

    for (uint32_t r = 0; r < 16; r++) {
        uint8_t row_bits = glyph[r];
        for (uint32_t sy = 0; sy < fb_scale; sy++) {
            uint32_t sl = row * (16 * fb_scale) + r * fb_scale + sy;
            uint8_t *line_ptr;
            if (sl < sizeof(fb_row_ptrs)/sizeof(fb_row_ptrs[0]) && fb_row_ptrs[sl]) {
                line_ptr = fb_row_ptrs[sl] + char_byte_offset;
            } else {
                line_ptr = (uint8_t *)fb_base + (fb_margin_y + sl) * fb_pitch + fb_margin_x * (fb_bpp / 8) + char_byte_offset;
            }

            if (fb_bpp == 32) {
                uint32_t *d32 = (uint32_t *)line_ptr;
                if (fb_scale == 1) {
                    for (int b = 7; b >= 0; b--) {
                        *d32++ = (row_bits & (1 << b)) ? fg_rgb : bg_rgb;
                    }
                } else { // fb_scale == 2
                    for (int b = 7; b >= 0; b--) {
                        uint32_t color = (row_bits & (1 << b)) ? fg_rgb : bg_rgb;
                        *d32++ = color;
                        *d32++ = color;
                    }
                }
            } else if (fb_bpp == 24) {
                uint8_t *d8 = line_ptr;
                if (fb_scale == 1) {
                    for (int b = 7; b >= 0; b--) {
                        uint32_t color = (row_bits & (1 << b)) ? fg_rgb : bg_rgb;
                        *d8++ = (uint8_t)(color & 0xFF);
                        *d8++ = (uint8_t)((color >> 8) & 0xFF);
                        *d8++ = (uint8_t)((color >> 16) & 0xFF);
                    }
                } else { // fb_scale == 2
                    for (int b = 7; b >= 0; b--) {
                        uint32_t color = (row_bits & (1 << b)) ? fg_rgb : bg_rgb;
                        uint8_t blue = (uint8_t)(color & 0xFF);
                        uint8_t green = (uint8_t)((color >> 8) & 0xFF);
                        uint8_t red = (uint8_t)((color >> 16) & 0xFF);
                        *d8++ = blue; *d8++ = green; *d8++ = red;
                        *d8++ = blue; *d8++ = green; *d8++ = red;
                    }
                }
            } else if (fb_bpp == 16) {
                uint16_t *d16 = (uint16_t *)line_ptr;
                uint16_t fg16 = (uint16_t)(((fg_rgb >> 19) & 0x1F) << 11) |
                                (uint16_t)(((fg_rgb >> 10) & 0x3F) << 5) |
                                (uint16_t)((fg_rgb >> 3) & 0x1F);
                uint16_t bg16 = (uint16_t)(((bg_rgb >> 19) & 0x1F) << 11) |
                                (uint16_t)(((bg_rgb >> 10) & 0x3F) << 5) |
                                (uint16_t)((bg_rgb >> 3) & 0x1F);
                if (fb_scale == 1) {
                    for (int b = 7; b >= 0; b--) {
                        *d16++ = (row_bits & (1 << b)) ? fg16 : bg16;
                    }
                } else {
                    for (int b = 7; b >= 0; b--) {
                        uint16_t color = (row_bits & (1 << b)) ? fg16 : bg16;
                        *d16++ = color;
                        *d16++ = color;
                    }
                }
            }
        }
    }
}

static void fb_clear_screen(uint8_t bg_color) {
    if (!fb_enabled || fb_base == 0) return;
    uint32_t bg_rgb = vga_to_rgb[bg_color & 0x0F];

    // Fast path: if black (0x000000), 32-bit zero fill across entire framebuffer memory
    if (bg_rgb == 0) {
        uint32_t total_dwords = (fb_pitch * fb_height) / 4;
        uint32_t *p = (uint32_t *)fb_base;
        for (uint32_t i = 0; i < total_dwords; i++) {
            p[i] = 0;
        }
        return;
    }

    if (fb_bpp == 32) {
        uint32_t total_dwords = (fb_pitch >> 2) * fb_height;
        uint32_t *p = (uint32_t *)fb_base;
        for (uint32_t i = 0; i < total_dwords; i++) p[i] = bg_rgb;
    } else {
        for (uint32_t y = 0; y < fb_height; y++) {
            for (uint32_t x = 0; x < fb_width; x++) {
                fb_put_pixel(x, y, bg_rgb);
            }
        }
    }
}

// ============================================================================
// WRITE-ONLY SCANLINE REDRAW FROM RAM SHADOW BUFFER
// ============================================================================
// VRAM on PCIe MMIO is strictly Write-Only (reads stall the bus and return torn data).
// To achieve instantaneous, artifact-free scrolling, we assemble each scanline
// in a small L1 cache buffer in system RAM, then blast it sequentially to VRAM
// using 32-bit DWORD writes.

static uint8_t scanline_ram[CONSOLE_MAX_COLS * 8 * 2 * 4] __attribute__((aligned(16)));

static void fb_redraw_from_shadow(void) {
    if (!fb_enabled || fb_base == 0) return;

    uint32_t bytes_per_pixel = fb_bpp / 8;
    uint32_t row_pixels = (uint32_t)console_cols * 8 * fb_scale;
    uint32_t row_bytes = row_pixels * bytes_per_pixel;
    uint32_t row_dwords = row_bytes / 4;
    uint32_t remainder_bytes = row_bytes % 4;

    for (uint8_t y = 0; y < console_rows; y++) {
        // Render font scanlines r = 0..15 across all console_cols for pristine, artifact-free display
        for (uint32_t r = 0; r < 16; r++) {
            // 1. Build this scanline in RAM (L1 cache)
            if (fb_bpp == 32) {
                uint32_t *buf32 = (uint32_t *)scanline_ram;
                uint32_t px = 0;
                for (uint32_t x = 0; x < console_cols; x++) {
                    uint16_t entry = vga_shadow[y][x];
                    char c = (char)(entry & 0xFF);
                    uint8_t attr = (uint8_t)(entry >> 8);
                    uint32_t fg = vga_to_rgb[attr & 0x0F];
                    uint32_t bg = vga_to_rgb[(attr >> 4) & 0x0F];
                    uint8_t bits = font_8x16[((uint8_t)c) * 16 + r];

                    if (bits == 0) {
                        int count = (fb_scale == 1) ? 8 : 16;
                        for (int k = 0; k < count; k++) buf32[px++] = bg;
                    } else if (fb_scale == 1) {
                        for (int b = 7; b >= 0; b--) {
                            buf32[px++] = (bits & (1 << b)) ? fg : bg;
                        }
                    } else { // fb_scale == 2
                        for (int b = 7; b >= 0; b--) {
                            uint32_t col = (bits & (1 << b)) ? fg : bg;
                            buf32[px++] = col;
                            buf32[px++] = col;
                        }
                    }
                }
            } else if (fb_bpp == 24) {
                uint8_t *buf8 = scanline_ram;
                uint32_t idx = 0;
                for (uint32_t x = 0; x < console_cols; x++) {
                    uint16_t entry = vga_shadow[y][x];
                    char c = (char)(entry & 0xFF);
                    uint8_t attr = (uint8_t)(entry >> 8);
                    uint32_t fg = vga_to_rgb[attr & 0x0F];
                    uint32_t bg = vga_to_rgb[(attr >> 4) & 0x0F];
                    uint8_t bits = font_8x16[((uint8_t)c) * 16 + r];

                    uint8_t bg_b = (uint8_t)(bg & 0xFF);
                    uint8_t bg_g = (uint8_t)((bg >> 8) & 0xFF);
                    uint8_t bg_r = (uint8_t)((bg >> 16) & 0xFF);

                    if (bits == 0) {
                        int count = (fb_scale == 1) ? 8 : 16;
                        for (int k = 0; k < count; k++) {
                            buf8[idx++] = bg_b;
                            buf8[idx++] = bg_g;
                            buf8[idx++] = bg_r;
                        }
                    } else if (fb_scale == 1) {
                        uint8_t fg_b = (uint8_t)(fg & 0xFF);
                        uint8_t fg_g = (uint8_t)((fg >> 8) & 0xFF);
                        uint8_t fg_r = (uint8_t)((fg >> 16) & 0xFF);
                        for (int b = 7; b >= 0; b--) {
                            if (bits & (1 << b)) {
                                buf8[idx++] = fg_b; buf8[idx++] = fg_g; buf8[idx++] = fg_r;
                            } else {
                                buf8[idx++] = bg_b; buf8[idx++] = bg_g; buf8[idx++] = bg_r;
                            }
                        }
                    } else { // fb_scale == 2
                        uint8_t fg_b = (uint8_t)(fg & 0xFF);
                        uint8_t fg_g = (uint8_t)((fg >> 8) & 0xFF);
                        uint8_t fg_r = (uint8_t)((fg >> 16) & 0xFF);
                        for (int b = 7; b >= 0; b--) {
                            if (bits & (1 << b)) {
                                buf8[idx++] = fg_b; buf8[idx++] = fg_g; buf8[idx++] = fg_r;
                                buf8[idx++] = fg_b; buf8[idx++] = fg_g; buf8[idx++] = fg_r;
                            } else {
                                buf8[idx++] = bg_b; buf8[idx++] = bg_g; buf8[idx++] = bg_r;
                                buf8[idx++] = bg_b; buf8[idx++] = bg_g; buf8[idx++] = bg_r;
                            }
                        }
                    }
                }
            } else if (fb_bpp == 16) {
                uint16_t *buf16 = (uint16_t *)scanline_ram;
                uint32_t px = 0;
                for (uint32_t x = 0; x < console_cols; x++) {
                    uint16_t entry = vga_shadow[y][x];
                    char c = (char)(entry & 0xFF);
                    uint8_t attr = (uint8_t)(entry >> 8);
                    uint32_t fg_rgb = vga_to_rgb[attr & 0x0F];
                    uint32_t bg_rgb = vga_to_rgb[(attr >> 4) & 0x0F];
                    uint16_t fg16 = (uint16_t)(((fg_rgb >> 19) & 0x1F) << 11) |
                                    (uint16_t)(((fg_rgb >> 10) & 0x3F) << 5) |
                                    (uint16_t)((fg_rgb >> 3) & 0x1F);
                    uint16_t bg16 = (uint16_t)(((bg_rgb >> 19) & 0x1F) << 11) |
                                    (uint16_t)(((bg_rgb >> 10) & 0x3F) << 5) |
                                    (uint16_t)((bg_rgb >> 3) & 0x1F);
                    uint8_t bits = font_8x16[((uint8_t)c) * 16 + r];

                    if (bits == 0) {
                        int count = (fb_scale == 1) ? 8 : 16;
                        for (int k = 0; k < count; k++) buf16[px++] = bg16;
                    } else if (fb_scale == 1) {
                        for (int b = 7; b >= 0; b--) {
                            buf16[px++] = (bits & (1 << b)) ? fg16 : bg16;
                        }
                    } else { // fb_scale == 2
                        for (int b = 7; b >= 0; b--) {
                            uint16_t col = (bits & (1 << b)) ? fg16 : bg16;
                            buf16[px++] = col;
                            buf16[px++] = col;
                        }
                    }
                }
            }

            // 2. Blast assembled scanline directly to VRAM via DWORD writes
            for (uint32_t s = 0; s < fb_scale; s++) {
                uint32_t sl = y * (16 * fb_scale) + r * fb_scale + s;
                uint8_t *dst;
                if (sl < sizeof(fb_row_ptrs)/sizeof(fb_row_ptrs[0]) && fb_row_ptrs[sl]) {
                    dst = fb_row_ptrs[sl];
                } else {
                    dst = (uint8_t *)fb_base + (fb_margin_y + sl) * fb_pitch + fb_margin_x * bytes_per_pixel;
                }

                uint32_t *d32 = (uint32_t *)dst;
                const uint32_t *s32 = (const uint32_t *)scanline_ram;
                for (uint32_t dw = 0; dw < row_dwords; dw++) {
                    d32[dw] = s32[dw];
                }
                for (uint32_t rb = 0; rb < remainder_bytes; rb++) {
                    dst[row_dwords * 4 + rb] = scanline_ram[row_dwords * 4 + rb];
                }
            }
        }
    }
}

// ============================================================================
// PUBLIC API
// ============================================================================

void vga_init(void) {
    current_color = (VGA_COLOR_BLACK << 4) | VGA_COLOR_LIGHT_GREY;

    // 1. Initialize legacy text mode buffer at 0xB8000 (always 80x25)
    uint16_t blank = ((uint16_t)current_color << 8) | ' ';
    for (int i = 0; i < VGA_TEXT_WIDTH * VGA_TEXT_HEIGHT; i++) {
        VGA_BUFFER[i] = blank;
    }

    // 2. Initialize VBE Linear Framebuffer mode via INT 10h real-mode thunk
    vbe_mode_info_t selected_mode;
    uint16_t mode_num = 0;
    if (vbe_setup_linear_framebuffer(&selected_mode, &mode_num) == 0) {
        fb_base = selected_mode.phys_base_ptr;
        fb_width = selected_mode.x_res;
        fb_height = selected_mode.y_res;
        fb_pitch = selected_mode.bytes_per_scanline;
        fb_bpp = selected_mode.bits_per_pixel;

        fb_enabled = true;
        console_cols = CONSOLE_DEFAULT_COLS;
        console_rows = CONSOLE_DEFAULT_ROWS;
        if (console_cols > CONSOLE_MAX_COLS) console_cols = CONSOLE_MAX_COLS;
        if (console_rows > CONSOLE_MAX_ROWS) console_rows = CONSOLE_MAX_ROWS;

        fb_recalc_geometry();
        fb_clear_screen(VGA_COLOR_BLACK);
    } else {
        fb_enabled = false;
        // In text-only fallback, strictly 80x25
        console_cols = VGA_TEXT_WIDTH;
        console_rows = VGA_TEXT_HEIGHT;
    }

    // Initialize shadow buffer
    for (uint8_t y = 0; y < console_rows; y++) {
        for (uint8_t x = 0; x < console_cols; x++) {
            vga_shadow[y][x] = blank;
        }
    }

    cursor_x = 0;
    cursor_y = 0;
    update_cursor();
}

void vga_set_color(uint8_t fg, uint8_t bg) {
    current_color = (bg << 4) | (fg & 0x0F);
}

void vga_clear(void) {
    uint16_t blank = ((uint16_t)current_color << 8) | ' ';

    // Clear legacy VGA text buffer
    for (int i = 0; i < VGA_TEXT_WIDTH * VGA_TEXT_HEIGHT; i++) {
        VGA_BUFFER[i] = blank;
    }

    // Clear shadow buffer
    for (uint8_t y = 0; y < console_rows; y++) {
        for (uint8_t x = 0; x < console_cols; x++) {
            vga_shadow[y][x] = blank;
        }
    }

    cursor_x = 0;
    cursor_y = 0;
    update_cursor();

    if (fb_enabled) {
        fb_clear_screen((current_color >> 4) & 0x0F);
    }
}

static void scroll(void) {
    uint16_t blank = ((uint16_t)current_color << 8) | ' ';

    if (!fb_enabled) {
        for (int y = 0; y < VGA_TEXT_HEIGHT - 1; y++) {
            for (int x = 0; x < VGA_TEXT_WIDTH; x++) {
                VGA_BUFFER[y * VGA_TEXT_WIDTH + x] = VGA_BUFFER[(y + 1) * VGA_TEXT_WIDTH + x];
            }
        }
        for (int x = 0; x < VGA_TEXT_WIDTH; x++) {
            VGA_BUFFER[(VGA_TEXT_HEIGHT - 1) * VGA_TEXT_WIDTH + x] = blank;
        }
        cursor_y = VGA_TEXT_HEIGHT - 1;
        update_cursor();
        return;
    }

    // Scroll shadow buffer (always uses console_cols/rows)
    for (uint8_t y = 0; y < console_rows - 1; y++) {
        for (uint8_t x = 0; x < console_cols; x++) {
            vga_shadow[y][x] = vga_shadow[y + 1][x];
        }
    }
    for (uint8_t x = 0; x < console_cols; x++) {
        vga_shadow[console_rows - 1][x] = blank;
    }

    cursor_y = console_rows - 1;

    // High-performance write-only active-region scanline redraw
    fb_redraw_from_shadow();
    update_cursor();
}

void vga_putchar(char c) {
    if (!fb_enabled) {
        // Standard fast 80x25 text mode path
        if (c == '\n') {
            cursor_x = 0;
            cursor_y++;
        } else if (c == '\r') {
            cursor_x = 0;
        } else if (c == '\t') {
            cursor_x = (cursor_x + 4) & ~3;
        } else if (c == '\b') {
            if (cursor_x > 0) {
                cursor_x--;
                VGA_BUFFER[cursor_y * VGA_TEXT_WIDTH + cursor_x] = ((uint16_t)current_color << 8) | ' ';
            }
        } else {
            VGA_BUFFER[cursor_y * VGA_TEXT_WIDTH + cursor_x] = ((uint16_t)current_color << 8) | (uint8_t)c;
            cursor_x++;
        }

        if (cursor_x >= VGA_TEXT_WIDTH) {
            cursor_x = 0;
            cursor_y++;
        }
        if (cursor_y >= VGA_TEXT_HEIGHT) {
            scroll();
        }
        update_cursor();
        return;
    }

    // VBE Linear Framebuffer mode path
    static bool just_wrapped = false;

    if (c == '\n') {
        if (just_wrapped) {
            just_wrapped = false;
        } else {
            cursor_x = 0;
            cursor_y++;
        }
    } else if (c == '\r') {
        cursor_x = 0;
        just_wrapped = false;
    } else if (c == '\t') {
        cursor_x = (cursor_x + 4) & ~3;
        just_wrapped = false;
    } else if (c == '\b') {
        just_wrapped = false;
        if (cursor_x > 0) {
            cursor_x--;
            vga_shadow[cursor_y][cursor_x] = ((uint16_t)current_color << 8) | ' ';
            fb_draw_char_cell(cursor_x, cursor_y, ' ', current_color);
        }
    } else {
        just_wrapped = false;
        vga_shadow[cursor_y][cursor_x] = ((uint16_t)current_color << 8) | (uint8_t)c;
        fb_draw_char_cell(cursor_x, cursor_y, c, current_color);
        cursor_x++;
    }

    if (cursor_x >= console_cols) {
        cursor_x = 0;
        cursor_y++;
        just_wrapped = true;
    }
    if (cursor_y >= console_rows) {
        scroll();
    }
    update_cursor();
}

void vga_puts(const char *s) {
    while (*s) {
        vga_putchar(*s++);
    }
}

void vga_set_geometry(uint8_t cols, uint8_t rows) {
    if (cols < 40) cols = 40;
    if (rows < 10) rows = 10;
    if (cols > CONSOLE_MAX_COLS) cols = CONSOLE_MAX_COLS;
    if (rows > CONSOLE_MAX_ROWS) rows = CONSOLE_MAX_ROWS;

    console_cols = cols;
    console_rows = rows;

    if (fb_enabled) {
        fb_recalc_geometry();
    } else {
        // Text-only mode: clamp to 80x25
        if (console_cols > VGA_TEXT_WIDTH) console_cols = VGA_TEXT_WIDTH;
        if (console_rows > VGA_TEXT_HEIGHT) console_rows = VGA_TEXT_HEIGHT;
    }

    vga_clear();
}

void vga_get_geometry(uint8_t *out_cols, uint8_t *out_rows) {
    if (out_cols) *out_cols = console_cols;
    if (out_rows) *out_rows = console_rows;
}

uint8_t vga_get_cols(void) {
    return console_cols;
}

uint8_t vga_get_rows(void) {
    return console_rows;
}

