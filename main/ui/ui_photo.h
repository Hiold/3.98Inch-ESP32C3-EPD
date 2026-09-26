#ifndef UI_PHOTO_H
#define UI_PHOTO_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t slot;
    uint32_t photo_id;
    bool valid;
} ui_photo_state_t;

void ui_photo_set(const ui_photo_state_t *state);
uint8_t ui_photo_current_slot(void);
int ui_photo_next_slot(void);

#endif
