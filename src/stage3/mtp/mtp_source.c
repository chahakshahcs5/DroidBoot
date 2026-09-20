#include "mtp_source.h"
#include "../core/printf.h"
#include "../memory/memory.h"
#include <stddef.h>

typedef struct mtp_fs {
    mtp_session_t *session;
    bool           file_open;
    uint32_t       handle;
    uint64_t       file_size;
    uint64_t       file_pos;
    uint8_t       *cached_data;
    uint32_t       cached_len;
} mtp_fs_t;

static bool str_eq_nocase(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return false;
    }
    return (*a == '\0' && *b == '\0');
}

static const char *get_filename_from_path(const char *path) {
    if (!path) return "";
    const char *last = path;
    while (*path) {
        if (*path == '/' || *path == '\\') {
            last = path + 1;
        }
        path++;
    }
    return last;
}

static int mtp_source_open(boot_source_t *src, const char *path) {
    mtp_fs_t *fs = (mtp_fs_t *)src->priv;
    if (!fs || !fs->session || !path) return -1;

    const char *target_name = get_filename_from_path(path);
    log_info("MTP", "Searching Android internal storage for '%s'...", target_name);

    if (!fs->session->session_active) {
        log_error("MTP", "MTP session is not active!");
        return -2;
    }

    // Query storage IDs if needed
    if (fs->session->active_storage_id == 0) {
        uint32_t storage_ids[8];
        uint32_t scount = 0;
        if (mtp_get_storage_ids(fs->session, storage_ids, 8, &scount) != 0 || scount == 0) {
            log_error("MTP", "No active MTP storage volumes found!");
            return -3;
        }
    }

    // Query object handles
    uint32_t handles[256];
    uint32_t hcount = 0;
    int res = mtp_get_object_handles(fs->session, fs->session->active_storage_id, 0x00000000, handles, 256, &hcount);
    if (res != 0) {
        log_error("MTP", "Failed to retrieve object handles (error %d)", res);
        return -4;
    }

    log_info("MTP", "Enumerating %u objects in Android storage...", hcount);

    for (uint32_t i = 0; i < hcount; i++) {
        char name[64];
        uint64_t size = 0;
        if (mtp_get_object_info(fs->session, handles[i], name, sizeof(name), &size) == 0) {
            log_info("MTP", "  Object [0x%08X]: '%s' (%u bytes)", handles[i], name, (uint32_t)size);
            if (str_eq_nocase(name, target_name)) {
                log_info("MTP", "Found target file '%s' (Handle 0x%08X, Size %u bytes)!",
                         name, handles[i], (uint32_t)size);
                fs->file_open = true;
                fs->handle = handles[i];
                fs->file_size = size;
                fs->file_pos = 0;
                return 0;
            }
        }
    }

    log_error("MTP", "File '%s' not found on Android device!", target_name);
    return -5;
}

int mtp_find_boot_file(boot_source_t *src, char *out_name, uint32_t max_len) {
    mtp_fs_t *fs = (mtp_fs_t *)src->priv;
    if (!fs || !fs->session || !out_name || max_len == 0) return -1;

    uint32_t handles[256];
    uint32_t hcount = 0;
    // Query with parent = 0xFFFFFFFF (all objects across all folders including /Download/)
    int res = mtp_get_object_handles(fs->session, fs->session->active_storage_id, 0xFFFFFFFF, handles, 256, &hcount);
    if (res != 0 || hcount == 0) {
        // Fallback to root directory
        res = mtp_get_object_handles(fs->session, fs->session->active_storage_id, 0x00000000, handles, 256, &hcount);
        if (res != 0) return res;
    }

    // First search for .iso files
    for (uint32_t i = 0; i < hcount; i++) {
        char name[64];
        uint64_t size = 0;
        if (mtp_get_object_info(fs->session, handles[i], name, sizeof(name), &size) == 0) {
            int len = 0;
            while (name[len]) len++;
            if (len >= 4) {
                const char *ext = name + len - 4;
                if (str_eq_nocase(ext, ".iso")) {
                    for (int c = 0; c < len && c < (int)max_len - 1; c++) {
                        out_name[c] = name[c];
                    }
                    uint32_t final_idx = (uint32_t)len < (max_len - 1) ? (uint32_t)len : (max_len - 1);
                    out_name[final_idx] = '\0';
                    return 0;
                }
            }
        }
    }

    // Then search for bzImage, vmlinuz*, or .img files
    for (uint32_t i = 0; i < hcount; i++) {
        char name[64];
        uint64_t size = 0;
        if (mtp_get_object_info(fs->session, handles[i], name, sizeof(name), &size) == 0) {
            int len = 0;
            while (name[len]) len++;
            if (str_eq_nocase(name, "bzImage") || str_eq_nocase(name, "vmlinuz") ||
                (len >= 4 && str_eq_nocase(name + len - 4, ".img"))) {
                for (int c = 0; c < len && c < (int)max_len - 1; c++) {
                    out_name[c] = name[c];
                }
                uint32_t final_idx = (uint32_t)len < (max_len - 1) ? (uint32_t)len : (max_len - 1);
                out_name[final_idx] = '\0';
                return 0;
            }
        }
    }

    return -2;
}

