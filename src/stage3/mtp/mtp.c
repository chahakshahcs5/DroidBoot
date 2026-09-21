#include "mtp.h"
#include "../xhci/xhci_regs.h"
#include "../../include/io.h"
#include "../core/printf.h"
#include "../core/rtc.h"
#include "../debug/vga.h"
#include "../debug/disk_log.h"
#include "../memory/memory.h"
#include <stddef.h>

#include "../usb/usb.h"

static const char *mtp_opcode_name(uint16_t op) {
    switch (op) {
        case PTP_OC_GetDeviceInfo:       return "GetDeviceInfo";
        case PTP_OC_OpenSession:         return "OpenSession";
        case PTP_OC_CloseSession:        return "CloseSession";
        case PTP_OC_GetStorageIDs:       return "GetStorageIDs";
        case PTP_OC_GetStorageInfo:      return "GetStorageInfo";
        case PTP_OC_GetNumObjects:       return "GetNumObjects";
        case PTP_OC_GetObjectHandles:    return "GetObjectHandles";
        case PTP_OC_GetObjectInfo:       return "GetObjectInfo";
        case PTP_OC_GetObject:           return "GetObject";
        case PTP_OC_DeleteObject:        return "DeleteObject";
        case PTP_OC_SendObjectInfo:      return "SendObjectInfo";
        case PTP_OC_SendObject:          return "SendObject";
        case PTP_OC_GetPartialObject:    return "GetPartialObject";
        default:                         return "Unknown";
    }
}

static const char *mtp_response_code_name(uint16_t rc) {
    switch (rc) {
        case PTP_RC_OK:                  return "OK";
        case PTP_RC_GeneralError:        return "GeneralError";
        case PTP_RC_SessionNotOpen:      return "SessionNotOpen";
        case PTP_RC_InvalidTransactionID:return "InvalidTransactionID";
        case PTP_RC_OperationNotSupported:return "OperationNotSupported";
        case PTP_RC_ParameterNotSupported:return "ParameterNotSupported";
        case PTP_RC_IncompleteTransfer:  return "IncompleteTransfer";
        case PTP_RC_InvalidStorageID:    return "InvalidStorageID";
        case PTP_RC_InvalidObjectHandle: return "InvalidObjectHandle";
        case PTP_RC_SessionAlreadyOpen:  return "SessionAlreadyOpen";
        default:                         return "Unknown";
    }
}

static int mtp_send_cmd(mtp_session_t *s, uint16_t opcode, int num_params, uint32_t p1, uint32_t p2, uint32_t p3) {
    ptp_container_t cmd;
    cmd.length = 12 + num_params * 4;
    cmd.type = PTP_CONTAINER_TYPE_COMMAND;
    cmd.code = opcode;
    cmd.transaction_id = s->transaction_id++;
    if (num_params > 0) cmd.params[0] = p1;
    if (num_params > 1) cmd.params[1] = p2;
    if (num_params > 2) cmd.params[2] = p3;

    log_debug("MTP", "TX CMD 0x%04X (%s) [xid=%u, p0=0x%X, p1=0x%X, p2=0x%X]",
              opcode, mtp_opcode_name(opcode), cmd.transaction_id, p1, p2, p3);

    uint32_t sent = 0;
    return usb_bulk_transfer(s->usb_dev, s->usb_dev->mtp_bulk_out_ep, &cmd, cmd.length, &sent);
}

static int mtp_recv_resp(mtp_session_t *s, ptp_container_t *resp) {
    uint32_t received = 0;
    int res = usb_bulk_transfer(s->usb_dev, s->usb_dev->mtp_bulk_in_ep, resp, sizeof(ptp_container_t), &received);
    if (res != 0) {
        log_debug("MTP", "RX RESP transfer error %d", res);
        return res;
    }

    if (resp->type != PTP_CONTAINER_TYPE_RESPONSE) {
        log_error("MTP", "Expected Response container, got type %u", resp->type);
        return -1;
    }

    log_debug("MTP", "RX RESP 0x%04X (%s) [xid=%u, p0=0x%X]",
              resp->code, mtp_response_code_name(resp->code), resp->transaction_id, resp->params[0]);

    return (resp->code == PTP_RC_OK) ? 0 : (int)resp->code;
}

