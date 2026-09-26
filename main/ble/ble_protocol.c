#include "ble_protocol.h"

#include <string.h>

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint16_t ble_proto_crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xffffu;
    if (!data && len != 0) {
        return 0;
    }
    while (len--) {
        crc ^= (uint16_t)(*data++) << 8;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                   : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

uint32_t ble_proto_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xffffffffu;
    if (!data && len != 0) {
        return 0;
    }
    while (len--) {
        crc ^= *data++;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return ~crc;
}

static int valid_utf8_text(const uint8_t *text, size_t length)
{
    size_t i = 0;
    while (i < length) {
        const uint8_t lead = text[i];
        if (lead < 0x80u) {
            if (lead < 0x20u || lead == 0x7fu) return 0;
            ++i;
            continue;
        }
        unsigned extra;
        uint32_t codepoint;
        if (lead >= 0xc2u && lead <= 0xdfu) {
            extra = 1;
            codepoint = lead & 0x1fu;
        } else if (lead >= 0xe0u && lead <= 0xefu) {
            extra = 2;
            codepoint = lead & 0x0fu;
        } else if (lead >= 0xf0u && lead <= 0xf4u) {
            extra = 3;
            codepoint = lead & 0x07u;
        } else {
            return 0;
        }
        if (i + extra >= length) return 0;
        for (unsigned n = 1; n <= extra; ++n) {
            if ((text[i + n] & 0xc0u) != 0x80u) return 0;
            codepoint = (codepoint << 6) | (text[i + n] & 0x3fu);
        }
        if ((extra == 2 && codepoint < 0x800u) ||
            (extra == 3 && codepoint < 0x10000u) ||
            (codepoint >= 0xd800u && codepoint <= 0xdfffu) ||
            codepoint > 0x10ffffu) {
            return 0;
        }
        i += extra + 1;
    }
    return 1;
}

ble_proto_status_t ble_proto_decode_today_plan(const uint8_t *payload,
                                               size_t payload_len,
                                               char *out, size_t out_capacity)
{
    static const char none[] = "无";
    if ((!payload && payload_len != 0) || !out) return BLE_PROTO_ERR_ARG;
    if (payload_len > 30u || out_capacity < payload_len + 1u) {
        return BLE_PROTO_ERR_LENGTH;
    }
    if (payload_len == 0) {
        if (out_capacity < sizeof(none)) return BLE_PROTO_ERR_NOSPACE;
        memcpy(out, none, sizeof(none));
        return BLE_PROTO_OK;
    }
    if (!valid_utf8_text(payload, payload_len)) return BLE_PROTO_ERR_FORMAT;
    memcpy(out, payload, payload_len);
    out[payload_len] = '\0';
    return BLE_PROTO_OK;
}

ble_proto_status_t ble_proto_encode(const ble_frame_view_t *frame,
                                    uint8_t *out, size_t out_capacity,
                                    size_t *out_len)
{
    if (!frame || !out || !out_len || (frame->payload_len != 0 && !frame->payload)) {
        return BLE_PROTO_ERR_ARG;
    }
    if (frame->version != BLE_PROTO_VERSION) {
        return BLE_PROTO_ERR_VERSION;
    }
    if (frame->payload_len > BLE_PROTO_MAX_PAYLOAD) {
        return BLE_PROTO_ERR_LENGTH;
    }
    const size_t total = BLE_PROTO_HEADER_SIZE + frame->payload_len + BLE_PROTO_CRC_SIZE;
    if (out_capacity < total) {
        return BLE_PROTO_ERR_NOSPACE;
    }
    out[0] = BLE_PROTO_MAGIC0;
    out[1] = BLE_PROTO_MAGIC1;
    out[2] = frame->version;
    out[3] = frame->type;
    put_le16(out + 4, frame->sequence);
    put_le16(out + 6, frame->payload_len);
    if (frame->payload_len) {
        memcpy(out + BLE_PROTO_HEADER_SIZE, frame->payload, frame->payload_len);
    }
    put_le16(out + BLE_PROTO_HEADER_SIZE + frame->payload_len,
             ble_proto_crc16_ccitt(out, BLE_PROTO_HEADER_SIZE + frame->payload_len));
    *out_len = total;
    return BLE_PROTO_OK;
}

ble_proto_status_t ble_proto_decode(const uint8_t *data, size_t len,
                                    ble_frame_view_t *frame)
{
    if (!data || !frame) {
        return BLE_PROTO_ERR_ARG;
    }
    if (len < BLE_PROTO_HEADER_SIZE + BLE_PROTO_CRC_SIZE ||
        data[0] != BLE_PROTO_MAGIC0 || data[1] != BLE_PROTO_MAGIC1) {
        return BLE_PROTO_ERR_FORMAT;
    }
    if (data[2] != BLE_PROTO_VERSION) {
        return BLE_PROTO_ERR_VERSION;
    }
    const uint16_t payload_len = get_le16(data + 6);
    if (payload_len > BLE_PROTO_MAX_PAYLOAD) {
        return BLE_PROTO_ERR_LENGTH;
    }
    const size_t expected = BLE_PROTO_HEADER_SIZE + payload_len + BLE_PROTO_CRC_SIZE;
    if (len != expected) {
        return BLE_PROTO_ERR_LENGTH;
    }
    const uint16_t actual_crc = ble_proto_crc16_ccitt(data, expected - BLE_PROTO_CRC_SIZE);
    if (get_le16(data + expected - BLE_PROTO_CRC_SIZE) != actual_crc) {
        return BLE_PROTO_ERR_CRC;
    }
    frame->version = data[2];
    frame->type = data[3];
    frame->sequence = get_le16(data + 4);
    frame->payload_len = payload_len;
    frame->payload = data + BLE_PROTO_HEADER_SIZE;
    return BLE_PROTO_OK;
}

int ble_proto_seq_is_newer(uint16_t sequence, uint16_t last_sequence)
{
    const uint16_t delta = (uint16_t)(sequence - last_sequence);
    return delta != 0 && delta < 0x8000u;
}

void ble_proto_stream_init(ble_proto_stream_t *stream,
                           ble_proto_frame_cb_t callback, void *user_ctx)
{
    if (!stream) {
        return;
    }
    memset(stream, 0, sizeof(*stream));
    stream->callback = callback;
    stream->user_ctx = user_ctx;
}

static void stream_drop_prefix(ble_proto_stream_t *stream, size_t count)
{
    if (count >= stream->length) {
        stream->length = 0;
        return;
    }
    memmove(stream->buffer, stream->buffer + count, stream->length - count);
    stream->length -= count;
}

size_t ble_proto_stream_feed(ble_proto_stream_t *stream,
                             const uint8_t *data, size_t len)
{
    if (!stream || (!data && len != 0)) {
        return 0;
    }
    size_t delivered = 0;
    while (len) {
        const size_t space = sizeof(stream->buffer) - stream->length;
        if (space == 0) {
            stream_drop_prefix(stream, 1);
            stream->dropped_frames++;
        }
        const size_t take = (len < (sizeof(stream->buffer) - stream->length))
                                ? len : (sizeof(stream->buffer) - stream->length);
        memcpy(stream->buffer + stream->length, data, take);
        stream->length += take;
        data += take;
        len -= take;

        for (;;) {
            if (stream->length < 2) {
                break;
            }
            size_t start = 0;
            while (start + 1 < stream->length &&
                   (stream->buffer[start] != BLE_PROTO_MAGIC0 ||
                    stream->buffer[start + 1] != BLE_PROTO_MAGIC1)) {
                start++;
            }
            if (start != 0) {
                stream_drop_prefix(stream, start);
                stream->dropped_frames++;
            }
            if (stream->length < BLE_PROTO_HEADER_SIZE) {
                break;
            }
            const uint16_t payload_len = get_le16(stream->buffer + 6);
            const size_t frame_len = BLE_PROTO_HEADER_SIZE + payload_len + BLE_PROTO_CRC_SIZE;
            if (payload_len > BLE_PROTO_MAX_PAYLOAD || frame_len > sizeof(stream->buffer)) {
                stream_drop_prefix(stream, 2);
                stream->dropped_frames++;
                continue;
            }
            if (stream->length < frame_len) {
                break;
            }
            ble_frame_view_t frame;
            const ble_proto_status_t status = ble_proto_decode(stream->buffer, frame_len, &frame);
            if (status == BLE_PROTO_OK) {
                if (stream->callback) {
                    stream->callback(&frame, stream->user_ctx);
                }
                delivered++;
                stream_drop_prefix(stream, frame_len);
            } else {
                stream_drop_prefix(stream, 2);
                stream->dropped_frames++;
            }
        }
    }
    return delivered;
}
