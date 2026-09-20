#ifndef IMAGE_DETECT_H
#define IMAGE_DETECT_H

#include <stdint.h>
#include <stdbool.h>

typedef enum image_type {
    IMAGE_TYPE_UNKNOWN = 0,
    IMAGE_TYPE_BZIMAGE,
    IMAGE_TYPE_MBR_DISK,
    IMAGE_TYPE_GPT_DISK,
    IMAGE_TYPE_ISO9660
} image_type_t;

typedef struct image_info {
    image_type_t type;
    const char  *description;
    uint32_t     partition_count;
    uint32_t     kernel_offset;
    uint32_t     kernel_size;
} image_info_t;

image_type_t image_detect_type(const void *buf, uint32_t len, image_info_t *out_info);
const char  *image_type_name(image_type_t type);

#endif // IMAGE_DETECT_H
