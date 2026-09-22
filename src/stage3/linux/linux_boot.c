#include "linux_boot.h"
#include "../core/printf.h"
#include "../memory/memory.h"
#include "../../include/io.h"
#include "../debug/vga.h"
#include "../debug/disk_log.h"

// 66-byte self-contained relocation trampoline for 32-bit Linux handoff
// Relocates kernel image to 0x00100000, establishes safe low-memory stack (0x0007FFF0),
// and transfers control directly via jmp *%eax without writing to the stack or clobbering RAM.
static const uint8_t trampoline_template[] = {
    0xFA,                               // cli
    0xFC,                               // cld
    0x8B, 0x74, 0x24, 0x04,             // mov esi, [esp+4]   (src)
    0x8B, 0x7C, 0x24, 0x08,             // mov edi, [esp+8]   (dst)
    0x8B, 0x4C, 0x24, 0x0C,             // mov ecx, [esp+12]  (len in bytes)
    0x83, 0xC1, 0x03,                   // add ecx, 3
    0xC1, 0xE9, 0x02,                   // shr ecx, 2         (len in dwords)
    0x8B, 0x54, 0x24, 0x10,             // mov edx, [esp+16]  (params)
    0x8B, 0x5C, 0x24, 0x14,             // mov ebx, [esp+20]  (entry)
    0xF3, 0xA5,                         // rep movsd          (fast 32-bit move)
    0xB8, 0x10, 0x00, 0x00, 0x00,       // mov eax, 0x00000010
    0x8E, 0xD8,                         // mov ds, eax
    0x8E, 0xC0,                         // mov es, eax
    0x8E, 0xE0,                         // mov fs, eax
    0x8E, 0xE8,                         // mov gs, eax
    0x8E, 0xD0,                         // mov ss, eax
    0x89, 0xD6,                         // mov esi, edx       (ESI = params)
    0xBC, 0xF0, 0xFF, 0x07, 0x00,       // mov esp, 0x0007FFF0(safe low-memory stack)
    0x89, 0xD8,                         // mov eax, ebx       (EAX = entry point)
    0x31, 0xDB,                         // xor ebx, ebx       (EBX = 0)
    0x31, 0xC9,                         // xor ecx, ecx       (ECX = 0)
    0x31, 0xD2,                         // xor edx, edx       (EDX = 0)
    0x31, 0xED,                         // xor ebp, ebp       (EBP = 0)
    0x31, 0xFF,                         // xor edi, edi       (EDI = 0)
    0xFF, 0xE0                          // jmp *%eax          (direct jump to kernel entry, NO STACK WRITES!)
};

static uint32_t kstrlen(const char *s) {
    uint32_t len = 0;
    while (s && s[len]) len++;
    return len;
}

static void kmemcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

static void kmemset(void *dst, uint8_t val, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) {
        d[i] = val;
    }
}

int linux_check_kernel_image(const void *image_buf, uint32_t size, linux_kernel_info_t *out_info) {
    if (!image_buf || size < 1024) {
        log_error("LINUX", "Image buffer too small for bzImage header (%u bytes)", size);
        return -1;
    }

    const uint8_t *bytes = (const uint8_t *)image_buf;
    const linux_setup_header_t *hdr = (const linux_setup_header_t *)(bytes + 0x1F1);

    if (hdr->header != LINUX_HDRS_MAGIC) {
        log_error("LINUX", "Invalid kernel magic: 0x%08X (expected 'HdrS' 0x%08X)",
                  hdr->header, LINUX_HDRS_MAGIC);
        return -2;
    }

    if (hdr->version < 0x0200) {
        log_error("LINUX", "Kernel protocol version 0x%04X too old (require >= 0x0200)",
                  hdr->version);
        return -3;
    }

    uint32_t setup_sects = hdr->setup_sects ? hdr->setup_sects : 4;
    uint32_t realmode_sectors = setup_sects + 1;
    uint32_t realmode_size = realmode_sectors * 512;
    uint32_t protected_mode_offset = realmode_size;
    uint32_t protected_mode_size = size > realmode_size ? (size - realmode_size) : 0;
    uint32_t code32_start = hdr->code32_start ? hdr->code32_start : LINUX_KERNEL_LOAD_PHYS;

    log_info("LINUX", "Linux bzImage Detected:");
    log_info("LINUX", "  Protocol Version : %u.%u (0x%04X)",
             (hdr->version >> 8) & 0xFF, hdr->version & 0xFF, hdr->version);
    log_info("LINUX", "  Setup Sectors    : %u (Realmode Size: %u bytes)",
             setup_sects, realmode_size);
    log_info("LINUX", "  Protected Size   : %u bytes (Offset: 0x%08X)",
             protected_mode_size, protected_mode_offset);
    log_info("LINUX", "  32-bit Entry     : 0x%08X", code32_start);

    if (out_info) {
        out_info->realmode_sectors = realmode_sectors;
        out_info->realmode_size = realmode_size;
        out_info->protected_mode_offset = protected_mode_offset;
        out_info->protected_mode_size = protected_mode_size;
        out_info->code32_start = code32_start;
        out_info->protocol_version = hdr->version;
    }

    return 0;
}

