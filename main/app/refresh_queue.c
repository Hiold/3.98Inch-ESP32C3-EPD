#include "refresh_queue.h"

#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
struct refresh_queue { QueueHandle_t handle; };
#else
struct refresh_queue {
    refresh_queue_item_t items[8];
    uint8_t head, tail, count;
};
#endif

refresh_queue_t *refresh_queue_create(void)
{
    refresh_queue_t *queue = calloc(1, sizeof(*queue));
    if (!queue) return NULL;
#ifdef ESP_PLATFORM
    queue->handle = xQueueCreate(8, sizeof(refresh_queue_item_t));
    if (!queue->handle) { free(queue); return NULL; }
#endif
    return queue;
}

void refresh_queue_destroy(refresh_queue_t *queue)
{
    if (!queue) return;
#ifdef ESP_PLATFORM
    vQueueDelete(queue->handle);
#endif
    free(queue);
}

bool refresh_queue_submit(refresh_queue_t *queue,
                          const refresh_queue_item_t *item)
{
    if (!queue || !item || item->reason_mask == 0) return false;
#ifdef ESP_PLATFORM
    return xQueueSend(queue->handle, item, 0) == pdTRUE;
#else
    if (queue->count >= 8) return false;
    queue->items[queue->tail] = *item;
    queue->tail = (uint8_t)((queue->tail + 1u) % 8u);
    queue->count++;
    return true;
#endif
}

bool refresh_queue_receive(refresh_queue_t *queue, refresh_queue_item_t *item,
                           uint32_t timeout_ms)
{
    if (!queue || !item) return false;
#ifdef ESP_PLATFORM
    return xQueueReceive(queue->handle, item, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
#else
    (void)timeout_ms;
    if (queue->count == 0) return false;
    *item = queue->items[queue->head];
    queue->head = (uint8_t)((queue->head + 1u) % 8u);
    queue->count--;
    return true;
#endif
}
