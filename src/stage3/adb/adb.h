#ifndef ADB_H
#define ADB_H

#include <stdint.h>
#include <stdbool.h>
#include "../usb/usb.h"

#define A_CNXN 0x4e584e43 // "CNXN"
#define A_AUTH 0x48545541 // "AUTH"
#define A_OPEN 0x4e45504f // "OPEN"
#define A_OKAY 0x59414b4f // "OKAY"
#define A_CLSE 0x45534c43 // "CLSE"
#define A_WRTE 0x45545257 // "WRTE"

#define A_VERSION 0x01000000
#define A_MAXDATA 4096

#define ADB_AUTH_TOKEN        1
#define ADB_AUTH_SIGNATURE    2
#define ADB_AUTH_RSAPUBLICKEY 3

#pragma pack(push, 1)
typedef struct adb_message {
    uint32_t command;     // e.g. A_CNXN, A_OPEN, etc.
    uint32_t arg0;        // first argument
    uint32_t arg1;        // second argument
    uint32_t data_length; // length of payload
    uint32_t data_crc32;  // CRC32 / checksum of payload
    uint32_t magic;       // command ^ 0xFFFFFFFF
} adb_message_t;
#pragma pack(pop)

typedef struct adb_session {
    usb_device_t *usb_dev;
    uint8_t      adb_bulk_in_ep;
    uint8_t      adb_bulk_out_ep;
    uint32_t     local_id;
    uint32_t     remote_id;
    uint32_t     max_data;
    bool         is_connected;
} adb_session_t;

struct os_entry;

int  adb_init_session(usb_device_t *dev, adb_session_t *session);
void adb_set_public_key(const char *key_str, uint32_t len);
int  adb_execute_shell(adb_session_t *session, const char *cmd, char *out_buf, uint32_t max_len);
int  adb_probe_kernel_gadgets(adb_session_t *session, char *out_buf, uint32_t max_len);
int  adb_trigger_mass_storage(adb_session_t *session, const char *iso_path, const char *profile_path);
int  adb_update_mass_storage_file(adb_session_t *session, const char *iso_path, const char *profile_path);
int  adb_scan_persistence_profiles(adb_session_t *session, struct os_entry *entry);
int  adb_create_sparse_overlay(adb_session_t *session, const char *overlay_path, uint32_t size_gb);
int  adb_save_log_to_phone(adb_session_t *session);

#endif // ADB_H