int mtp_init_session(usb_device_t *dev, mtp_session_t *session) {
    if (!dev || !session) return -1;

    session->usb_dev = dev;
    session->session_id = 1;
    session->transaction_id = 1;
    session->active_storage_id = 0;
    session->session_active = false;

    log_info("MTP", "Sending OpenSession command (Session ID: 1)...");
    int res = mtp_send_cmd(session, PTP_OC_OpenSession, 1, session->session_id, 0, 0);
    if (res != 0) {
        log_error("MTP", "Failed to send OpenSession command (error %d)", res);
        return res;
    }

    ptp_container_t resp;
    res = mtp_recv_resp(session, &resp);
    if (res == 0 || res == PTP_RC_SessionAlreadyOpen) {
        session->session_active = true;
        log_info("MTP", "MTP Session Initialized Successfully (Session ID: %u)", session->session_id);
        return 0;
    }

    log_error("MTP", "OpenSession rejected by device (Response Code: 0x%04X)", resp.code);
    return res;
}

static void mtp_drain_bulk_in(mtp_session_t *session, uint32_t expected_total, uint32_t received_so_far) {
    if (received_so_far >= expected_total) return;
    uint32_t remaining = expected_total - received_so_far;
    static uint8_t drain_buf[1024];
    while (remaining > 0) {
        uint32_t to_read = (remaining > sizeof(drain_buf)) ? sizeof(drain_buf) : remaining;
        uint32_t transferred = 0;
        int res = usb_bulk_transfer(session->usb_dev, session->usb_dev->mtp_bulk_in_ep, drain_buf, to_read, &transferred);
        if (res != 0 || transferred == 0) break;
        if (transferred >= remaining) break;
        remaining -= transferred;
    }
}

int mtp_get_storage_ids(mtp_session_t *session, uint32_t *storage_ids, uint32_t max_ids, uint32_t *out_count) {
    if (!session || !storage_ids || max_ids == 0) return -1;

    log_info("MTP", "Requesting Storage IDs from Android device...");
    int res = mtp_send_cmd(session, PTP_OC_GetStorageIDs, 0, 0, 0, 0);
    if (res != 0) return res;

    // Buffer for Storage IDs data payload
    uint8_t buf[256];
    uint32_t received = 0;
    res = usb_bulk_transfer(session->usb_dev, session->usb_dev->mtp_bulk_in_ep, buf, sizeof(buf), &received);
    if (res != 0) return res;

    ptp_container_t *data_cont = (ptp_container_t *)buf;
    if (data_cont->type != PTP_CONTAINER_TYPE_DATA) {
        log_error("MTP", "Expected Data container for GetStorageIDs!");
        return -2;
    }

    // Number of array elements is at offset 12
    uint32_t num_elements = *(uint32_t *)(buf + 12);
    log_info("MTP", "Android reported %u storage volume(s):", num_elements);

    uint32_t copy_count = num_elements > max_ids ? max_ids : num_elements;
    uint32_t *ids = (uint32_t *)(buf + 16);
    for (uint32_t i = 0; i < copy_count; i++) {
        storage_ids[i] = ids[i];
        log_info("MTP", "  Storage [%u]: ID = 0x%08X", i, ids[i]);
    }
    if (out_count) *out_count = copy_count;

    if (copy_count > 0) {
        session->active_storage_id = storage_ids[0];
    }

    mtp_drain_bulk_in(session, data_cont->length, received);

    ptp_container_t resp;
    return mtp_recv_resp(session, &resp);
}

int mtp_get_object_handles(mtp_session_t *session, uint32_t storage_id, uint32_t parent_handle, uint32_t *handles, uint32_t max_handles, uint32_t *out_count) {
    if (!session || !handles || max_handles == 0) return -1;

    int res = mtp_send_cmd(session, PTP_OC_GetObjectHandles, 3, storage_id, 0x00000000, parent_handle);
    if (res != 0) return res;

    static uint8_t handle_buf[4096];
    uint32_t received = 0;
    res = usb_bulk_transfer(session->usb_dev, session->usb_dev->mtp_bulk_in_ep, handle_buf, sizeof(handle_buf), &received);
    if (res != 0) return res;

    ptp_container_t *data_cont = (ptp_container_t *)handle_buf;
    if (data_cont->type != PTP_CONTAINER_TYPE_DATA) {
        log_error("MTP", "Expected Data container for GetObjectHandles!");
        return -2;
    }

    uint32_t num_handles = *(uint32_t *)(handle_buf + 12);
    uint32_t copy_count = num_handles > max_handles ? max_handles : num_handles;
    uint32_t *hlist = (uint32_t *)(handle_buf + 16);

    for (uint32_t i = 0; i < copy_count; i++) {
        handles[i] = hlist[i];
    }
    if (out_count) *out_count = copy_count;

    mtp_drain_bulk_in(session, data_cont->length, received);

    ptp_container_t resp;
    return mtp_recv_resp(session, &resp);
}

