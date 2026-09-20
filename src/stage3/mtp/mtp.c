#include "mtp.h"
#include "../xhci/xhci_regs.h"
#include "../../include/io.h"
#include "../core/printf.h"
#include "../memory/memory.h"
#include <stddef.h>

#define BULK_RING_TRBS 64

int usb_bulk_transfer(usb_device_t *dev, uint8_t ep_addr, void *data, uint32_t len, uint32_t *transferred_out) {
    if (!dev || !data || len == 0) return -1;

    xhci_controller_t *ctrl = dev->ctrl;
    uint8_t slot_id = dev->slot_id;

    // Determine target endpoint index for Doorbell:
    // EP0 = 1, EP1 OUT = 2, EP1 IN = 3, EP2 OUT = 4, EP2 IN = 5, ...
    // Formula: target = (ep_num * 2) + (is_in ? 1 : 0)
    uint8_t ep_num = ep_addr & 0x0F;
    bool is_in = (ep_addr & 0x80) != 0;
    uint32_t doorbell_target = (ep_num * 2) + (is_in ? 1 : 0);

    xhci_trb_t *ring = is_in ? dev->bulk_in_ring : dev->bulk_out_ring;
    uint32_t *enqueue_idx_ptr = is_in ? &dev->bulk_in_enqueue_idx : &dev->bulk_out_enqueue_idx;
    uint8_t *cycle_ptr = is_in ? &dev->bulk_in_cycle_state : &dev->bulk_out_cycle_state;

    if (!ring) {
        log_error("MTP", "Bulk endpoint 0x%02X ring not initialized!", ep_addr);
        return -2;
    }

    uint32_t idx = *enqueue_idx_ptr;
    xhci_trb_t *trb = &ring[idx++];
    if (idx >= BULK_RING_TRBS - 1) {
        idx = 0;
        *cycle_ptr ^= 1;
    }
    *enqueue_idx_ptr = idx;

    trb->parameter = (uintptr_t)data;
    trb->status = len;
    trb->control = TRB_TYPE(TRB_NORMAL) | TRB_IOC | (*cycle_ptr ? 1U : 0U);

    uintptr_t trb_phys = (uintptr_t)trb;

    // Ring endpoint doorbell
    uintptr_t db_reg = ctrl->db_regs + slot_id * 4;
    xhci_write32(db_reg, doorbell_target);

    // Poll Event Ring for Transfer Event
    int timeout = 20000;
    while (--timeout > 0) {
        xhci_trb_t *evt = &ctrl->event_ring[ctrl->event_dequeue_idx];
        uint32_t cycle = evt->control & 1U;

        if (cycle == ctrl->event_cycle_state) {
            uint32_t type = (evt->control >> TRB_TYPE_SHIFT) & 0x3F;
            uint8_t cc = (uint8_t)((evt->status >> 24) & 0xFF);
            log_info("MTP", "  Event: Type=%u, CC=%u, Param=0x%08X (expect=0x%08X)",
                     type, cc, (uint32_t)evt->parameter, (uint32_t)trb_phys);
            if (type == TRB_TRANSFER_EVENT) {
                if (evt->parameter == trb_phys) {
                    ctrl->event_dequeue_idx++;
                    if (ctrl->event_dequeue_idx == XHCI_EVENT_RING_TRBS) {
                        ctrl->event_dequeue_idx = 0;
                        ctrl->event_cycle_state ^= 1;
                    }
                    uintptr_t intr0 = ctrl->rt_regs + 0x20;
                    uintptr_t erdp = (uintptr_t)&ctrl->event_ring[ctrl->event_dequeue_idx];
                    xhci_write64(intr0 + XHCI_INTR_ERDP, erdp | XHCI_ERDP_EHB);

                    uint8_t cc = (uint8_t)((evt->status >> 24) & 0xFF);
                    uint32_t rem = evt->status & 0xFFFFFF;
                    if (transferred_out) {
                        *transferred_out = len - rem;
                    }
                    return (cc == TRB_COMPL_SUCCESS || cc == TRB_COMPL_SHORT_TX) ? 0 : (int)cc;
                }
            }

            ctrl->event_dequeue_idx++;
            if (ctrl->event_dequeue_idx == XHCI_EVENT_RING_TRBS) {
                ctrl->event_dequeue_idx = 0;
                ctrl->event_cycle_state ^= 1;
            }
            uintptr_t intr0 = ctrl->rt_regs + 0x20;
            uintptr_t erdp = (uintptr_t)&ctrl->event_ring[ctrl->event_dequeue_idx];
            xhci_write64(intr0 + XHCI_INTR_ERDP, erdp | XHCI_ERDP_EHB);
        }
        for (int w = 0; w < 50; w++) io_wait();
    }

    log_error("MTP", "Bulk transfer timed out on EP 0x%02X!", ep_addr);
    return -100;
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

    uint32_t sent = 0;
    return usb_bulk_transfer(s->usb_dev, s->usb_dev->mtp_bulk_out_ep, &cmd, cmd.length, &sent);
}

static int mtp_recv_resp(mtp_session_t *s, ptp_container_t *resp) {
    uint32_t received = 0;
    int res = usb_bulk_transfer(s->usb_dev, s->usb_dev->mtp_bulk_in_ep, resp, sizeof(ptp_container_t), &received);
    if (res != 0) return res;

    if (resp->type != PTP_CONTAINER_TYPE_RESPONSE) {
        log_error("MTP", "Expected Response container, got type %u", resp->type);
        return -1;
    }
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
            out_name[i] = (char)(chars[i] & 0x7F);
        }
        out_name[i] = '\0';
    }

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
        if (to_read > 4096) to_read = 4096;
        if (total_read + to_read > max_len) to_read = max_len - total_read;

        uint32_t chunk_received = 0;
        res = usb_bulk_transfer(session->usb_dev, session->usb_dev->mtp_bulk_in_ep, dst + total_read, to_read, &chunk_received);
        if (res != 0) break;
        total_read += chunk_received;
        if (chunk_received == 0) break;
    }

    if (actual_len) *actual_len = total_read;

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
        if (to_read > 4096) to_read = 4096;
        if (total_read + to_read > max_bytes) to_read = max_bytes - total_read;

        uint32_t chunk_received = 0;
        res = usb_bulk_transfer(session->usb_dev, session->usb_dev->mtp_bulk_in_ep, dst + total_read, to_read, &chunk_received);
        if (res != 0) break;
        total_read += chunk_received;
        if (chunk_received == 0) break;
    }

    if (actual_len) *actual_len = total_read;

    ptp_container_t resp;
    return mtp_recv_resp(session, &resp);
}