int linux_prepare_boot_params(const void *kernel_image, uint32_t kernel_size,
                               const void *initrd_buf, uint32_t initrd_size,
                               const char *cmdline, boot_info_t *boot_info,
                               const vbe_mode_info_t *vbe_mode,
                               uint32_t ram_iso_addr, uint32_t ram_iso_size,
                               linux_boot_params_t *out_params) {
    (void)kernel_size;
    if (!kernel_image || !out_params) return -1;

    // Zero entire boot_params structure (4096 bytes)
    kmemset(out_params, 0, sizeof(linux_boot_params_t));

    // Copy kernel setup_header
    const uint8_t *bytes = (const uint8_t *)kernel_image;
    const linux_setup_header_t *src_hdr = (const linux_setup_header_t *)(bytes + 0x1F1);
    kmemcpy(&out_params->hdr, src_hdr, sizeof(linux_setup_header_t));

    // Fill in bootloader identification and flags
    out_params->hdr.type_of_loader = LINUX_BOOT_LOADER_TYPE;
    out_params->hdr.loadflags |= LINUX_LOADFLAGS_LOADED_HIGH | LINUX_LOADFLAGS_CAN_USE_HEAP;
    out_params->hdr.heap_end_ptr = 0x9000;

    // Setup screen_info: VBE Linear Framebuffer or fallback to 80x25 VGA text mode
    linux_screen_info_t *si = &out_params->screen_info;
    if (vbe_mode && vbe_mode->phys_base_ptr != 0) {
        si->orig_video_isVGA = VIDEO_TYPE_VLFB; // 0x23: VBE Linear Framebuffer
        si->orig_video_mode = 0x18;             // VBE mode flag
        si->orig_video_cols = vbe_mode->x_res / 8;
        si->orig_video_lines = vbe_mode->y_res / 16;
        si->orig_video_points = 16;
        si->lfb_width = vbe_mode->x_res;
        si->lfb_height = vbe_mode->y_res;
        si->lfb_depth = vbe_mode->bits_per_pixel;
        si->lfb_base = vbe_mode->phys_base_ptr;
        si->lfb_size = (vbe_mode->y_res * vbe_mode->bytes_per_scanline + 65535) / 65536;
        si->lfb_linelength = vbe_mode->bytes_per_scanline;
        si->red_size = vbe_mode->red_mask_size ? vbe_mode->red_mask_size : 8;
        si->red_pos = vbe_mode->red_field_position ? vbe_mode->red_field_position : 16;
        si->green_size = vbe_mode->green_mask_size ? vbe_mode->green_mask_size : 8;
        si->green_pos = vbe_mode->green_field_position ? vbe_mode->green_field_position : 8;
        si->blue_size = vbe_mode->blue_mask_size ? vbe_mode->blue_mask_size : 8;
        si->blue_pos = vbe_mode->blue_field_position ? vbe_mode->blue_field_position : 0;
        si->rsvd_size = vbe_mode->rsvd_mask_size ? vbe_mode->rsvd_mask_size : 8;
        si->rsvd_pos = vbe_mode->rsvd_field_position ? vbe_mode->rsvd_field_position : 24;
        si->vesa_attributes = vbe_mode->attributes;
        si->capabilities = 0;

        log_info("LINUX", "Configured VESA Linear Framebuffer screen_info: %ux%ux%u @ 0x%08X (Pitch %u B)",
                 si->lfb_width, si->lfb_height, si->lfb_depth, si->lfb_base, si->lfb_linelength);
    } else {
        // Fallback: standard 80x25 VGA text mode
        si->orig_x = 0;
        si->orig_y = 0;
        si->orig_video_page = 0;
        si->orig_video_mode = 3;
        si->orig_video_cols = 80;
        si->orig_video_lines = 25;
        si->orig_video_isVGA = VIDEO_TYPE_VGAC; // 0x22
        si->orig_video_points = 16;
    }

    // Fill in alt_mem_k from usable RAM
    uint32_t mem_k = (uint32_t)(memory_get_total_usable() / 1024);
    if (mem_k > 0) {
        out_params->alt_mem_k = (mem_k > 0x100000) ? 0x100000 : mem_k;
    }

    // Setup command line
    if (cmdline && *cmdline) {
        uint32_t clen = kstrlen(cmdline);
        char *cmdline_dst = (char *)LINUX_CMDLINE_PHYS;
        kmemcpy(cmdline_dst, cmdline, clen + 1);
        out_params->hdr.cmd_line_ptr = LINUX_CMDLINE_PHYS;
        out_params->hdr.cmdline_size = clen + 1;
        log_info("LINUX", "Command Line attached at 0x%08X: \"%s\"",
                 LINUX_CMDLINE_PHYS, cmdline);
    }

    // Setup initramfs
    if (initrd_buf && initrd_size > 0) {
        out_params->hdr.ramdisk_image = (uint32_t)initrd_buf;
        out_params->hdr.ramdisk_size = initrd_size;
        log_info("LINUX", "Initramfs attached at 0x%08X (%u bytes)",
                 (uint32_t)initrd_buf, initrd_size);
    }

    // Transfer E820 memory map from boot_info, and reserve in-RAM ISO region if active
    if (boot_info && boot_info->e820_count > 0) {
        e820_entry_t *src_e820 = (e820_entry_t *)boot_info->e820_map_addr;
        uint32_t count = boot_info->e820_count;
        if (count > 120) count = 120;

        uint32_t dst_idx = 0;
        uint64_t iso_start = ram_iso_addr;
        uint64_t iso_len_aligned = (ram_iso_size > 0) ? ((ram_iso_size + 0x1FFFFF) & ~0x1FFFFFULL) : 0;
        uint64_t iso_end = iso_start + iso_len_aligned;
        bool iso_reserved = false;

        for (uint32_t i = 0; i < count && dst_idx < 126; i++) {
            uint64_t base = src_e820[i].base;
            uint64_t length = src_e820[i].length;
            uint32_t type = src_e820[i].type;
            uint64_t end = base + length;

            // Check if this USABLE entry contains the in-RAM ISO
            if (ram_iso_addr > 0 && ram_iso_size > 0 && !iso_reserved &&
                type == 1 && base <= iso_start && end >= iso_end) {

                // 1. Portion before ISO (if any)
                if (iso_start > base) {
                    out_params->e820_table[dst_idx].addr = base;
                    out_params->e820_table[dst_idx].size = iso_start - base;
                    out_params->e820_table[dst_idx].type = 1; // USABLE
                    dst_idx++;
                }

                // 2. ISO memory region -> Marked RESERVED (Type 2) so kernel never overwrites it!
                out_params->e820_table[dst_idx].addr = iso_start;
                out_params->e820_table[dst_idx].size = iso_end - iso_start;
                out_params->e820_table[dst_idx].type = 2; // RESERVED
                log_info("LINUX", "E820: Reserved In-RAM ISO: 0x%08X - 0x%08X (%u MB, Type 2 RESERVED)",
                         (uint32_t)iso_start, (uint32_t)iso_end, (uint32_t)(iso_len_aligned >> 20));
                dst_idx++;

                // 3. Portion after ISO (if any)
                if (end > iso_end) {
                    out_params->e820_table[dst_idx].addr = iso_end;
                    out_params->e820_table[dst_idx].size = end - iso_end;
                    out_params->e820_table[dst_idx].type = 1; // USABLE
                    dst_idx++;
                }

                iso_reserved = true;
            } else {
                out_params->e820_table[dst_idx].addr = base;
                out_params->e820_table[dst_idx].size = length;
                out_params->e820_table[dst_idx].type = type;
                dst_idx++;
            }
        }
        out_params->e820_entries = (uint8_t)dst_idx;
        log_info("LINUX", "Transferred %u E820 memory entries to boot_params (ISO protected)", dst_idx);
    }

    log_info("LINUX", "Prepared Linux boot_params at 0x%08X", (uint32_t)out_params);
#if IS_DEBUG_BUILD
    log_debug("LINUX", "Boot Params Dump: Proto=0x%04X Loader=0x%02X LoadFlags=0x%02X Code32=0x%08X",
              out_params->hdr.version, out_params->hdr.type_of_loader, out_params->hdr.loadflags, out_params->hdr.code32_start);
    log_debug("LINUX", "CmdlinePtr=0x%08X RamdiskPtr=0x%08X RamdiskSize=%u B AltMem=%u KB",
              out_params->hdr.cmd_line_ptr, out_params->hdr.ramdisk_image, out_params->hdr.ramdisk_size, out_params->alt_mem_k);
#endif
    return 0;
}