int mtp_get_object_info(mtp_session_t *session, uint32_t handle, char *out_name, uint32_t max_name_len, uint64_t *out_size) {
    if (!session || handle == 0) return -1;

    int res = mtp_send_cmd(session, PTP_OC_GetObjectInfo, 1, handle, 0, 0);
    if (res != 0) return res;

    static uint8_t info_buf[1024];
    uint32_t received = 0;
    res = usb_bulk_transfer(session->usb_dev, session->usb_dev->mtp_bulk_in_ep, info_buf, sizeof(info_buf), &received);
    if (res != 0) return res;

    ptp_container_t *data_cont = (ptp_container_t *)info_buf;
    if (data_cont->type != PTP_CONTAINER_TYPE_DATA) {
        log_error("MTP", "Expected Data container for GetObjectInfo!");
        return -2;
    }

    const uint8_t *ds = info_buf + 12;
    uint32_t obj_size = *(const uint32_t *)(ds + 8);
    if (out_size) *out_size = obj_size;

    if (out_name && max_name_len > 0) {
        uint8_t num_chars = ds[52];
        const uint16_t *chars = (const uint16_t *)(ds + 53);
        uint32_t i = 0;
        for (; i < num_chars && i < max_name_len - 1; i++) {
            char c = (char)(chars[i] & 0x7F);
            if (c == '\0') break;
            out_name[i] = c;
        }
        out_name[i] = '\0';
    }

    mtp_drain_bulk_in(session, data_cont->length, received);

    ptp_container_t resp;
    return mtp_recv_resp(session, &resp);
}

int mtp_get_object(mtp_session_t *session, uint32_t handle, void *out_buf, uint32_t max_len, uint32_t *actual_len) {
    if (!session || handle == 0 || !out_buf) return -1;

    int res = mtp_send_cmd(session, PTP_OC_GetObject, 1, handle, 0, 0);
    if (res != 0) return res;

    static uint8_t first_chunk[1024];
    uint32_t received = 0;
    res = usb_bulk_transfer(session->usb_dev, session->usb_dev->mtp_bulk_in_ep, first_chunk, sizeof(first_chunk), &received);
    if (res != 0) return res;

    ptp_container_t *cont = (ptp_container_t *)first_chunk;
    if (cont->type != PTP_CONTAINER_TYPE_DATA) {
        log_error("MTP", "Expected Data container for GetObject!");
        return -2;
    }

    uint32_t total_payload = cont->length - 12;
    uint32_t copy_first = received > 12 ? (received - 12) : 0;
    if (copy_first > max_len) copy_first = max_len;

    uint8_t *dst = (uint8_t *)out_buf;
    for (uint32_t i = 0; i < copy_first; i++) {
        dst[i] = first_chunk[12 + i];
    }

    uint32_t total_read = copy_first;
    while (total_read < total_payload && total_read < max_len) {
        uint32_t to_read = total_payload - total_read;
        if (to_read > 65536) to_read = 65536;
        if (total_read + to_read > max_len) to_read = max_len - total_read;

        uint32_t chunk_received = 0;
        res = usb_bulk_transfer(session->usb_dev, session->usb_dev->mtp_bulk_in_ep, dst + total_read, to_read, &chunk_received);
        if (res != 0) break;
        total_read += chunk_received;
        if (chunk_received == 0) break;
    }

    if (actual_len) *actual_len = total_read;

    mtp_drain_bulk_in(session, cont->length, total_read + 12);

    ptp_container_t resp;
    return mtp_recv_resp(session, &resp);
}