static uint32_t mtp_source_read(boot_source_t *src, void *buf, uint32_t size) {
    mtp_fs_t *fs = (mtp_fs_t *)src->priv;
    if (!fs || !fs->file_open || !buf) return 0;

    if (fs->file_pos + size > fs->file_size) {
        size = (uint32_t)(fs->file_size - fs->file_pos);
    }
    if (size == 0) return 0;

    // For large files (> 8 MB, e.g. 353 MB Alpine ISO), stream on demand via GetPartialObject!
    if (fs->file_size > (8 * 1024 * 1024)) {
        uint32_t actual = 0;
        int res = mtp_get_partial_object(fs->session, fs->handle, (uint32_t)fs->file_pos, size, buf, &actual);
        if (res == 0) {
            fs->file_pos += actual;
            return actual;
        }
        log_error("MTP", "GetPartialObject failed at offset %u (error %d)", (uint32_t)fs->file_pos, res);
        return 0;
    }

    // For smaller files (<= 8 MB), cache on first read
    if (!fs->cached_data) {
        fs->cached_data = (uint8_t *)kmalloc((uint32_t)fs->file_size);
        if (!fs->cached_data) {
            log_error("MTP", "Failed to allocate memory buffer (%u bytes) for streaming",
                      (uint32_t)fs->file_size);
            return 0;
        }

        uint32_t actual = 0;
        log_info("MTP", "Streaming %u bytes over USB Bulk IN endpoint...", (uint32_t)fs->file_size);
        int res = mtp_get_object(fs->session, fs->handle, fs->cached_data, (uint32_t)fs->file_size, &actual);
        if (res != 0) {
            log_error("MTP", "GetObject failed with error %d", res);
            kfree(fs->cached_data);
            fs->cached_data = NULL;
            return 0;
        }
        fs->cached_len = actual;
        log_info("MTP", "Streaming complete: Received %u bytes.", actual);
    }

    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0; i < size; i++) {
        dst[i] = fs->cached_data[fs->file_pos + i];
    }
    fs->file_pos += size;

    return size;
}

static int mtp_source_seek(boot_source_t *src, uint64_t offset) {
    mtp_fs_t *fs = (mtp_fs_t *)src->priv;
    if (!fs || !fs->file_open) return -1;
    if (offset > fs->file_size) offset = fs->file_size;
    fs->file_pos = offset;
    return 0;
}

static uint64_t mtp_source_tell(boot_source_t *src) {
    mtp_fs_t *fs = (mtp_fs_t *)src->priv;
    return fs ? fs->file_pos : 0;
}

static uint64_t mtp_source_size(boot_source_t *src) {
    mtp_fs_t *fs = (mtp_fs_t *)src->priv;
    return fs ? fs->file_size : 0;
}

static void mtp_source_close(boot_source_t *src) {
    mtp_fs_t *fs = (mtp_fs_t *)src->priv;
    if (fs) {
        if (fs->cached_data) {
            kfree(fs->cached_data);
            fs->cached_data = NULL;
        }
        fs->file_open = false;
        fs->handle = 0;
        fs->file_size = 0;
        fs->file_pos = 0;
    }
}

boot_source_t *boot_source_mtp_create(mtp_session_t *session) {
    if (!session) return NULL;

    mtp_fs_t *fs = (mtp_fs_t *)kmalloc(sizeof(mtp_fs_t));
    if (!fs) return NULL;

    fs->session = session;
    fs->file_open = false;
    fs->handle = 0;
    fs->file_size = 0;
    fs->file_pos = 0;
    fs->cached_data = NULL;
    fs->cached_len = 0;

    boot_source_t *src = (boot_source_t *)kmalloc(sizeof(boot_source_t));
    if (!src) return NULL;

    src->name = "Android MTP";
    src->open = mtp_source_open;
    src->read = mtp_source_read;
    src->seek = mtp_source_seek;
    src->tell = mtp_source_tell;
    src->size = mtp_source_size;
    src->close = mtp_source_close;
    src->priv = fs;

    log_info("MTP", "BootSource 'Android MTP' registered.");
    return src;
}
