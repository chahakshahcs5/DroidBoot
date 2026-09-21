#include "adb.h"
#include "../core/printf.h"
#include "../debug/vga.h"
#include "../../include/io.h"
#include "../image/os_scanner.h"

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

    uint8_t ep = s->adb_bulk_out_ep;
    if (ep == 0 && s->usb_dev) ep = s->usb_dev->adb_bulk_out_ep;
    if (ep == 0) {
        log_error("ADB", "ADB Bulk OUT endpoint is invalid (0x00)!");
        return -1;
    }
    if (s->usb_dev && s->usb_dev->adb_bulk_out_ep == 0) s->usb_dev->adb_bulk_out_ep = ep;

    adb_message_t msg;
    msg.command = cmd;
    msg.arg0 = arg0;
    msg.arg1 = arg1;
    msg.data_length = data_len;
    msg.data_crc32 = adb_calc_checksum(data, data_len);
    msg.magic = cmd ^ 0xFFFFFFFF;

    uint32_t sent = 0;
    int res = usb_bulk_transfer(s->usb_dev, ep, &msg, sizeof(msg), &sent);
    if (res != 0) return res;

    if (data && data_len > 0) {
        sent = 0;
        res = usb_bulk_transfer(s->usb_dev, ep, (void *)data, data_len, &sent);
        if (res != 0) return res;
    }
    return 0;
}

static int adb_recv_msg(adb_session_t *s, adb_message_t *out_msg, void *out_data, uint32_t max_data_len, uint32_t *out_data_len) {
    if (!s || !s->usb_dev || !out_msg) return -1;

    uint8_t ep = s->adb_bulk_in_ep;
    if (ep == 0 && s->usb_dev) ep = s->usb_dev->adb_bulk_in_ep;
    if (ep == 0) {
        log_error("ADB", "ADB Bulk IN endpoint is invalid (0x00)!");
        return -1;
    }
    if (s->usb_dev && s->usb_dev->adb_bulk_in_ep == 0) s->usb_dev->adb_bulk_in_ep = ep;

    uint32_t rec = 0;
    int res = usb_bulk_transfer(s->usb_dev, ep, out_msg, sizeof(adb_message_t), &rec);
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
            res = usb_bulk_transfer(s->usb_dev, ep, (uint8_t *)out_data + total_read, to_read - total_read, &chunk_rec);
            if (res != 0 || chunk_rec == 0) break;
            total_read += chunk_rec;
        }
        if (out_data_len) *out_data_len = total_read;
    }

    return 0;
}

static int adb_recv_msg_wait(adb_session_t *s, adb_message_t *out_msg, void *out_data, uint32_t max_data_len, uint32_t *out_data_len, int max_seconds) {
    if (!s || !s->usb_dev || !out_msg) return -1;

    uint8_t ep = s->adb_bulk_in_ep;
    if (ep == 0 && s->usb_dev) ep = s->usb_dev->adb_bulk_in_ep;
    if (ep == 0) {
        log_error("ADB", "ADB Bulk IN endpoint is invalid (0x00)!");
        return -1;
    }
    if (s->usb_dev && s->usb_dev->adb_bulk_in_ep == 0) s->usb_dev->adb_bulk_in_ep = ep;

    uint32_t rec = 0;
    int res = usb_bulk_transfer_wait(s->usb_dev, ep, out_msg, sizeof(adb_message_t), &rec, max_seconds);
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
            res = usb_bulk_transfer_wait(s->usb_dev, ep, (uint8_t *)out_data + total_read, to_read - total_read, &chunk_rec, 2);
            if (res != 0 || chunk_rec == 0) break;
            total_read += chunk_rec;
        }
        if (out_data_len) *out_data_len = total_read;
    }

    return 0;
}

static char     g_adb_key_buffer[1024] = {0};
static uint32_t g_adb_key_len = 0;

void adb_set_public_key(const char *key_str, uint32_t len) {
    if (!key_str || len == 0 || len >= sizeof(g_adb_key_buffer)) return;
    for (uint32_t i = 0; i < len; i++) {
        g_adb_key_buffer[i] = key_str[i];
    }
    g_adb_key_buffer[len] = '\0';
    g_adb_key_len = len + 1; // include null terminator for ADB packet
    log_info("ADB", "Registered dynamic ADB public key (%u bytes).", len);
}

