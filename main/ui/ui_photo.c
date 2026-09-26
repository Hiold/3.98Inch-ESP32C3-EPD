#include "ui_photo.h"

static ui_photo_state_t current;

void ui_photo_set(const ui_photo_state_t *state)
{
    if (state) current = *state;
}

uint8_t ui_photo_current_slot(void)
{
    return current.slot;
}

int ui_photo_next_slot(void)
{
    current.slot = (uint8_t)((current.slot + 1u) % 4u);
    return current.slot;
}