int mtp_get_partial_object(mtp_session_t *session, uint32_t handle, uint32_t offset, uint32_t max_bytes, void *out_buf, uint32_t *actual_len) {
    if (!session || handle == 0 || !out_buf || max_bytes == 0) return -1;

    // Operation 0x101B (GetPartialObject): Param1=Handle, Param2=Offset, Param3=MaxBytes
    int res = mtp_send_cmd(session, PTP_OC_GetPartialObject, 3, handle, offset, max_bytes);
    if (res != 0) return res;

    static uint8_t first_chunk[1024];
    uint32_t received = 0;
    res = usb_bulk_transfer(session->usb_dev, session->usb_dev->mtp_bulk_in_ep, first_chunk, sizeof(first_chunk), &received);
    if (res != 0) return res;

    ptp_container_t *cont = (ptp_container_t *)first_chunk;
    if (cont->type != PTP_CONTAINER_TYPE_DATA) {
        log_error("MTP", "Expected Data container for GetPartialObject!");
        return -2;
    }

    uint32_t total_payload = cont->length - 12;
    uint32_t copy_first = received > 12 ? (received - 12) : 0;
    if (copy_first > max_bytes) copy_first = max_bytes;

    uint8_t *dst = (uint8_t *)out_buf;
    for (uint32_t i = 0; i < copy_first; i++) {
        dst[i] = first_chunk[12 + i];
    }

    uint32_t total_read = copy_first;
    while (total_read < total_payload && total_read < max_bytes) {
        uint32_t to_read = total_payload - total_read;
        if (to_read > 65536) to_read = 65536;
        if (total_read + to_read > max_bytes) to_read = max_bytes - total_read;

        uint32_t chunk_received = 0;
        res = usb_bulk_transfer(session->usb_dev, session->usb_dev->mtp_bulk_in_ep, dst + total_read, to_read, &chunk_received);
        if (res != 0) break;
        total_read += chunk_received;
        if (chunk_received == 0) break;
    }

    if (actual_len) *actual_len = total_read;

    mtp_drain_bulk_in(session, cont->length, total_read + 12);

    ptp_container_t resp;
    return mtp_recv_resp(session, &resp);
}

static void mtp_k_memset(void *dst, uint8_t val, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = val;
}

static void mtp_k_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static bool mtp_str_eq_nocase(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return false;
    }
    return (*a == '\0' && *b == '\0');
}

static bool mtp_str_starts_with_nocase(const char *str, const char *prefix) {
    if (!str || !prefix) return false;
    while (*prefix) {
        char cs = *str++;
        char cp = *prefix++;
        if (cs >= 'a' && cs <= 'z') cs -= 32;
        if (cp >= 'a' && cp <= 'z') cp -= 32;
        if (cs != cp) return false;
    }
    return true;
}

static int mtp_send_data(mtp_session_t *s, uint16_t opcode, const void *payload, uint32_t payload_len) {
    uint32_t total_len = 12 + payload_len;
    ptp_container_t header;
    header.length = total_len;
    header.type = PTP_CONTAINER_TYPE_DATA;
    header.code = opcode;
    header.transaction_id = s->transaction_id - 1;

    uint32_t sent = 0;
    if (payload_len == 0) {
        return usb_bulk_transfer(s->usb_dev, s->usb_dev->mtp_bulk_out_ep, &header, 12, &sent);
    }

    uint8_t buf[512];
    if (total_len <= sizeof(buf)) {
        mtp_k_memcpy(buf, &header, 12);
        mtp_k_memcpy(buf + 12, payload, payload_len);
        return usb_bulk_transfer(s->usb_dev, s->usb_dev->mtp_bulk_out_ep, buf, total_len, &sent);
    }

    int res = usb_bulk_transfer(s->usb_dev, s->usb_dev->mtp_bulk_out_ep, &header, 12, &sent);
    if (res != 0) return res;

    // Send payload in chunks of up to 16 KiB
    const uint8_t *src = (const uint8_t *)payload;
    uint32_t offset = 0;
    while (offset < payload_len) {
        uint32_t chunk = payload_len - offset;
        if (chunk > 16384) chunk = 16384;
        uint32_t chunk_sent = 0;
        res = usb_bulk_transfer(s->usb_dev, s->usb_dev->mtp_bulk_out_ep, (void *)(src + offset), chunk, &chunk_sent);
        if (res != 0) return res;
        offset += chunk;
    }
    return 0;
}