int adb_init_session(usb_device_t *dev, adb_session_t *session) {
    if (!dev || !session || !dev->has_adb) return -1;

    adb_k_memset(session, 0, sizeof(adb_session_t));
    session->usb_dev = dev;
    session->adb_bulk_in_ep = dev->adb_bulk_in_ep;
    session->adb_bulk_out_ep = dev->adb_bulk_out_ep;
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
        log_info("ADB", "ADB Authentication challenge received. Sending RSA Public Key...");
        const char *key_to_send = (g_adb_key_len > 0) ? g_adb_key_buffer : "bootmanager@baremetal\0";
        uint32_t key_len = (g_adb_key_len > 0) ? g_adb_key_len : 22;

        res = adb_send_msg(session, A_AUTH, ADB_AUTH_RSAPUBLICKEY, 0, key_to_send, key_len);
        if (res != 0) {
            log_error("ADB", "Failed to send RSA public key to adbd!");
            return res;
        }

        vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        log_info("ADB", ">>> PLEASE TAP 'ALWAYS ALLOW' ON YOUR PHONE SCREEN! <<<");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

        resp_len = 0;
        res = adb_recv_msg_wait(session, &resp, resp_buf, sizeof(resp_buf) - 1, &resp_len, 35);

        if (res == 0 && resp.command == A_CNXN) {
            if (resp_len < sizeof(resp_buf)) resp_buf[resp_len] = '\0';
            session->is_connected = true;
            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            log_info("ADB", "ADB Connection Authorized! Device: '%s'", resp_buf);
            vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
            return 0;
        } else if (res == 0 && resp.command == A_AUTH) {
            log_info("ADB", "Secondary auth requested by phone. Re-sending RSA public key...");
            adb_send_msg(session, A_AUTH, ADB_AUTH_RSAPUBLICKEY, 0, key_to_send, key_len);
            resp_len = 0;
            res = adb_recv_msg_wait(session, &resp, resp_buf, sizeof(resp_buf) - 1, &resp_len, 15);
            if (res == 0 && resp.command == A_CNXN) {
                if (resp_len < sizeof(resp_buf)) resp_buf[resp_len] = '\0';
                session->is_connected = true;
                vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                log_info("ADB", "ADB Connection Authorized! Device: '%s'", resp_buf);
                vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                return 0;
            }
        }

        log_error("ADB", "ADB authorization timed out after 35 seconds.");
        return 1;
    }

    log_info("ADB", "Unexpected command from adbd: 0x%08X", resp.command);
    return -3;
}

int adb_execute_shell(adb_session_t *session, const char *cmd, char *out_buf, uint32_t max_len) {
    if (!session || !session->is_connected || !cmd) return -1;

    static char open_dest[4096];
    snprintf(open_dest, sizeof(open_dest), "shell:%s", cmd);
    uint32_t dest_len = 0;
    while (open_dest[dest_len]) dest_len++;
    dest_len++; // include null terminator

    uint32_t my_id = session->local_id++;
    int res = adb_send_msg(session, A_OPEN, my_id, 0, open_dest, dest_len);
    if (res != 0) return res;

    adb_message_t resp;
    static char chunk_buf[4096];
    uint32_t chunk_len = 0;
    uint32_t total_out = 0;

    while (1) {
        // Allow up to 12 seconds for su / root shell to spawn and execute
        res = adb_recv_msg_wait(session, &resp, chunk_buf, sizeof(chunk_buf) - 1, &chunk_len, 12);
        if (res != 0) break;

        // Discard any stale packets from older closed streams
        if (resp.command != A_CNXN && resp.command != A_AUTH) {
            if (resp.arg1 != my_id) {
                continue;
            }
        }

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
    if (total_out == 0 && res != 0) {
        return res;
    }
    return 0;
}

static bool adb_str_contains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    for (int i = 0; haystack[i]; i++) {
        int j = 0;
        while (haystack[i + j] && needle[j] && haystack[i + j] == needle[j]) j++;
        if (!needle[j]) return true;
    }
    return false;
}

