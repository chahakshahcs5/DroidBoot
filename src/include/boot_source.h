#ifndef BOOT_SOURCE_H
#define BOOT_SOURCE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct boot_source;

typedef struct boot_source {
    const char *name;
    
    // Core file operations
    int      (*open)(struct boot_source *src, const char *path);
    uint32_t (*read)(struct boot_source *src, void *buf, uint32_t size);
    int      (*seek)(struct boot_source *src, uint64_t offset);
    uint64_t (*tell)(struct boot_source *src);
    uint64_t (*size)(struct boot_source *src);
    void     (*close)(struct boot_source *src);
    
    // Private driver data
    void     *priv;
} boot_source_t;

#endif // BOOT_SOURCE_H