int mtp_create_folder(mtp_session_t *session, uint32_t storage_id, uint32_t parent_handle, const char *folder_name, uint32_t *out_handle) {
    if (!session || !folder_name) return -1;

    int res = mtp_send_cmd(session, PTP_OC_SendObjectInfo, 2, storage_id, parent_handle, 0);
    if (res != 0) return res;

    uint8_t dataset[256];
    mtp_k_memset(dataset, 0, sizeof(dataset));
    *(uint32_t *)(dataset + 0) = storage_id;
    *(uint16_t *)(dataset + 4) = PTP_OFC_Association; // 0x3001
    *(uint32_t *)(dataset + 38) = parent_handle;
    *(uint16_t *)(dataset + 42) = PTP_AT_GenericFolder; // 0x0001

    uint32_t name_len = 0;
    while (folder_name[name_len]) name_len++;
    uint8_t *str_ptr = dataset + 52;
    *str_ptr++ = (uint8_t)(name_len + 1);
    for (uint32_t i = 0; i < name_len; i++) {
        *str_ptr++ = (uint8_t)folder_name[i];
        *str_ptr++ = 0x00;
    }
    *str_ptr++ = 0x00;
    *str_ptr++ = 0x00;

    uint32_t dataset_len = (uint32_t)(str_ptr - dataset);
    res = mtp_send_data(session, PTP_OC_SendObjectInfo, dataset, dataset_len);
    if (res != 0) return res;

    ptp_container_t resp;
    res = mtp_recv_resp(session, &resp);
    if (res == 0 && out_handle) {
        *out_handle = resp.params[2];
    }
    return res;
}

int mtp_create_empty_file(mtp_session_t *session, uint32_t storage_id, uint32_t parent_handle, const char *file_name, uint32_t *out_handle) {
    if (!session || !file_name) return -1;

    int res = mtp_send_cmd(session, PTP_OC_SendObjectInfo, 2, storage_id, parent_handle, 0);
    if (res != 0) return res;

    uint8_t dataset[256];
    mtp_k_memset(dataset, 0, sizeof(dataset));
    *(uint32_t *)(dataset + 0) = storage_id;
    *(uint16_t *)(dataset + 4) = PTP_OFC_Undefined; // 0x3000
    *(uint32_t *)(dataset + 8) = 0; // 0 bytes
    *(uint32_t *)(dataset + 38) = parent_handle;

    uint32_t name_len = 0;
    while (file_name[name_len]) name_len++;
    uint8_t *str_ptr = dataset + 52;
    *str_ptr++ = (uint8_t)(name_len + 1);
    for (uint32_t i = 0; i < name_len; i++) {
        *str_ptr++ = (uint8_t)file_name[i];
        *str_ptr++ = 0x00;
    }
    *str_ptr++ = 0x00;
    *str_ptr++ = 0x00;

    uint32_t dataset_len = (uint32_t)(str_ptr - dataset);
    res = mtp_send_data(session, PTP_OC_SendObjectInfo, dataset, dataset_len);
    if (res != 0) return res;

    ptp_container_t resp;
    res = mtp_recv_resp(session, &resp);
    if (res != 0) return res;

    uint32_t assigned_handle = resp.params[2];
    if (out_handle) *out_handle = assigned_handle;

    // Send SendObject with 0-byte payload
    res = mtp_send_cmd(session, PTP_OC_SendObject, 0, 0, 0, 0);
    if (res != 0) return res;

    res = mtp_send_data(session, PTP_OC_SendObject, NULL, 0);
    if (res != 0) return res;

    return mtp_recv_resp(session, &resp);
}