static bool adb_str_ends_with_nocase(const char *str, const char *suffix) {
    if (!str || !suffix) return false;
    int str_len = 0, suf_len = 0;
    while (str[str_len]) str_len++;
    while (suffix[suf_len]) suf_len++;
    if (str_len < suf_len) return false;
    for (int i = 0; i < suf_len; i++) {
        char c1 = str[str_len - suf_len + i];
        char c2 = suffix[i];
        if (c1 >= 'A' && c1 <= 'Z') c1 += ('a' - 'A');
        if (c2 >= 'A' && c2 <= 'Z') c2 += ('a' - 'A');
        if (c1 != c2) return false;
    }
    return true;
}

static bool adb_str_eq_nocase(const char *s1, const char *s2) {
    if (!s1 || !s2) return false;
    int i = 0;
    while (s1[i] && s2[i]) {
        char c1 = s1[i], c2 = s2[i];
        if (c1 >= 'A' && c1 <= 'Z') c1 += ('a' - 'A');
        if (c2 >= 'A' && c2 <= 'Z') c2 += ('a' - 'A');
        if (c1 != c2) return false;
        i++;
    }
    return (s1[i] == '\0' && s2[i] == '\0');
}

int adb_probe_kernel_gadgets(adb_session_t *session, char *out_buf, uint32_t max_len) {
    if (!session || !session->is_connected) return -1;

    const char *cmd = "su -c \"echo '[GADGET_FUNCS]'; ls /config/usb_gadget/g1/functions 2>/dev/null; echo '[SYSFS_USB]'; ls /sys/class/android_usb 2>/dev/null; echo '[UDC]'; cat /config/usb_gadget/g1/UDC 2>/dev/null || ls /sys/class/udc 2>/dev/null\"";
    return adb_execute_shell(session, cmd, out_buf, max_len);
}

