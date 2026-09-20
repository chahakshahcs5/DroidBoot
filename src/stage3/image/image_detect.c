#include "image_detect.h"
#include "../linux/linux_boot.h"
#include "../core/printf.h"

static bool kbytes_equal(const void *a, const void *b, uint32_t n) {
    const uint8_t *p1 = (const uint8_t *)a;
    const uint8_t *p2 = (const uint8_t *)b;
    for (uint32_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) return false;
    }
    return true;
}

const char *image_type_name(image_type_t type) {
    switch (type) {
        case IMAGE_TYPE_BZIMAGE:  return "Linux bzImage Kernel";
        case IMAGE_TYPE_MBR_DISK: return "MBR Partitioned Disk Image";
        case IMAGE_TYPE_GPT_DISK: return "GPT Partitioned Disk Image";
        case IMAGE_TYPE_ISO9660:  return "ISO9660 Optical Disk Image";
        default:                  return "Unknown / Unrecognized Image";
    }
}

image_type_t image_detect_type(const void *buf, uint32_t len, image_info_t *out_info) {
    if (!buf || len < 512) {
        if (out_info) {
            out_info->type = IMAGE_TYPE_UNKNOWN;
            out_info->description = image_type_name(IMAGE_TYPE_UNKNOWN);
            out_info->partition_count = 0;
            out_info->kernel_offset = 0;
            out_info->kernel_size = 0;
        }
        return IMAGE_TYPE_UNKNOWN;
    }

    const uint8_t *data = (const uint8_t *)buf;

    // 1. Check for Linux bzImage ("HdrS" at offset 0x0202)
    if (len >= 1024) {
        uint32_t magic = *(const uint32_t *)(data + 0x0202);
        if (magic == LINUX_HDRS_MAGIC) {
            log_info("IMAGE", "Format detected: Linux bzImage");
            if (out_info) {
                out_info->type = IMAGE_TYPE_BZIMAGE;
                out_info->description = image_type_name(IMAGE_TYPE_BZIMAGE);
                out_info->partition_count = 0;
                out_info->kernel_offset = 0;
                out_info->kernel_size = len;
            }
            return IMAGE_TYPE_BZIMAGE;
        }
    }

    // 2. Check for GPT Header ("EFI PART" at LBA 1 / offset 512)
    if (len >= 1024) {
        if (kbytes_equal(data + 512, "EFI PART", 8)) {
            log_info("IMAGE", "Format detected: GPT Partitioned Disk Image");
            if (out_info) {
                out_info->type = IMAGE_TYPE_GPT_DISK;
                out_info->description = image_type_name(IMAGE_TYPE_GPT_DISK);
                out_info->partition_count = 0;
                out_info->kernel_offset = 0;
                out_info->kernel_size = 0;
            }
            return IMAGE_TYPE_GPT_DISK;
        }
    }

    // 3. Check for ISO9660 PVD ("CD001" at sector 16 / offset 0x8000)
    if (len >= (32768 + 6)) {
        if (data[0x8000] == 0x01 && kbytes_equal(data + 0x8001, "CD001", 5)) {
            log_info("IMAGE", "Format detected: ISO9660 CD-ROM Image");
            if (out_info) {
                out_info->type = IMAGE_TYPE_ISO9660;
                out_info->description = image_type_name(IMAGE_TYPE_ISO9660);
                out_info->partition_count = 0;
                out_info->kernel_offset = 0;
                out_info->kernel_size = 0;
            }
            return IMAGE_TYPE_ISO9660;
        }
    }

    // 4. Check for MBR Partitioned Disk (0xAA55 at offset 510)
    uint16_t mbr_sig = *(const uint16_t *)(data + 510);
    if (mbr_sig == 0xAA55) {
        // Count valid partitions at offset 446 (0x1BE)
        uint32_t valid_parts = 0;
        bool is_protective_gpt = false;

        for (int p = 0; p < 4; p++) {
            const uint8_t *part = data + 446 + (p * 16);
            uint8_t type = part[4];
            uint32_t sec_count = *(const uint32_t *)(part + 12);

            if (type == 0xEE) {
                is_protective_gpt = true;
            }
            if (type != 0x00 && sec_count > 0) {
                valid_parts++;
            }
        }

        if (is_protective_gpt) {
            log_info("IMAGE", "Format detected: GPT Disk (Protective MBR)");
            if (out_info) {
                out_info->type = IMAGE_TYPE_GPT_DISK;
                out_info->description = image_type_name(IMAGE_TYPE_GPT_DISK);
                out_info->partition_count = valid_parts;
                out_info->kernel_offset = 0;
                out_info->kernel_size = 0;
            }
            return IMAGE_TYPE_GPT_DISK;
        }

        if (valid_parts > 0) {
            log_info("IMAGE", "Format detected: MBR Partitioned Disk (%u partition(s))", valid_parts);
            if (out_info) {
                out_info->type = IMAGE_TYPE_MBR_DISK;
                out_info->description = image_type_name(IMAGE_TYPE_MBR_DISK);
                out_info->partition_count = valid_parts;
                out_info->kernel_offset = 0;
                out_info->kernel_size = 0;
            }
            return IMAGE_TYPE_MBR_DISK;
        }
    }

    log_info("IMAGE", "Format unrecognized (size %u bytes)", len);
    if (out_info) {
        out_info->type = IMAGE_TYPE_UNKNOWN;
        out_info->description = image_type_name(IMAGE_TYPE_UNKNOWN);
        out_info->partition_count = 0;
        out_info->kernel_offset = 0;
        out_info->kernel_size = 0;
    }
    return IMAGE_TYPE_UNKNOWN;
}
