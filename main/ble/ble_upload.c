#include "ble_upload.h"

#include <string.h>

int ble_upload_begin(ble_upload_t *upload, const photo_store_begin_t *begin)
{
    if (!upload || !begin) return PHOTO_STORE_ERR_ARG;
    /* Release a previous dynamic resume bitmap before reusing the session.
     * photo_store_upload_reset() has a magic guard, so this is also safe when
     * the caller supplies a fresh, uninitialised stack object. */
    photo_store_upload_reset(&upload->state);
    memset(upload, 0, sizeof(*upload));
    const int status = photo_store_upload_begin(&upload->state, begin);
    upload->active = status == PHOTO_STORE_OK;
#ifdef ESP_PLATFORM
    if (upload->active && photo_store_begin(begin) != ESP_OK) {
        photo_store_upload_reset(&upload->state);
        upload->active = false;
        photo_store_abort(begin->slot);
        return PHOTO_STORE_ERR_IO;
    }
#endif
    return status;
}

int ble_upload_data(ble_upload_t *upload, uint32_t offset,
                    const void *data, size_t len)
{
    if (!upload || !upload->active || !data) return PHOTO_STORE_ERR_STATE;
#ifdef ESP_PLATFORM
    if (photo_store_write(upload->state.slot, offset, data, len) != ESP_OK) {
        return PHOTO_STORE_ERR_IO;
    }
    /* Keep the protocol-side bitmap in sync for resume queries. The storage
     * layer maintains its own bitmap for the final completeness check. */
    return photo_store_upload_mark_data(&upload->state, offset, len);
#else
    return photo_store_upload_mark_data(&upload->state, offset, len);
#endif
}

int ble_upload_end(ble_upload_t *upload, uint32_t crc32,
                   photo_store_info_t *info)
{
    if (!upload || !upload->active) return PHOTO_STORE_ERR_STATE;
#ifdef ESP_PLATFORM
    const photo_store_begin_t begin = {
        .photo_id = upload->state.photo_id,
        .slot = upload->state.slot,
        .total_len = upload->state.total_len,
        .crc32 = upload->state.expected_crc32,
    };
    /* photo_store_end() performs the flash CRC check and atomic metadata commit.
     * Do not call the host bookkeeping finish path again: it would attempt to
     * commit the same upload a second time. */
    const esp_err_t end_err = photo_store_end(&begin, crc32);
    int status = end_err == ESP_OK ? PHOTO_STORE_OK :
                 end_err == ESP_ERR_INVALID_CRC ? PHOTO_STORE_ERR_CRC :
                 end_err == ESP_ERR_INVALID_STATE ? PHOTO_STORE_ERR_STATE :
                                                    PHOTO_STORE_ERR_IO;
    if (status == PHOTO_STORE_OK && info) {
        info->valid = true;
        info->photo_id = begin.photo_id;
        info->slot = begin.slot;
        info->length = begin.total_len;
        info->crc32 = crc32;
    }
#else
    int status = photo_store_upload_finish(&upload->state, crc32, info);
#endif
    if (status == PHOTO_STORE_OK) upload->active = false;
    return status;
}

void ble_upload_abort(ble_upload_t *upload)
{
    if (upload) {
#ifdef ESP_PLATFORM
        (void)photo_store_abort(upload->state.slot);
#endif
        photo_store_upload_reset(&upload->state);
        upload->active = false;
    }
}

uint32_t ble_upload_next_offset(const ble_upload_t *upload)
{
    return upload ? photo_store_upload_next_offset(&upload->state) : 0;
}