int adb_trigger_mass_storage(adb_session_t *session, const char *iso_path, const char *profile_path) {
    if (!session || !session->is_connected) return -1;

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    log_info("ADB", "Probing phone root gadget subsystem (check phone screen if Magisk asks)...");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    // Stage 1: Non-destructive pre-flight check for kernel driver support
    static char check_cmd[1024];
    snprintf(check_cmd, sizeof(check_cmd),
             "su -c 'modprobe usb_f_mass_storage 2>/dev/null; "
             "for g in /config/usb_gadget/* /sys/kernel/config/usb_gadget/*; do "
             "  if [ -d \"$g/functions\" ]; then "
             "    if [ -d \"$g/functions/mass_storage.0\" ] || mkdir \"$g/functions/mass_storage.0\" 2>/dev/null; then "
             "      echo SUPP:CONFIGFS_0:$g; exit 0; "
             "    elif [ -d \"$g/functions/mass_storage.usb0\" ] || mkdir \"$g/functions/mass_storage.usb0\" 2>/dev/null; then "
             "      echo SUPP:CONFIGFS_USB0:$g; exit 0; "
             "    fi; "
             "  fi; "
             "done; "
             "if [ -d /sys/class/android_usb ]; then echo SUPP:SYSFS; exit 0; fi; "
             "echo ERR_NO_MSC_DRIVER'");

    char check_out[128] = {0};
    int res = adb_execute_shell(session, check_cmd, check_out, sizeof(check_out));
    if (res != 0 || check_out[0] == '\0') {
        // Retry once in case user just approved Magisk superuser prompt
        for (int w = 0; w < 1000000; w++) io_wait();
        check_out[0] = '\0';
        res = adb_execute_shell(session, check_cmd, check_out, sizeof(check_out));
    }

    if (res != 0) {
        log_error("ADB", "Failed to communicate with su shell over ADB (error %d)!", res);
        return -1;
    }

    log_info("ADB", "Pre-flight response: '%s' (code %d)", check_out[0] ? check_out : "(empty)", res);

    char gadget_dir[128] = "/config/usb_gadget/g1";
    const char *func_name = NULL;
    bool is_sysfs = false;

    if (adb_str_contains(check_out, "SUPP:CONFIGFS_0")) {
        func_name = "mass_storage.0";
        for (int i = 0; check_out[i]; i++) {
            if (check_out[i] == ':' && check_out[i+1] == '/') {
                int j = 0;
                char *col = &check_out[i+1];
                while (col[j] && col[j] != '\r' && col[j] != '\n' && col[j] != ' ' && j < 127) {
                    gadget_dir[j] = col[j];
                    j++;
                }
                gadget_dir[j] = '\0';
                break;
            }
        }
    } else if (adb_str_contains(check_out, "SUPP:CONFIGFS_USB0")) {
        func_name = "mass_storage.usb0";
        for (int i = 0; check_out[i]; i++) {
            if (check_out[i] == ':' && check_out[i+1] == '/') {
                int j = 0;
                char *col = &check_out[i+1];
                while (col[j] && col[j] != '\r' && col[j] != '\n' && col[j] != ' ' && j < 127) {
                    gadget_dir[j] = col[j];
                    j++;
                }
                gadget_dir[j] = '\0';
                break;
            }
        }
    } else if (adb_str_contains(check_out, "SUPP:SYSFS")) {
        is_sysfs = true;
    } else {
        log_info("ADB", "Phone kernel does not have USB Mass Storage driver enabled (resp: '%s').",
                 check_out[0] ? check_out : "unsupported");
        log_info("ADB", "MTP & ADB left active. (To enable UMS, install Magisk UMS module or DriveDroid).");
        return -2; // Unsupported, but harmless! Phone stays connected!
    }

    log_info("ADB", "Kernel UMS support verified via %s (%s)! Locating target image...",
             is_sysfs ? "sysfs" : func_name, is_sysfs ? "/sys/class/android_usb" : gadget_dir);

    // Delay between pre-flight and switch to let ADB stream fully close
    for (int w = 0; w < 500000; w++) io_wait();

    // Stage 2: Configure LUNs (LUN 0 = Boot OS, LUN 1 = Persistent Data Profile) and initiate atomic switch
    static char switch_cmd[4096];
    const char *req_name = (iso_path && iso_path[0]) ? iso_path : "";
    const char *prof_name = (profile_path && profile_path[0]) ? profile_path : "";
    bool is_raw_img = (req_name[0] && adb_str_ends_with_nocase(req_name, ".img"));
    int ro_val = is_raw_img ? 0 : 1;

    if (!is_sysfs) {
        snprintf(switch_cmd, sizeof(switch_cmd),
                 "su -c '"
                 "ISO=\"%s\"; PROF=\"%s\"; "
                 "F=$(find /sdcard /storage/emulated/0 /storage -name \"$ISO\" 2>/dev/null | head -1); "
                 "[ -z \"$F\" ] && [ -f \"/sdcard/Download/$ISO\" ] && F=\"/sdcard/Download/$ISO\"; "
                 "[ -z \"$F\" ] && [ -f \"/sdcard/$ISO\" ] && F=\"/sdcard/$ISO\"; "
                 "if [ -z \"$F\" ] || [ ! -f \"$F\" ]; then echo ERR_NO_FILE; exit 2; fi; "
                 "L0=\"%s/functions/%s/lun.0\"; "
                 "mkdir -p \"$L0\" 2>/dev/null; "
                 "echo \"$F\" > \"$L0/file\" 2>/dev/null; "
                 "echo %d > \"$L0/ro\" 2>/dev/null; "
                 "echo 0 > \"$L0/cdrom\" 2>/dev/null; "
                 "if [ ! -s \"$L0/file\" ]; then echo ERR_LUN_FAIL; exit 3; fi; "
                 "L1=\"%s/functions/%s/lun.1\"; "
                 "if [ -n \"$PROF\" ] && [ -f \"$PROF\" ]; then "
                 "  mkdir -p \"$L1\" 2>/dev/null; "
                 "  echo \"$PROF\" > \"$L1/file\" 2>/dev/null; "
                 "  echo 0 > \"$L1/ro\" 2>/dev/null; "
                 "  echo 0 > \"$L1/cdrom\" 2>/dev/null; "
                 "fi; "
                 "C=$(ls -d %s/configs/*.* 2>/dev/null | head -1); "
                 "U=$(cat %s/UDC 2>/dev/null); "
                 "[ -z \"$U\" ] && U=$(ls /sys/class/udc 2>/dev/null | head -1); "
                 "if [ -z \"$C\" ] || [ -z \"$U\" ]; then echo ERR_NO_UDC; exit 4; fi; "
                 "echo OK_SWITCHING:\"$F\"; "
                 "(sleep 2; echo \"\" > %s/UDC 2>/dev/null; rm -f \"$C\"/f* 2>/dev/null; "
                 "ln -s %s/functions/%s \"$C/f1\" 2>/dev/null; "
                 "[ -d %s/functions/ffs.adb ] && ln -s %s/functions/ffs.adb \"$C/f2\" 2>/dev/null; "
                 "setprop sys.usb.state mass_storage,adb 2>/dev/null || setprop sys.usb.state mass_storage 2>/dev/null; "
                 "echo \"$U\" > %s/UDC 2>/dev/null) &"
                 "'",
                 req_name, prof_name, gadget_dir, func_name, ro_val,
                 gadget_dir, func_name, gadget_dir, gadget_dir,
                 gadget_dir, gadget_dir, func_name, gadget_dir, gadget_dir, gadget_dir);
    } else {
        snprintf(switch_cmd, sizeof(switch_cmd),
                 "su -c '"
                 "ISO=\"%s\"; PROF=\"%s\"; "
                 "F=$(find /sdcard /storage/emulated/0 /storage -name \"$ISO\" 2>/dev/null | head -1); "
                 "[ -z \"$F\" ] && [ -f \"/sdcard/Download/$ISO\" ] && F=\"/sdcard/Download/$ISO\"; "
                 "[ -z \"$F\" ] && [ -f \"/sdcard/$ISO\" ] && F=\"/sdcard/$ISO\"; "
                 "if [ -z \"$F\" ] || [ ! -f \"$F\" ]; then echo ERR_NO_FILE; exit 2; fi; "
                 "L=\"\"; "
                 "[ -f /sys/class/android_usb/android0/f_mass_storage/lun/file ] && L=\"/sys/class/android_usb/android0/f_mass_storage/lun\"; "
                 "[ -f /sys/class/android_usb/android0/f_mass_storage/lun0/file ] && L=\"/sys/class/android_usb/android0/f_mass_storage/lun0\"; "
                 "[ -f /sys/devices/virtual/android_usb/android0/f_mass_storage/lun/file ] && L=\"/sys/devices/virtual/android_usb/android0/f_mass_storage/lun\"; "
                 "[ -f /sys/devices/virtual/android_usb/android0/f_mass_storage/lun0/file ] && L=\"/sys/devices/virtual/android_usb/android0/f_mass_storage/lun0\"; "
                 "if [ -z \"$L\" ]; then echo ERR_NO_SYSFS_LUN; exit 3; fi; "
                 "echo OK_SWITCHING:\"$F\"; "
                 "(sleep 2; echo 0 > /sys/class/android_usb/android0/enable 2>/dev/null; "
                 "echo \"$F\" > \"$L/file\" 2>/dev/null; echo %d > \"$L/ro\" 2>/dev/null; echo 0 > \"$L/cdrom\" 2>/dev/null; "
                 "if [ -n \"$PROF\" ] && [ -f \"$PROF\" ]; then "
                 "  echo \"$PROF\" > \"$L/../lun1/file\" 2>/dev/null; "
                 "  echo 0 > \"$L/../lun1/ro\" 2>/dev/null; "
                 "fi; "
                 "echo mass_storage,adb > /sys/class/android_usb/android0/functions 2>/dev/null || echo mass_storage > /sys/class/android_usb/android0/functions 2>/dev/null; "
                 "echo 1 > /sys/class/android_usb/android0/enable 2>/dev/null) &"
                 "'",
                 req_name, prof_name, ro_val);
    }

    char switch_out[256] = {0};
    res = adb_execute_shell(session, switch_cmd, switch_out, sizeof(switch_out));

    log_info("ADB", "Switch command result: code=%d, resp='%s'", res, switch_out[0] ? switch_out : "(empty)");

    // Check for explicit file-not-found error (this comes before any USB disruption)
    if (adb_str_contains(switch_out, "ERR_NO_FILE")) {
        log_error("ADB", "Target image '%s' not found on phone storage!", req_name[0] ? req_name : "*.iso");
        return -3;
    }
    if (adb_str_contains(switch_out, "ERR_LUN_FAIL")) {
        log_error("ADB", "Failed to attach image to phone LUN device!");
        return -4;
    }
    if (adb_str_contains(switch_out, "ERR_NO_UDC")) {
        log_error("ADB", "No USB Device Controller (UDC) found on phone!");
        return -4;
    }

    if (res == 0 && adb_str_contains(switch_out, "OK_SWITCHING")) {
        log_info("ADB", "Mass Storage gadget switch armed! Target: %s", switch_out);
        session->is_connected = false;
        return 0; // Success!
    }

    if (adb_str_contains(switch_out, "OK_SWITCHING") || res < 0 || res == 4) {
        log_info("ADB", "Gadget switch in progress (USB bus disruption expected during UDC rebind).");
        log_info("ADB", "Phone background job will complete UDC switch in ~2 seconds.");
        session->is_connected = false;
        return 0; // Treat as success — phone is switching!
    }

    log_info("ADB", "Pre-flight confirmed UMS support; assuming switch is in progress.");
    session->is_connected = false;
    return 0;
}

