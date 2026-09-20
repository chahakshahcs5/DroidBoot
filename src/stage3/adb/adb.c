#include "adb.h"
#include "../core/printf.h"
#include "../../include/io.h"

static void adb_k_memset(void *dst, uint8_t val, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = val;
}

static uint32_t adb_calc_checksum(const void *data, uint32_t len) {
    if (!data || len == 0) return 0;
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < len; i++) {
        sum += p[i];
    }
    return sum;
}

static int adb_send_msg(adb_session_t *s, uint32_t cmd, uint32_t arg0, uint32_t arg1, const void *data, uint32_t data_len) {
    if (!s || !s->usb_dev) return -1;

    adb_message_t msg;
    msg.command = cmd;
    msg.arg0 = arg0;
    msg.arg1 = arg1;
    msg.data_length = data_len;
    msg.data_crc32 = adb_calc_checksum(data, data_len);
    msg.magic = cmd ^ 0xFFFFFFFF;

    uint32_t sent = 0;
    int res = usb_bulk_transfer(s->usb_dev, s->usb_dev->adb_bulk_out_ep, &msg, sizeof(msg), &sent);
    if (res != 0) return res;

    if (data && data_len > 0) {
        sent = 0;
        res = usb_bulk_transfer(s->usb_dev, s->usb_dev->adb_bulk_out_ep, (void *)data, data_len, &sent);
        if (res != 0) return res;
    }
    return 0;
}

static int adb_recv_msg(adb_session_t *s, adb_message_t *out_msg, void *out_data, uint32_t max_data_len, uint32_t *out_data_len) {
    if (!s || !s->usb_dev || !out_msg) return -1;

    uint32_t rec = 0;
    int res = usb_bulk_transfer(s->usb_dev, s->usb_dev->adb_bulk_in_ep, out_msg, sizeof(adb_message_t), &rec);
    if (res != 0 || rec < sizeof(adb_message_t)) return -1;

    if (out_msg->magic != (out_msg->command ^ 0xFFFFFFFF)) {
        log_error("ADB", "Corrupt message header received (magic mismatch)!");
        return -2;
    }

    if (out_data_len) *out_data_len = 0;

    if (out_msg->data_length > 0) {
        uint32_t to_read = out_msg->data_length;
        if (to_read > max_data_len) to_read = max_data_len;

        uint32_t total_read = 0;
        while (total_read < to_read) {
            uint32_t chunk_rec = 0;
            res = usb_bulk_transfer(s->usb_dev, s->usb_dev->adb_bulk_in_ep, (uint8_t *)out_data + total_read, to_read - total_read, &chunk_rec);
            if (res != 0 || chunk_rec == 0) break;
            total_read += chunk_rec;
        }
        if (out_data_len) *out_data_len = total_read;
    }

    return 0;
}

int adb_init_session(usb_device_t *dev, adb_session_t *session) {
    if (!dev || !session || !dev->has_adb) return -1;

    adb_k_memset(session, 0, sizeof(adb_session_t));
    session->usb_dev = dev;
    session->local_id = 1;
    session->max_data = A_MAXDATA;

    log_info("ADB", "Configuring USB endpoints for ADB communication...");
    int res = usb_configure_adb_endpoints(dev);
    if (res != 0) {
        log_error("ADB", "Failed to configure ADB endpoints (code %d)!", res);
        return res;
    }

    // Send A_CNXN packet to adbd
    const char *banner = "host::bootmanager\0";
    uint32_t banner_len = 18;

    log_info("ADB", "Initiating ADB Connection Handshake (A_CNXN)...");
    res = adb_send_msg(session, A_CNXN, A_VERSION, A_MAXDATA, banner, banner_len);
    if (res != 0) {
        log_error("ADB", "Failed to send A_CNXN to Android device!");
        return res;
    }

    adb_message_t resp;
    char resp_buf[256];
    uint32_t resp_len = 0;
    res = adb_recv_msg(session, &resp, resp_buf, sizeof(resp_buf) - 1, &resp_len);
    if (res != 0) {
        log_error("ADB", "No response received from adbd.");
        return res;
    }

    if (resp.command == A_CNXN) {
        if (resp_len < sizeof(resp_buf)) resp_buf[resp_len] = '\0';
        session->is_connected = true;
        log_info("ADB", "ADB Connection Established! Device: '%s'", resp_buf);
        return 0;
    } else if (resp.command == A_AUTH) {
        log_info("ADB", "ADB Authentication required (Allow USB Debugging on phone screen).");
        return 1;
    }

    log_info("ADB", "Unexpected command from adbd: 0x%08X", resp.command);
    return -3;
}

