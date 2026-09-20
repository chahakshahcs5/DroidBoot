#include "mtp_source.h"
#include "../core/printf.h"
#include "../memory/memory.h"
#include "../debug/disk_log.h"
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

static bool str_ends_with_nocase(const char *str, const char *suffix) {
    if (!str || !suffix) return false;
    int str_len = 0;
    while (str[str_len]) str_len++;
    int suf_len = 0;
    while (suffix[suf_len]) suf_len++;
    if (str_len < suf_len) return false;

    const char *p = str + str_len - suf_len;
    while (*p && *suffix) {
        char cp = *p++;
        char cs = *suffix++;
        if (cp >= 'a' && cp <= 'z') cp -= 32;
        if (cs >= 'a' && cs <= 'z') cs -= 32;
        if (cp != cs) return false;
    }
    return true;
}

static bool is_boot_image(const char *name) {
    if (!name || !name[0]) return false;
    if (str_ends_with_nocase(name, ".iso")) return true;
    if (str_ends_with_nocase(name, ".img")) return true;
    if (str_eq_nocase(name, "bzImage")) return true;
    if (str_eq_nocase(name, "vmlinuz")) return true;
    if ((name[0] == 'v' || name[0] == 'V') &&
        (name[1] == 'm' || name[1] == 'M') &&
        (name[2] == 'l' || name[2] == 'L') &&
        (name[3] == 'i' || name[3] == 'I') &&
        (name[4] == 'n' || name[4] == 'N') &&
        (name[5] == 'u' || name[5] == 'U') &&
        (name[6] == 'z' || name[6] == 'Z')) {
        return true;
    }
    return false;
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

static int mtp_search_folder(mtp_session_t *session, uint32_t storage_id, uint32_t folder_handle,
                             const char *folder_name, const char *target_exact,
                             char *out_name, uint32_t max_len, uint32_t *out_handle, uint64_t *out_size) {
    uint32_t handles[256];
    uint32_t hcount = 0;
    int res = mtp_get_object_handles(session, storage_id, folder_handle, handles, 256, &hcount);
    if (res != 0) return res;

    log_info("MTP", "Searching /%s/ (Handle 0x%08X): %u items found", folder_name, folder_handle, hcount);
    disk_log_flush();

    for (uint32_t i = 0; i < hcount; i++) {
        char name[64];
        uint64_t size = 0;
        if (mtp_get_object_info(session, handles[i], name, sizeof(name), &size) == 0) {
            log_info("MTP", "  [%s] '%s' (%u MB)", folder_name, name, (uint32_t)(size / 1024 / 1024));
            disk_log_flush();
            bool match = false;
            if (target_exact && target_exact[0]) {
                if (str_eq_nocase(name, target_exact)) match = true;
            } else {
                if (is_boot_image(name)) match = true;
            }

            if (match) {
                if (out_name && max_len > 0) {
                    uint32_t l = 0;
                    while (name[l] && l < max_len - 1) {
                        out_name[l] = name[l];
                        l++;
                    }
                    out_name[l] = '\0';
                }
                if (out_handle) *out_handle = handles[i];
                if (out_size) *out_size = size;
                return 0;
            }
        }
    }
    return -1;
}

static int mtp_source_open(boot_source_t *src, const char *path) {
    mtp_fs_t *fs = (mtp_fs_t *)src->priv;
    if (!fs || !fs->session || !path) return -1;

    const char *target_name = get_filename_from_path(path);
    log_info("MTP", "Opening '%s'...", target_name);

    if (!fs->session->session_active) {
        log_error("MTP", "MTP session is not active!");
        return -2;
    }

    // If file was already located and handle cached by mtp_find_boot_file:
    if (fs->file_open && fs->handle != 0) {
        log_info("MTP", "Target '%s' already open (Handle 0x%08X, %u MB). Ready to stream.",
                 target_name, fs->handle, (uint32_t)(fs->file_size / 1024 / 1024));
        fs->file_pos = 0;
        return 0;
    }

    uint32_t storage_id = fs->session->active_storage_id;
    if (storage_id == 0) {
        uint32_t storage_ids[8];
        uint32_t scount = 0;
        if (mtp_get_storage_ids(fs->session, storage_ids, 8, &scount) != 0 || scount == 0) {
            log_error("MTP", "No active MTP storage volumes found!");
            return -3;
        }
        storage_id = fs->session->active_storage_id;
    }

    uint32_t handle = 0;
    uint64_t size = 0;

    // Search root directory
    if (mtp_search_folder(fs->session, storage_id, PTP_OBJECT_HANDLE_ROOT, "Root", target_name,
                          NULL, 0, &handle, &size) == 0) {
        fs->file_open = true;
        fs->handle = handle;
        fs->file_size = size;
        fs->file_pos = 0;
        return 0;
    }

    // Search subfolders
    uint32_t root_handles[64];
    uint32_t root_count = 0;
    if (mtp_get_object_handles(fs->session, storage_id, PTP_OBJECT_HANDLE_ROOT, root_handles, 64, &root_count) == 0) {
        for (uint32_t i = 0; i < root_count; i++) {
            char name[64];
            uint64_t s = 0;
            if (mtp_get_object_info(fs->session, root_handles[i], name, sizeof(name), &s) == 0 && s == 0) {
                if (mtp_search_folder(fs->session, storage_id, root_handles[i], name, target_name,
                                      NULL, 0, &handle, &size) == 0) {
                    fs->file_open = true;
                    fs->handle = handle;
                    fs->file_size = size;
                    fs->file_pos = 0;
                    return 0;
                }
            }
        }
    }

    log_error("MTP", "File '%s' not found on Android device!", target_name);
    return -5;
}

int mtp_find_boot_file(boot_source_t *src, char *out_name, uint32_t max_len) {
    mtp_fs_t *fs = (mtp_fs_t *)src->priv;
    if (!fs || !fs->session || !out_name || max_len == 0) return -1;

    if (fs->session->active_storage_id == 0) {
        uint32_t storage_ids[8];
        uint32_t scount = 0;
        if (mtp_get_storage_ids(fs->session, storage_ids, 8, &scount) != 0 || scount == 0) {
            log_error("MTP", "No active MTP storage volumes found!");
            return -3;
        }
    }

    uint32_t storage_id = fs->session->active_storage_id;

    // 1. Enumerate root directory (PTP_OBJECT_HANDLE_ROOT = 0xFFFFFFFF)
    uint32_t root_handles[256];
    uint32_t root_count = 0;
    int res = mtp_get_object_handles(fs->session, storage_id, PTP_OBJECT_HANDLE_ROOT, root_handles, 256, &root_count);
    if (res != 0) {
        log_error("MTP", "Failed to enumerate root directory (error %d)", res);
        return res;
    }

    log_info("MTP", "Android Storage root: %u items found", root_count);
    disk_log_flush();

    uint32_t download_handle = 0;
    uint32_t docs_handle = 0;
    uint32_t other_folders[16];
    char other_names[16][32];
    uint32_t other_count = 0;

    // Check root items first
    for (uint32_t i = 0; i < root_count; i++) {
        char name[64];
        uint64_t size = 0;
        if (mtp_get_object_info(fs->session, root_handles[i], name, sizeof(name), &size) == 0) {
            if (is_boot_image(name)) {
                log_info("BOOT", "Found boot image in root: '%s' (%u MB)", name, (uint32_t)(size / 1024 / 1024));
                uint32_t l = 0;
                while (name[l] && l < max_len - 1) {
                    out_name[l] = name[l];
                    l++;
                }
                out_name[l] = '\0';
                fs->handle = root_handles[i];
                fs->file_size = size;
                fs->file_open = true;
                return 0;
            }

            if (str_eq_nocase(name, "Download") || str_eq_nocase(name, "Downloads")) {
                download_handle = root_handles[i];
            } else if (str_eq_nocase(name, "Documents") || str_eq_nocase(name, "Document")) {
                docs_handle = root_handles[i];
            } else if (other_count < 16 && size == 0) {
                other_folders[other_count] = root_handles[i];
                uint32_t nl = 0;
                while (name[nl] && nl < 31) {
                    other_names[other_count][nl] = name[nl];
                    nl++;
                }
                other_names[other_count][nl] = '\0';
                other_count++;
            }
        }
    }

    // 2. Search /Download/ or /Downloads/
    if (download_handle != 0) {
        uint32_t handle = 0;
        uint64_t size = 0;
        if (mtp_search_folder(fs->session, storage_id, download_handle, "Download", NULL,
                              out_name, max_len, &handle, &size) == 0) {
            log_info("BOOT", "SUCCESS: Found boot image in /Download/: '%s' (Handle 0x%08X, %u MB)",
                     out_name, handle, (uint32_t)(size / 1024 / 1024));
            fs->handle = handle;
            fs->file_size = size;
            fs->file_open = true;
            disk_log_flush();
            return 0;
        }
    }

    // 3. Search /Documents/
    if (docs_handle != 0) {
        uint32_t handle = 0;
        uint64_t size = 0;
        if (mtp_search_folder(fs->session, storage_id, docs_handle, "Documents", NULL,
                              out_name, max_len, &handle, &size) == 0) {
            log_info("BOOT", "SUCCESS: Found boot image in /Documents/: '%s' (Handle 0x%08X, %u MB)",
                     out_name, handle, (uint32_t)(size / 1024 / 1024));
            fs->handle = handle;
            fs->file_size = size;
            fs->file_open = true;
            disk_log_flush();
            return 0;
        }
    }

    // 4. Search any other top-level folders
    for (uint32_t k = 0; k < other_count; k++) {
        uint32_t handle = 0;
        uint64_t size = 0;
        if (mtp_search_folder(fs->session, storage_id, other_folders[k], other_names[k], NULL,
                              out_name, max_len, &handle, &size) == 0) {
            log_info("BOOT", "SUCCESS: Found boot image in /%s/: '%s' (Handle 0x%08X, %u MB)",
                     other_names[k], out_name, handle, (uint32_t)(size / 1024 / 1024));
            fs->handle = handle;
            fs->file_size = size;
            fs->file_open = true;
            disk_log_flush();
            return 0;
        }
    }

    // 5. Fallback: Search all objects on device
    log_info("MTP", "Searching all storage objects on device (fallback)...");
    disk_log_flush();
    uint32_t all_handles[256];
    uint32_t all_count = 0;
    if (mtp_get_object_handles(fs->session, storage_id, 0x00000000, all_handles, 256, &all_count) == 0) {
        for (uint32_t a = 0; a < all_count; a++) {
            char name[64];
            uint64_t size = 0;
            if (mtp_get_object_info(fs->session, all_handles[a], name, sizeof(name), &size) == 0) {
                if (is_boot_image(name)) {
                    log_info("BOOT", "SUCCESS: Found boot image: '%s' (Handle 0x%08X, %u MB)",
                             name, all_handles[a], (uint32_t)(size / 1024 / 1024));
                    uint32_t l = 0;
                    while (name[l] && l < max_len - 1) {
                        out_name[l] = name[l];
                        l++;
                    }
                    out_name[l] = '\0';
                    fs->handle = all_handles[a];
                    fs->file_size = size;
                    fs->file_open = true;
                    disk_log_flush();
                    return 0;
                }
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

    // For large files (> 8 MB, e.g. 353 MB Alpine ISO), stream on demand via GetPartialObject in 64 KiB chunks!
    if (fs->file_size > (8 * 1024 * 1024)) {
        uint8_t *dst = (uint8_t *)buf;
        uint32_t total_transferred = 0;
        uint32_t remaining = size;
        uint32_t last_pct = 0xFFFFFFFF;

        while (remaining > 0) {
            uint32_t chunk_req = (remaining > 65536) ? 65536 : remaining;
            uint32_t actual = 0;
            int res = mtp_get_partial_object(fs->session, fs->handle,
                                             (uint32_t)(fs->file_pos + total_transferred),
                                             chunk_req, dst + total_transferred, &actual);
            if (res != 0 || actual == 0) {
                log_error("MTP", "GetPartialObject failed at offset %u (error %d)",
                          (uint32_t)(fs->file_pos + total_transferred), res);
                break;
            }
            total_transferred += actual;
            remaining -= actual;

            // Progress indication for large multi-megabyte transfers
            if (size > (4 * 1024 * 1024)) {
                uint32_t pct = (total_transferred * 100) / size;
                if (pct / 25 != last_pct / 25) {
                    last_pct = pct;
                    log_info("MTP", "  Streamed %u / %u MB (%u%%)...",
                             total_transferred / 1024 / 1024, size / 1024 / 1024, pct);
                    disk_log_flush();
                }
            }
        }
        fs->file_pos += total_transferred;
        return total_transferred;
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