int adb_update_mass_storage_file(adb_session_t *session, const char *iso_path, const char *profile_path) {
    if (!session || !session->is_connected) return -1;

    char req_name[128] = {0};
    if (iso_path) {
        const char *p = iso_path;
        for (const char *s = iso_path; *s; s++) {
            if (*s == '/' || *s == '\\') p = s + 1;
        }
        for (int i = 0; p[i] && i < 127; i++) req_name[i] = p[i];
    }

    const char *prof_name = (profile_path && profile_path[0]) ? profile_path : "";
    bool is_raw_img = (req_name[0] && adb_str_ends_with_nocase(req_name, ".img"));
    int ro_val = is_raw_img ? 0 : 1;

    log_info("ADB", "Hot-swapping USB Mass Storage: LUN0='%s' (ro=%d), LUN1='%s'...",
             req_name[0] ? req_name : "*.iso", ro_val, prof_name[0] ? prof_name : "(none)");

    static char update_cmd[4096];
    snprintf(update_cmd, sizeof(update_cmd),
             "su -c '"
             "ISO=\"%s\"; PROF=\"%s\"; RO=%d; "
             "F=$(find /sdcard /storage/emulated/0 -name \"$ISO\" 2>/dev/null | head -1); "
             "[ -z \"$F\" ] && [ -f \"/sdcard/Download/$ISO\" ] && F=\"/sdcard/Download/$ISO\"; "
             "[ -z \"$F\" ] && [ -f \"/sdcard/$ISO\" ] && F=\"/sdcard/$ISO\"; "
             "if [ -z \"$F\" ] || [ ! -f \"$F\" ]; then echo ERR_NO_FILE; exit 2; fi; "
             "DONE=0; "
             "for M in /config/usb_gadget/*/functions/mass_storage* /sys/kernel/config/usb_gadget/*/functions/mass_storage*; do "
             "  if [ -d \"$M\" ]; then "
             "    echo \"\" > \"$M/lun.0/file\" 2>/dev/null; "
             "    echo \"$F\" > \"$M/lun.0/file\" 2>/dev/null; "
             "    echo $RO > \"$M/lun.0/ro\" 2>/dev/null; "
             "    if [ -n \"$PROF\" ] && [ -f \"$PROF\" ]; then "
             "      mkdir -p \"$M/lun.1\" 2>/dev/null; "
             "      echo \"\" > \"$M/lun.1/file\" 2>/dev/null; "
             "      echo \"$PROF\" > \"$M/lun.1/file\" 2>/dev/null; "
             "      echo 0 > \"$M/lun.1/ro\" 2>/dev/null; "
             "      echo 0 > \"$M/lun.1/cdrom\" 2>/dev/null; "
             "    else "
             "      echo \"\" > \"$M/lun.1/file\" 2>/dev/null; "
             "    fi; "
             "    DONE=1; "
             "    break; "
             "  fi; "
             "done; "
             "if [ $DONE -eq 0 ]; then "
             "  for L in /sys/class/android_usb/android0/f_mass_storage/lun /sys/class/android_usb/android0/f_mass_storage/lun0; do "
             "    if [ -f \"$L/file\" ]; then "
             "      echo \"\" > \"$L/file\" 2>/dev/null; "
             "      echo \"$F\" > \"$L/file\" 2>/dev/null; "
             "      echo $RO > \"$L/ro\" 2>/dev/null; "
             "      if [ -n \"$PROF\" ] && [ -f \"$PROF\" ]; then "
             "        echo \"\" > \"$L/../lun1/file\" 2>/dev/null; "
             "        echo \"$PROF\" > \"$L/../lun1/file\" 2>/dev/null; "
             "        echo 0 > \"$L/../lun1/ro\" 2>/dev/null; "
             "      fi; "
             "      DONE=1; "
             "      break; "
             "    fi; "
             "  done; "
             "fi; "
             "if [ $DONE -eq 1 ]; then echo OK_UPDATED:\"$F\"; exit 0; else echo ERR_NO_LUN; exit 3; fi;'",
             req_name, prof_name, ro_val);

    char out[256] = {0};
    int res = adb_execute_shell(session, update_cmd, out, sizeof(out));
    log_info("ADB", "Hot-swap result: code=%d, resp='%s'", res, out[0] ? out : "(empty)");

    if (adb_str_contains(out, "OK_UPDATED")) {
        log_info("ADB", "USB Mass Storage backing file successfully updated: %s", out);
        return 0;
    }
    if (adb_str_contains(out, "ERR_NO_FILE")) {
        log_error("ADB", "Target image '%s' not found on phone storage!", req_name);
        return -2;
    }
    return -1;
}