int adb_execute_shell(adb_session_t *session, const char *cmd, char *out_buf, uint32_t max_len) {
    if (!session || !session->is_connected || !cmd) return -1;

    char open_dest[256];
    snprintf(open_dest, sizeof(open_dest), "shell:%s", cmd);
    uint32_t dest_len = 0;
    while (open_dest[dest_len]) dest_len++;
    dest_len++; // include null terminator

    uint32_t my_id = session->local_id++;
    int res = adb_send_msg(session, A_OPEN, my_id, 0, open_dest, dest_len);
    if (res != 0) return res;

    adb_message_t resp;
    static char chunk_buf[1024];
    uint32_t chunk_len = 0;
    uint32_t total_out = 0;

    while (1) {
        res = adb_recv_msg(session, &resp, chunk_buf, sizeof(chunk_buf) - 1, &chunk_len);
        if (res != 0) break;

        if (resp.command == A_OKAY) {
            session->remote_id = resp.arg0;
        } else if (resp.command == A_WRTE) {
            session->remote_id = resp.arg0;
            if (out_buf && total_out < max_len - 1) {
                uint32_t to_copy = chunk_len;
                if (total_out + to_copy > max_len - 1) to_copy = max_len - 1 - total_out;
                for (uint32_t c = 0; c < to_copy; c++) {
                    out_buf[total_out + c] = chunk_buf[c];
                }
                total_out += to_copy;
                out_buf[total_out] = '\0';
            }
            // Acknowledge WRTE
            adb_send_msg(session, A_OKAY, my_id, session->remote_id, NULL, 0);
        } else if (resp.command == A_CLSE) {
            adb_send_msg(session, A_CLSE, my_id, session->remote_id, NULL, 0);
            break;
        }
    }

    if (out_buf && max_len > 0) {
        out_buf[total_out < max_len ? total_out : max_len - 1] = '\0';
    }
    return 0;
}

int adb_trigger_mass_storage(adb_session_t *session, const char *iso_path) {
    if (!session || !session->is_connected) return -1;

    log_info("ADB", "Triggering Root USB Gadget Mass Storage mode for: %s...",
             iso_path ? iso_path : "/sdcard/Download/*.iso");

    char cmd[512];
    if (iso_path && iso_path[0]) {
        snprintf(cmd, sizeof(cmd),
                 "su -c \"echo '%s' > /config/usb_gadget/g1/functions/mass_storage.0/lun.0/file 2>/dev/null || echo '%s' > /sys/class/android_usb/android0/f_mass_storage/lun/file 2>/dev/null; setprop sys.usb.config mass_storage\"",
                 iso_path, iso_path);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "su -c \"ISO=$(ls /sdcard/Download/*.iso /sdcard/Documents/*.iso 2>/dev/null | head -n 1); [ -n \\\"$ISO\\\" ] && echo \\\"$ISO\\\" > /config/usb_gadget/g1/functions/mass_storage.0/lun.0/file 2>/dev/null; setprop sys.usb.config mass_storage\"");
    }

    char out[256] = {0};
    int res = adb_execute_shell(session, cmd, out, sizeof(out));
    if (res == 0) {
        log_info("ADB", "USB Mass Storage gadget switch command sent to Android kernel.");
    } else {
        log_error("ADB", "Failed to execute gadget switch command over ADB!");
    }
    return res;
}

int adb_create_sparse_overlay(adb_session_t *session, const char *overlay_path, uint32_t size_gb) {
    if (!session || !session->is_connected || !overlay_path) return -1;

    log_info("ADB", "Allocating %u GB sparse overlay at '%s'...", size_gb, overlay_path);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "su -c \"mkdir -p /sdcard/BootManager/persistence && touch /sdcard/BootManager/persistence/.nomedia && truncate -s %uG %s && mkfs.ext4 -F -O ^has_journal %s\"",
             size_gb, overlay_path, overlay_path);

    char out[256] = {0};
    int res = adb_execute_shell(session, cmd, out, sizeof(out));
    if (res == 0) {
        log_info("ADB", "Sparse %u GB persistence overlay created successfully on phone storage!", size_gb);
    } else {
        log_error("ADB", "Failed to create sparse overlay over ADB!");
    }
    return res;
}
