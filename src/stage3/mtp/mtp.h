#ifndef MTP_H
#define MTP_H

#include <stdint.h>
#include <stdbool.h>
#include "../usb/usb.h"

// PTP / MTP Container Types
#define PTP_CONTAINER_TYPE_UNDEFINED    0x0000
#define PTP_CONTAINER_TYPE_COMMAND      0x0001
#define PTP_CONTAINER_TYPE_DATA         0x0002
#define PTP_CONTAINER_TYPE_RESPONSE     0x0003
#define PTP_CONTAINER_TYPE_EVENT        0x0004

// PTP / MTP Operation Codes
#define PTP_OC_GetDeviceInfo            0x1001
#define PTP_OC_OpenSession              0x1002
#define PTP_OC_CloseSession             0x1003
#define PTP_OC_GetStorageIDs            0x1004
#define PTP_OC_GetStorageInfo           0x1005
#define PTP_OC_GetNumObjects            0x1006
#define PTP_OC_GetObjectHandles         0x1007
#define PTP_OC_GetObjectInfo            0x1008
#define PTP_OC_GetObject                0x1009
#define PTP_OC_DeleteObject             0x100B
#define PTP_OC_SendObjectInfo           0x100C
#define PTP_OC_SendObject               0x100D
#define PTP_OC_GetPartialObject         0x101B

// PTP / MTP Response Codes
#define PTP_RC_OK                       0x2001
#define PTP_RC_GeneralError             0x2002
#define PTP_RC_SessionNotOpen           0x2003
#define PTP_RC_InvalidTransactionID     0x2004
#define PTP_RC_OperationNotSupported    0x2005
#define PTP_RC_ParameterNotSupported    0x2006
#define PTP_RC_IncompleteTransfer       0x2007
#define PTP_RC_InvalidStorageID         0x2008
#define PTP_RC_InvalidObjectHandle      0x2009
#define PTP_RC_SessionAlreadyOpen       0x201E

// Standard Handle Identifiers
#define PTP_OBJECT_HANDLE_ROOT          0xFFFFFFFF
#define PTP_OBJECT_FORMAT_ALL           0x00000000

#pragma pack(push, 1)

// Standard 12-byte PTP / MTP Container Header
typedef struct ptp_container {
    uint32_t length;            // Total length including header
    uint16_t type;              // Container Type (Command, Data, Response, Event)
    uint16_t code;              // Operation or Response Code
    uint32_t transaction_id;    // Transaction ID
    uint32_t params[5];         // Optional parameters (up to 5)
} ptp_container_t;

typedef struct mtp_session {
    usb_device_t *usb_dev;
    uint32_t session_id;
    uint32_t transaction_id;
    uint32_t active_storage_id;
    bool     session_active;
} mtp_session_t;

#pragma pack(pop)

#define PTP_OFC_Undefined               0x3000
#define PTP_OFC_Association             0x3001
#define PTP_AT_GenericFolder            0x0001

int  mtp_init_session(usb_device_t *dev, mtp_session_t *session);
int  mtp_get_storage_ids(mtp_session_t *session, uint32_t *storage_ids, uint32_t max_ids, uint32_t *out_count);
int  mtp_get_object_handles(mtp_session_t *session, uint32_t storage_id, uint32_t parent_handle, uint32_t *handles, uint32_t max_handles, uint32_t *out_count);
int  mtp_get_object_info(mtp_session_t *session, uint32_t handle, char *out_name, uint32_t max_name_len, uint64_t *out_size);
int  mtp_get_object(mtp_session_t *session, uint32_t handle, void *out_buf, uint32_t max_len, uint32_t *actual_len);
int  mtp_get_partial_object(mtp_session_t *session, uint32_t handle, uint32_t offset, uint32_t max_bytes, void *out_buf, uint32_t *actual_len);
int  mtp_create_folder(mtp_session_t *session, uint32_t storage_id, uint32_t parent_handle, const char *folder_name, uint32_t *out_handle);
int  mtp_create_empty_file(mtp_session_t *session, uint32_t storage_id, uint32_t parent_handle, const char *file_name, uint32_t *out_handle);
int  mtp_ensure_bootmanager_dirs(mtp_session_t *session, uint32_t storage_id);

#endif // MTP_H
