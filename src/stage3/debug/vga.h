#ifndef VGA_H
#define VGA_H

#include <stdint.h>

// ============================================================================
// CONFIGURABLE CONSOLE GEOMETRY
// ============================================================================
// Change these values to adjust the console size on the VBE framebuffer.
// The font is 8x16 pixels. At 2x scale on 1080p (1920x1080):
//   Max columns = 1920 / (8*2) = 120,  Max rows = 1080 / (16*2) = 33
//
// Preset suggestions:
//   FULLSCREEN on 1080p:  120 cols x 33 rows  (edge-to-edge, tiny 8px bezel)
//   WIDESCREEN:           100 cols x 28 rows  (comfortable reading)
//   CLASSIC CENTERED:      80 cols x 25 rows  (original VGA look)
//
#define CONSOLE_DEFAULT_COLS  120
#define CONSOLE_DEFAULT_ROWS   33

// Maximum supported geometry (for static shadow buffer allocation).
// Do not exceed: 240 cols x 67 rows (would require >16KB shadow buffer).
#define CONSOLE_MAX_COLS  240
#define CONSOLE_MAX_ROWS   67

// ============================================================================
// VGA COLOR CONSTANTS
// ============================================================================
#define VGA_COLOR_BLACK         0
#define VGA_COLOR_BLUE          1
#define VGA_COLOR_GREEN         2
#define VGA_COLOR_CYAN          3
#define VGA_COLOR_RED           4
#define VGA_COLOR_MAGENTA       5
#define VGA_COLOR_BROWN         6
#define VGA_COLOR_LIGHT_GREY    7
#define VGA_COLOR_DARK_GREY     8
#define VGA_COLOR_LIGHT_BLUE    9
#define VGA_COLOR_LIGHT_GREEN   10
#define VGA_COLOR_LIGHT_CYAN    11
#define VGA_COLOR_LIGHT_RED     12
#define VGA_COLOR_LIGHT_MAGENTA 13
#define VGA_COLOR_YELLOW        14
#define VGA_COLOR_WHITE         15

// ============================================================================
// VGA CONSOLE API
// ============================================================================
void vga_init(void);
void vga_clear(void);
void vga_set_color(uint8_t fg, uint8_t bg);
void vga_putchar(char c);
void vga_puts(const char *s);

// Runtime geometry configuration (takes effect immediately, clears screen)
void vga_set_geometry(uint8_t cols, uint8_t rows);
void vga_get_geometry(uint8_t *out_cols, uint8_t *out_rows);

// Read-only accessors for current geometry
uint8_t vga_get_cols(void);
uint8_t vga_get_rows(void);

#endif // VGA_H
