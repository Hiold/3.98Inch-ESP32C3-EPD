#ifndef BLE_PROTOCOL_H
#define BLE_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_PROTO_MAGIC0 0x45u
#define BLE_PROTO_MAGIC1 0x50u
#define BLE_PROTO_VERSION 1u
#define BLE_PROTO_HEADER_SIZE 8u
#define BLE_PROTO_CRC_SIZE 2u
#define BLE_PROTO_MAX_PAYLOAD 1024u
#define BLE_PROTO_MAX_FRAME (BLE_PROTO_HEADER_SIZE + BLE_PROTO_MAX_PAYLOAD + BLE_PROTO_CRC_SIZE)

typedef enum {
    BLE_CMD_GET_INFO = 1,
    BLE_CMD_SET_ROTATION,
    BLE_CMD_SET_MODE,
    BLE_CMD_REFRESH,
    BLE_CMD_NEXT_PHOTO,
    BLE_CMD_SET_TIMEZONE,
    BLE_CMD_SET_LOCATION,
    BLE_CMD_WIFI_SET_CREDENTIALS,
    BLE_CMD_WEATHER_REFRESH,
    BLE_CMD_PHOTO_BEGIN,
    BLE_CMD_PHOTO_DATA,
    BLE_CMD_PHOTO_END,
    BLE_CMD_PHOTO_ABORT,
    BLE_CMD_SET_TODAY_PLAN = 14,
} ble_command_type_t;

/* Notify response type is request type | BLE_PROTO_RESPONSE_BIT. */
#define BLE_PROTO_RESPONSE_BIT 0x80u

typedef struct {
    uint8_t version;
    uint8_t type;
    uint16_t sequence;
    uint16_t payload_len;
    const uint8_t *payload;
} ble_frame_view_t;

typedef enum {
    BLE_PROTO_OK = 0,
    BLE_PROTO_ERR_ARG = -1,
    BLE_PROTO_ERR_NOSPACE = -2,
    BLE_PROTO_ERR_FORMAT = -3,
    BLE_PROTO_ERR_VERSION = -4,
    BLE_PROTO_ERR_LENGTH = -5,
    BLE_PROTO_ERR_CRC = -6,
} ble_proto_status_t;

uint16_t ble_proto_crc16_ccitt(const uint8_t *data, size_t len);
uint32_t ble_proto_crc32(const uint8_t *data, size_t len);

ble_proto_status_t ble_proto_encode(const ble_frame_view_t *frame,
                                    uint8_t *out, size_t out_capacity,
                                    size_t *out_len);
ble_proto_status_t ble_proto_decode(const uint8_t *data, size_t len,
                                    ble_frame_view_t *frame);

/* SET_TODAY_PLAN payload is un-terminated UTF-8, at most 30 bytes. An empty
 * payload clears the plan and is normalized to the display default "无". */
ble_proto_status_t ble_proto_decode_today_plan(const uint8_t *payload,
                                               size_t payload_len,
                                               char *out, size_t out_capacity);

/* Sequence comparison is modulo 16 bits; equal is not newer. */
int ble_proto_seq_is_newer(uint16_t sequence, uint16_t last_sequence);

typedef void (*ble_proto_frame_cb_t)(const ble_frame_view_t *frame,
                                     void *user_ctx);

typedef struct {
    uint8_t buffer[BLE_PROTO_MAX_FRAME];
    size_t length;
    ble_proto_frame_cb_t callback;
    void *user_ctx;
    uint32_t dropped_frames;
} ble_proto_stream_t;

void ble_proto_stream_init(ble_proto_stream_t *stream,
                           ble_proto_frame_cb_t callback, void *user_ctx);
/* Accepts arbitrary BLE writes; complete frames are delivered in order. */
size_t ble_proto_stream_feed(ble_proto_stream_t *stream,
                             const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