int mtp_ensure_bootmanager_dirs(mtp_session_t *session, uint32_t storage_id) {
    if (!session) return -1;
    log_info("MTP", "Checking for /BootManager/persistence/ on Android storage...");

    uint32_t handles[128];
    uint32_t count = 0;
    uint32_t bm_handle = 0;
    uint32_t pers_handle = 0;
    bool nomedia_found = false;

    // 1. Check root directory for BootManager
    if (mtp_get_object_handles(session, storage_id, PTP_OBJECT_HANDLE_ROOT, handles, 128, &count) == 0) {
        for (uint32_t i = 0; i < count; i++) {
            char name[64] = {0};
            if (mtp_get_object_info(session, handles[i], name, sizeof(name), NULL) == 0) {
                if (mtp_str_eq_nocase(name, "BootManager")) {
                    bm_handle = handles[i];
                    break;
                }
            }
        }
    }

    if (bm_handle == 0) {
        log_info("MTP", "Auto-creating /BootManager/ folder on phone...");
        if (mtp_create_folder(session, storage_id, PTP_OBJECT_HANDLE_ROOT, "BootManager", &bm_handle) != 0) {
            log_info("MTP", "Note: /BootManager/ folder auto-creation skipped or unsupported over MTP.");
            return 0;
        }
    }

    // 2. Check inside BootManager for persistence/
    if (bm_handle != 0) {
        count = 0;
        if (mtp_get_object_handles(session, storage_id, bm_handle, handles, 128, &count) == 0) {
            for (uint32_t i = 0; i < count; i++) {
                char name[64] = {0};
                if (mtp_get_object_info(session, handles[i], name, sizeof(name), NULL) == 0) {
                    if (mtp_str_eq_nocase(name, "persistence")) {
                        pers_handle = handles[i];
                        break;
                    }
                }
            }
        }

        if (pers_handle == 0) {
            log_info("MTP", "Auto-creating /BootManager/persistence/ folder on phone...");
            mtp_create_folder(session, storage_id, bm_handle, "persistence", &pers_handle);
        }
    }

    // 3. Check inside persistence/ for .nomedia
    if (pers_handle != 0) {
        count = 0;
        if (mtp_get_object_handles(session, storage_id, pers_handle, handles, 128, &count) == 0) {
            for (uint32_t i = 0; i < count; i++) {
                char name[64] = {0};
                if (mtp_get_object_info(session, handles[i], name, sizeof(name), NULL) == 0) {
                    if (mtp_str_eq_nocase(name, ".nomedia")) {
                        nomedia_found = true;
                        break;
                    }
                }
            }
        }

        if (!nomedia_found) {
            log_info("MTP", "Auto-generating .nomedia file in /BootManager/persistence/...");
            mtp_create_empty_file(session, storage_id, pers_handle, ".nomedia", NULL);
        }
        log_info("MTP", "Verified /BootManager/persistence/ (.nomedia protected)");
    }

    return 0;
}

static int mtp_write_file_payload(mtp_session_t *session, uint32_t storage_id, uint32_t parent_handle, const char *file_name, const void *data, uint32_t total_len) {
    if (!session || !file_name || !data || total_len == 0) return -1;

    // Check if file already exists in parent folder; delete if found
    uint32_t handles[128];
    uint32_t count = 0;
    if (mtp_get_object_handles(session, storage_id, parent_handle, handles, 128, &count) == 0) {
        for (uint32_t i = 0; i < count; i++) {
            char name[64] = {0};
            if (mtp_get_object_info(session, handles[i], name, sizeof(name), NULL) == 0) {
                if (mtp_str_eq_nocase(name, file_name)) {
                    ptp_container_t del_resp;
                    mtp_send_cmd(session, PTP_OC_DeleteObject, 1, handles[i], 0, 0);
                    mtp_recv_resp(session, &del_resp);
                    break;
                }
            }
        }
    }

    // Send SendObjectInfo
    int res = mtp_send_cmd(session, PTP_OC_SendObjectInfo, 2, storage_id, parent_handle, 0);
    if (res != 0) return res;

    uint8_t dataset[256];
    mtp_k_memset(dataset, 0, sizeof(dataset));
    *(uint32_t *)(dataset + 0) = storage_id;
    *(uint16_t *)(dataset + 4) = PTP_OFC_Undefined;
    *(uint32_t *)(dataset + 8) = total_len;
    *(uint32_t *)(dataset + 38) = parent_handle;

    uint32_t name_len = 0;
    while (file_name[name_len] && name_len < 60) name_len++;

    uint8_t *str_ptr = dataset + 52;
    *str_ptr++ = (uint8_t)(name_len + 1);
    for (uint32_t i = 0; i < name_len; i++) {
        *str_ptr++ = (uint8_t)file_name[i];
        *str_ptr++ = 0x00;
    }
    *str_ptr++ = 0x00;
    *str_ptr++ = 0x00;
    uint32_t dataset_len = (uint32_t)(str_ptr - dataset);

    res = mtp_send_data(session, PTP_OC_SendObjectInfo, dataset, dataset_len);
    if (res != 0) return res;

    ptp_container_t resp;
    res = mtp_recv_resp(session, &resp);
    if (res != 0) return res;

    // Send SendObject with payload
    res = mtp_send_cmd(session, PTP_OC_SendObject, 0, 0, 0, 0);
    if (res != 0) return res;

    res = mtp_send_data(session, PTP_OC_SendObject, data, total_len);
    if (res != 0) return res;

    return mtp_recv_resp(session, &resp);
}

