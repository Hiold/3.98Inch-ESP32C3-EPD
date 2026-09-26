#include "weather_client.h"

#include <string.h>

void weather_client_init(weather_client_t *client, int32_t latitude_e7,
                         int32_t longitude_e7)
{
    if (!client) return;
    memset(client, 0, sizeof(*client));
    client->latitude_e7 = latitude_e7;
    client->longitude_e7 = longitude_e7;
}

void weather_client_set_location(weather_client_t *client, int32_t latitude_e7,
                                 int32_t longitude_e7)
{
    if (!client) return;
    client->latitude_e7 = latitude_e7;
    client->longitude_e7 = longitude_e7;
}

bool weather_client_due(const weather_client_t *client, uint64_t now_unix)
{
    if (!client || client->request_in_flight) return false;
    return !client->last.valid || now_unix - client->last.updated_unix >= WEATHER_UPDATE_INTERVAL_SEC;
}

const weather_data_t *weather_client_data(const weather_client_t *client)
{
    return client ? &client->last : NULL;
}

void weather_client_mark_failure(weather_client_t *client, uint64_t now_unix)
{
    if (!client) return;
    client->request_in_flight = false;
    if (client->last.valid) client->last.stale = true;
    else client->last.updated_unix = now_unix;
}

void weather_client_mark_success(weather_client_t *client,
                                 const weather_data_t *data)
{
    if (!client || !data) return;
    client->last = *data;
    client->last.stale = false;
    client->last.valid = true;
    client->request_in_flight = false;
}
