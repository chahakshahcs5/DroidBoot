#include "linux_boot.h"
#include "../core/printf.h"
#include "../memory/memory.h"
#include "../../include/io.h"

// 60-byte self-contained relocation trampoline for 32-bit Linux handoff
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
    0x53,                               // push ebx           (push entry point)
    0x31, 0xC0,                         // xor eax, eax       (EAX = 0)
    0x31, 0xDB,                         // xor ebx, ebx       (EBX = 0)
    0x31, 0xC9,                         // xor ecx, ecx       (ECX = 0)
    0x31, 0xD2,                         // xor edx, edx       (EDX = 0)
    0x31, 0xED,                         // xor ebp, ebp       (EBP = 0)
    0x31, 0xFF,                         // xor edi, edi       (EDI = 0)
    0xC3                                // ret                (jump to entry point)
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

    // Setup standard 80x25 VGA text mode screen_info (prevents early kernel console crash)
    out_params->screen_info[0] = 0;      // orig_x = 0
    out_params->screen_info[1] = 0;      // orig_y = 0
    out_params->screen_info[4] = 0;      // orig_video_page = 0
    out_params->screen_info[6] = 3;      // orig_video_mode = 3 (80x25 color text)
    out_params->screen_info[7] = 80;     // orig_video_cols = 80
    out_params->screen_info[14] = 25;    // orig_video_lines = 25
    out_params->screen_info[15] = 0x22;  // orig_video_isVGA = 0x22 (VIDEO_TYPE_VGAC)
    out_params->screen_info[16] = 16;    // orig_video_points = 16 (font 8x16)

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

    // Transfer E820 memory map from boot_info
    if (boot_info && boot_info->e820_count > 0) {
        e820_entry_t *src_e820 = (e820_entry_t *)boot_info->e820_map_addr;
        uint32_t count = boot_info->e820_count;
        if (count > 128) count = 128;

        for (uint32_t i = 0; i < count; i++) {
            out_params->e820_table[i].addr = src_e820[i].base;
            out_params->e820_table[i].size = src_e820[i].length;
            out_params->e820_table[i].type = src_e820[i].type;
        }
        out_params->e820_entries = (uint8_t)count;
        log_info("LINUX", "Transferred %u E820 memory entries to boot_params", count);
    }

    log_info("LINUX", "Prepared Linux boot_params at 0x%08X", (uint32_t)out_params);
    return 0;
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

    typedef void (*trampoline_fn_t)(uint32_t src, uint32_t dst, uint32_t len,
                                    uint32_t params, uint32_t entry);
    trampoline_fn_t jump_to_kernel = (trampoline_fn_t)LINUX_TRAMPOLINE_PHYS;

    // Call trampoline in low memory
    jump_to_kernel(kernel_source_addr, kernel_target_addr, kernel_size,
                   boot_params_addr, entry_point);

    // Unreachable
    while (1) {
        __asm__ volatile ("hlt");
    }
}
