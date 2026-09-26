#include "ble_service.h"

#include <NimBLEDevice.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

static const char *TAG = "ble_service";
static constexpr uint16_t ATTRIBUTE_MAX_LEN = 512;

static ble_gatt_t s_gatt;
static ble_proto_stream_t s_control_stream;
static ble_proto_stream_t s_image_stream;
static NimBLECharacteristic *s_notify_characteristic;
static uint16_t s_connection_handle = BLE_HS_CONN_HANDLE_NONE;
static bool s_ble_failed;

struct StreamEndpoint {
    bool image;
};
static StreamEndpoint s_control_endpoint = {false};
static StreamEndpoint s_image_endpoint = {true};

static void dispatch_frame(const ble_frame_view_t *frame, void *ctx)
{
    const StreamEndpoint *endpoint = static_cast<const StreamEndpoint *>(ctx);
    if (!frame || !endpoint) return;
    if (endpoint->image != (frame->type == BLE_CMD_PHOTO_DATA)) {
        ESP_LOGW(TAG, "drop command 0x%02x from wrong RX characteristic",
                 frame->type);
        return;
    }
    ble_gatt_dispatch_frame(frame);
}

class ProtocolWriteCallbacks final : public NimBLECharacteristicCallbacks {
public:
    explicit ProtocolWriteCallbacks(ble_proto_stream_t *stream) : stream_(stream) {}

    void onWrite(NimBLECharacteristic *characteristic,
                 NimBLEConnInfo &connInfo) override
    {
        (void)connInfo;
        if (!characteristic) return;
        const NimBLEAttValue value = characteristic->getValue();
        if (value.size() != 0) {
            (void)ble_proto_stream_feed(stream_, value.data(), value.size());
        }
    }

private:
    ble_proto_stream_t *stream_;
};

class ServerCallbacks final : public NimBLEServerCallbacks {
public:
    void onConnect(NimBLEServer *, NimBLEConnInfo &connInfo) override
    {
        /* A peer may reconnect after dropping a partial protocol frame.  Do
         * not let bytes from the previous link become the prefix of a new
         * command stream. */
        ble_proto_stream_init(&s_control_stream, dispatch_frame,
                              &s_control_endpoint);
        ble_proto_stream_init(&s_image_stream, dispatch_frame,
                              &s_image_endpoint);
        s_connection_handle = connInfo.getConnHandle();
        (void)ble_gatt_set_connected(&s_gatt, true);
        const int power_result = ble_gatt_apply_connection_tx_limit(
            &s_gatt, connInfo.getConnHandle());
        if (power_result != 0) {
            ESP_LOGW(TAG, "connection TX power selector unavailable (handle=%u, err=%d); default remains capped",
                     connInfo.getConnHandle(), power_result);
        }
    }

    void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) override
    {
        ble_proto_stream_init(&s_control_stream, dispatch_frame,
                              &s_control_endpoint);
        ble_proto_stream_init(&s_image_stream, dispatch_frame,
                              &s_image_endpoint);
        s_connection_handle = BLE_HS_CONN_HANDLE_NONE;
        (void)ble_gatt_set_connected(&s_gatt, false);
        (void)NimBLEDevice::startAdvertising();
    }
};

static ProtocolWriteCallbacks s_control_callbacks(&s_control_stream);
static ProtocolWriteCallbacks s_image_callbacks(&s_image_stream);
static ServerCallbacks s_server_callbacks;

} // namespace

