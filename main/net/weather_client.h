#ifndef WEATHER_CLIENT_H
#define WEATHER_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#define WEATHER_UPDATE_INTERVAL_SEC (6u * 60u * 60u)

typedef struct {
    int32_t latitude_e7;
    int32_t longitude_e7;
    int16_t temperature_c10;
    char summary[48];
    uint64_t updated_unix;
    bool valid;
    bool stale;
} weather_data_t;

typedef struct {
    int32_t latitude_e7;
    int32_t longitude_e7;
    weather_data_t last;
    bool request_in_flight;
} weather_client_t;

void weather_client_init(weather_client_t *client, int32_t latitude_e7,
                         int32_t longitude_e7);
void weather_client_set_location(weather_client_t *client, int32_t latitude_e7,
                                 int32_t longitude_e7);
bool weather_client_due(const weather_client_t *client, uint64_t now_unix);
const weather_data_t *weather_client_data(const weather_client_t *client);
void weather_client_mark_failure(weather_client_t *client, uint64_t now_unix);
void weather_client_mark_success(weather_client_t *client,
                                 const weather_data_t *data);

#endif