int mtp_save_log_to_phone(mtp_session_t *session) {
    if (!session || !session->session_active || !session->usb_dev) return -1;

    static char log_buf[DISK_LOG_BUFFER_SIZE];
    uint32_t total_len = 0;
    disk_log_copy_linear(log_buf, sizeof(log_buf), &total_len);
    if (total_len == 0) return 0;

    // Dedicated per-boot session filename (preserved across flushes of the same boot)
    static char s_boot_session_file[64] = {0};
    if (s_boot_session_file[0] == '\0') {
        char ts[32] = {0};
        rtc_get_timestamp_str(ts, sizeof(ts));
        if (ts[0] && !mtp_str_starts_with_nocase(ts, "session_")) {
            snprintf(s_boot_session_file, sizeof(s_boot_session_file), "boot_%s.log", ts);
        } else {
            const char *fat_sess = disk_log_get_session_filename();
            if (fat_sess && fat_sess[0]) {
                snprintf(s_boot_session_file, sizeof(s_boot_session_file), "%s", fat_sess);
            } else {
                snprintf(s_boot_session_file, sizeof(s_boot_session_file), "boot_%u.log", disk_log_get_session_id());
            }
        }
    }

    uint32_t sids[8];
    uint32_t scnt = 0;
    if (mtp_get_storage_ids(session, sids, 8, &scnt) != 0 || scnt == 0) {
        return -1;
    }

    int overall_res = -1;
    for (uint32_t s = 0; s < scnt; s++) {
        uint32_t storage_id = sids[s];

        // 1. Locate or create /BootManager folder on phone
        uint32_t handles[128];
        uint32_t count = 0;
        uint32_t bm_handle = 0;
        uint32_t logs_handle = 0;

        if (mtp_get_object_handles(session, storage_id, PTP_OBJECT_HANDLE_ROOT, handles, 128, &count) == 0) {
            for (uint32_t i = 0; i < count; i++) {
                char name[64] = {0};
                if (mtp_get_object_info(session, handles[i], name, sizeof(name), NULL) == 0) {
                    if (mtp_str_eq_nocase(name, "BootManager")) {
                        bm_handle = handles[i];
                        break;
                    }
                }
            }
        }
        if (bm_handle == 0) {
            if (mtp_create_folder(session, storage_id, PTP_OBJECT_HANDLE_ROOT, "BootManager", &bm_handle) != 0) {
                continue;
            }
        }

        // 2. Locate or create /BootManager/logs folder on phone
        if (mtp_get_object_handles(session, storage_id, bm_handle, handles, 128, &count) == 0) {
            for (uint32_t i = 0; i < count; i++) {
                char name[64] = {0};
                if (mtp_get_object_info(session, handles[i], name, sizeof(name), NULL) == 0) {
                    if (mtp_str_eq_nocase(name, "logs")) {
                        logs_handle = handles[i];
                        break;
                    }
                }
            }
        }
        if (logs_handle == 0) {
            mtp_create_folder(session, storage_id, bm_handle, "logs", &logs_handle);
        }

        // 3. Write dedicated per-boot archive /BootManager/logs/<session_file>
        if (logs_handle != 0) {
            mtp_write_file_payload(session, storage_id, logs_handle, s_boot_session_file, log_buf, total_len);
        }

        // 4. Write active latest pointer /BootManager/boot.log
        int res = mtp_write_file_payload(session, storage_id, bm_handle, "boot.log", log_buf, total_len);
        if (res == 0) {
            overall_res = 0;
        }
    }

    if (overall_res == 0) {
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        log_info("MTP", "SUCCESS: Boot log saved to /BootManager/logs/%s and boot.log (%u bytes)!",
                 s_boot_session_file, total_len);
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        return 0;
    }

    log_error("MTP", "Failed to save boot log over MTP (error %d)", overall_res);
    return -1;
}

