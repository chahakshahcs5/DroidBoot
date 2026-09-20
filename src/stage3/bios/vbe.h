#ifndef VBE_H
#define VBE_H

#include <stdint.h>
#include <stdbool.h>

#define VBE_INFO_BLOCK_PHYS 0x7400
#define VBE_MODE_INFO_PHYS  0x7600

#pragma pack(push, 1)

typedef struct vbe_info_block {
    char     signature[4];          // "VESA" (or "VBE2" upon input)
    uint16_t version;               // VBE version (0x0200, 0x0300)
    uint32_t oem_string_ptr;        // Far pointer to OEM string
    uint32_t capabilities;          // Capabilities flags
    uint32_t video_mode_ptr;        // Far pointer to list of supported video modes
    uint16_t total_memory;          // Total memory in 64KB blocks
    uint16_t oem_software_rev;      // OEM software revision
    uint32_t oem_vendor_name_ptr;   // Far pointer to vendor name
    uint32_t oem_product_name_ptr;  // Far pointer to product name
    uint32_t oem_product_rev_ptr;   // Far pointer to product revision
    uint8_t  reserved[222];
    uint8_t  oem_data[256];
} vbe_info_block_t;

typedef struct vbe_mode_info {
    uint16_t attributes;            // Mode attributes (bit 0: supported, bit 4: graphics, bit 7: linear FB)
    uint8_t  win_a_attributes;
    uint8_t  win_b_attributes;
    uint16_t win_granularity;
    uint16_t win_size;
    uint16_t win_a_segment;
    uint16_t win_b_segment;
    uint32_t win_func_ptr;
    uint16_t bytes_per_scanline;    // Bytes per scanline (pitch)
    uint16_t x_res;                 // Horizontal resolution in pixels
    uint16_t y_res;                 // Vertical resolution in pixels
    uint8_t  x_char_size;
    uint8_t  y_char_size;
    uint8_t  number_of_planes;
    uint8_t  bits_per_pixel;        // Bits per pixel (16, 24, 32)
    uint8_t  number_of_banks;
    uint8_t  memory_model;          // Memory model (4: Packed pixel, 6: Direct Color)
    uint8_t  bank_size;
    uint8_t  number_of_image_pages;
    uint8_t  reserved1;
    uint8_t  red_mask_size;
    uint8_t  red_field_position;
    uint8_t  green_mask_size;
    uint8_t  green_field_position;
    uint8_t  blue_mask_size;
    uint8_t  blue_field_position;
    uint8_t  rsvd_mask_size;
    uint8_t  rsvd_field_position;
    uint8_t  direct_color_mode_info;
    uint32_t phys_base_ptr;         // 32-bit physical address of Linear Frame Buffer
    uint32_t offscreen_mem_offset;
    uint16_t offscreen_mem_size;
    uint8_t  reserved2[206];
} vbe_mode_info_t;

#pragma pack(pop)

// BIOS INT 10h real-mode thunk entry
extern uint16_t bios_int10_call(uint16_t ax, uint16_t bx, uint16_t cx, uint16_t dx,
                               uint16_t es, uint16_t di);

int vbe_get_controller_info(vbe_info_block_t *out_info);
int vbe_get_mode_info(uint16_t mode, vbe_mode_info_t *out_info);
int vbe_set_mode(uint16_t mode);
int vbe_setup_linear_framebuffer(vbe_mode_info_t *out_selected_mode, uint16_t *out_mode_num);

#endif // VBE_H