extern "C" int ble_service_start(ble_gatt_rx_cb_t callback, void *ctx)
{
    s_ble_failed = false;
    s_notify_characteristic = nullptr;
    s_connection_handle = BLE_HS_CONN_HANDLE_NONE;
    if (!callback) {
        s_ble_failed = true;
        return -1;
    }
    if (!NimBLEDevice::init("EPD-Calendar")) {
        ESP_LOGE(TAG, "NimBLE initialization failed");
        s_ble_failed = true;
        return -2;
    }

    /* Use ordinary open GATT for broad mobile-platform compatibility. The
     * protocol still validates framing, CRC, sequence and command payloads;
     * applications that need confidentiality must provide it above GATT. */
    (void)NimBLEDevice::setMTU(256);

    ble_proto_stream_init(&s_control_stream, dispatch_frame,
                          &s_control_endpoint);
    ble_proto_stream_init(&s_image_stream, dispatch_frame,
                          &s_image_endpoint);
    const int gatt_result = ble_gatt_init(&s_gatt, callback, ctx);
    if (gatt_result != 0) {
        ESP_LOGE(TAG, "BLE controller TX power cap failed: %d", gatt_result);
        s_ble_failed = true;
        (void)NimBLEDevice::deinit(true);
        return -3;
    }

    NimBLEServer *server = NimBLEDevice::createServer();
    if (!server) {
        s_ble_failed = true;
        (void)NimBLEDevice::deinit(true);
        return -4;
    }
    server->setCallbacks(&s_server_callbacks, false);

    NimBLEService *service = server->createService(BLE_GATT_SERVICE_UUID);
    if (!service) {
        s_ble_failed = true;
        (void)NimBLEDevice::deinit(true);
        return -5;
    }

    NimBLECharacteristic *control = service->createCharacteristic(
        BLE_GATT_CONTROL_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE,
        ATTRIBUTE_MAX_LEN);
    NimBLECharacteristic *notify = service->createCharacteristic(
        BLE_GATT_NOTIFY_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY,
        ATTRIBUTE_MAX_LEN);
    NimBLECharacteristic *image = service->createCharacteristic(
        BLE_GATT_IMAGE_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE,
        ATTRIBUTE_MAX_LEN);
    if (!control || !notify || !image) {
        s_ble_failed = true;
        (void)NimBLEDevice::deinit(true);
        return -6;
    }
    s_notify_characteristic = notify;

    control->setCallbacks(&s_control_callbacks);
    image->setCallbacks(&s_image_callbacks);
    control->setValue("");
    notify->setValue("");
    image->setValue("");

    if (!server->start()) {
        s_ble_failed = true;
        (void)NimBLEDevice::deinit(true);
        return -7;
    }
    NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
    if (!advertising) {
        s_ble_failed = true;
        (void)NimBLEDevice::deinit(true);
        return -8;
    }
    advertising->setName("EPD-Calendar");
    if (!advertising->addServiceUUID(BLE_GATT_SERVICE_UUID)) {
        s_ble_failed = true;
        (void)NimBLEDevice::deinit(true);
        return -9;
    }
    advertising->enableScanResponse(true);
    if (!advertising->start()) {
        s_ble_failed = true;
        (void)NimBLEDevice::deinit(true);
        return -10;
    }

    ESP_LOGI(TAG, "NimBLE GATT online; open Control/Image RX, TX cap=%d dBm",
             ble_gatt_applied_power_dbm(&s_gatt));
    return 0;
}

extern "C" int ble_service_notify(const uint8_t *frame, size_t frame_len)
{
    if (s_ble_failed || !frame || frame_len == 0 || frame_len > BLE_PROTO_MAX_FRAME ||
        !s_notify_characteristic || !s_gatt.connected ||
        s_connection_handle == BLE_HS_CONN_HANDLE_NONE) {
        return -1;
    }
    NimBLEServer *server = NimBLEDevice::getServer();
    if (!server) return -1;
    const uint16_t mtu = server->getPeerMTU(s_connection_handle);
    const size_t max_chunk = mtu > 3u ? (size_t)mtu - 3u : 20u;
    for (size_t offset = 0; offset < frame_len;) {
        size_t chunk = frame_len - offset;
        if (chunk > max_chunk) chunk = max_chunk;
        if (!s_notify_characteristic->notify(frame + offset, chunk,
                                              s_connection_handle)) {
            return -2;
        }
        offset += chunk;
        if (offset < frame_len) vTaskDelay(1);
    }
    return 0;
}
