#ifndef REFRESH_QUEUE_H
#define REFRESH_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t reason_mask;
    bool force;
    uint64_t requested_ms;
} refresh_queue_item_t;

typedef struct refresh_queue refresh_queue_t;

refresh_queue_t *refresh_queue_create(void);
void refresh_queue_destroy(refresh_queue_t *queue);
bool refresh_queue_submit(refresh_queue_t *queue,
                          const refresh_queue_item_t *item);
bool refresh_queue_receive(refresh_queue_t *queue, refresh_queue_item_t *item,
                           uint32_t timeout_ms);

#endif