int adb_scan_persistence_profiles(adb_session_t *session, struct os_entry *entry) {
    if (!session || !session->is_connected || !entry) return -1;

    static char scan_cmd[] =
        "su -c 'mkdir -p /sdcard/BootManager/persistence && touch /sdcard/BootManager/persistence/.nomedia && "
        "ls -1 /sdcard/BootManager/persistence/*.img /sdcard/BootManager/persistence/*.casper-rw /sdcard/BootManager/persistence/*.apkovl.tar.gz 2>/dev/null'";

    char out_buf[1024] = {0};
    int res = adb_execute_shell(session, scan_cmd, out_buf, sizeof(out_buf));
    if (res != 0 || out_buf[0] == '\0') {
        return 0;
    }

    char *p = out_buf;
    while (*p) {
        while (*p == '\r' || *p == '\n' || *p == ' ') p++;
        if (!*p) break;
        char *line_start = p;
        while (*p && *p != '\r' && *p != '\n') p++;
        if (*p) {
            *p = '\0';
            p++;
        }

        char *base = line_start;
        for (char *s = line_start; *s; s++) {
            if (*s == '/' || *s == '\\') base = s + 1;
        }

        if (base[0] && base[0] != '*') {
            bool exists = false;
            for (uint32_t i = 0; i < entry->profile_count; i++) {
                if (adb_str_eq_nocase(entry->profiles[i].filename, base)) {
                    exists = true;
                    break;
                }
            }
            if (!exists && entry->profile_count < MAX_PERSISTENCE_PROFILES) {
                char prof_title[32];
                snprintf(prof_title, sizeof(prof_title), "Profile: %s", base);
                os_add_custom_profile(entry, prof_title, base, (uint64_t)2 * 1024 * 1024 * 1024);
                log_info("ADB", "[+] Discovered profile on phone: '%s'", base);
            }
        }
    }
    return (int)entry->profile_count;
}

int adb_create_sparse_overlay(adb_session_t *session, const char *overlay_path, uint32_t size_gb) {
    if (!session || !session->is_connected || !overlay_path) return -1;

    log_info("ADB", "Allocating %u GB sparse overlay at '%s'...", size_gb, overlay_path);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "su -c 'mkdir -p /sdcard/BootManager/persistence && touch /sdcard/BootManager/persistence/.nomedia && "
             "truncate -s %uG \"%s\" && "
             "(mkfs.ext4 -F \"%s\" 2>/dev/null || mke2fs -F \"%s\" 2>/dev/null || make_ext4fs -l %uM \"%s\" 2>/dev/null || mkfs.vfat \"%s\" 2>/dev/null || true)'",
             size_gb, overlay_path, overlay_path, overlay_path, size_gb * 1024, overlay_path, overlay_path);

    char out[256] = {0};
    int res = adb_execute_shell(session, cmd, out, sizeof(out));
    if (res == 0) {
        log_info("ADB", "Sparse %u GB persistence overlay created successfully on phone storage!", size_gb);
    } else {
        log_error("ADB", "Failed to create sparse overlay over ADB!");
    }
    return res;
}