void linux_setup_mbft(uint32_t iso_phys_addr, uint32_t iso_size) {
    if (iso_phys_addr == 0 || iso_size == 0) return;

    // Deploy mBFT at standard ACPI/ROM scan area 0x000E0000
    // memdiskfind scans 0x80000..0xA0000 and 0xE0000..0x100000
    linux_mbft_t *mbft = (linux_mbft_t *)0x000E0000;
    kmemset(mbft, 0, sizeof(linux_mbft_t));

    // ACPI Header
    mbft->acpi.signature[0] = 'm';
    mbft->acpi.signature[1] = 'B';
    mbft->acpi.signature[2] = 'F';
    mbft->acpi.signature[3] = 'T';
    mbft->acpi.length = sizeof(linux_mbft_t);
    mbft->acpi.revision = 1;
    mbft->acpi.oem_id[0] = 'S'; mbft->acpi.oem_id[1] = 'Y'; mbft->acpi.oem_id[2] = 'S';
    mbft->acpi.oem_id[3] = 'L'; mbft->acpi.oem_id[4] = 'N'; mbft->acpi.oem_id[5] = 'X';
    mbft->acpi.oem_table_id[0] = 'M'; mbft->acpi.oem_table_id[1] = 'E'; mbft->acpi.oem_table_id[2] = 'M';
    mbft->acpi.oem_table_id[3] = 'D'; mbft->acpi.oem_table_id[4] = 'I'; mbft->acpi.oem_table_id[5] = 'S';
    mbft->acpi.oem_table_id[6] = 'K'; mbft->acpi.oem_table_id[7] = ' ';
    mbft->acpi.oem_revision = 1;

    // MEMDISK Info (MDI)
    mbft->mdi.bytes = sizeof(mbft->mdi);
    mbft->mdi.version_minor = 0;
    mbft->mdi.version_major = 1;
    mbft->mdi.diskbuf = iso_phys_addr;
    mbft->mdi.disksize = (iso_size + 2047) / 2048; // CD/ISO sector count
    mbft->mdi.olddosmem = 640;
    mbft->mdi.bootloaderid = 0x30;
    mbft->mdi.sector_shift = 11; // 2048-byte CD sectors

    // Checksum calculation (sum over length == 0)
    uint8_t *bytes = (uint8_t *)mbft;
    uint8_t csum = 0;
    for (uint32_t i = 0; i < sizeof(linux_mbft_t); i++) {
        csum += bytes[i];
    }
    mbft->acpi.checksum = (uint8_t)(0x100 - csum);

    log_info("LINUX", "Installed mBFT table at 0x000E0000 (diskbuf=0x%08X, size=%u MB, csum=0x%02X)",
             iso_phys_addr, iso_size / 1024 / 1024, mbft->acpi.checksum);
}


void linux_boot_jump(uint32_t kernel_source_addr, uint32_t kernel_target_addr,
                    uint32_t kernel_size, uint32_t boot_params_addr,
                    uint32_t entry_point) {
    log_info("LINUX", "Deploying relocation trampoline to 0x%08X...", LINUX_TRAMPOLINE_PHYS);
    kmemcpy((void *)LINUX_TRAMPOLINE_PHYS, trampoline_template, sizeof(trampoline_template));

    log_info("LINUX", "Executing trampoline: Relocating %u B from 0x%08X to 0x%08X...",
             kernel_size, kernel_source_addr, kernel_target_addr);
    log_info("LINUX", "Handoff to code32_start at 0x%08X with ESI=0x%08X...",
             entry_point, boot_params_addr);

    // Guaranteed final log flush right before handing off execution to Linux!
    disk_log_flush_with_feedback();

    typedef void (*trampoline_fn_t)(uint32_t src, uint32_t dst, uint32_t len,
                                    uint32_t params, uint32_t entry);
    trampoline_fn_t jump_to_kernel = (trampoline_fn_t)LINUX_TRAMPOLINE_PHYS;

    // Clear VGA screen so Linux console starts with a clean display
    vga_clear();

    // Call trampoline in low memory
    jump_to_kernel(kernel_source_addr, kernel_target_addr, kernel_size,
                   boot_params_addr, entry_point);

    // Unreachable
    while (1) {
        __asm__ volatile ("hlt");
    }
}
