#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define TIME_MANAGER_DEVICE_TIME_LENGTH 20U

typedef struct {
    int64_t ts;
    char time_device[TIME_MANAGER_DEVICE_TIME_LENGTH];
} time_manager_snapshot_t;

esp_err_t time_manager_start(void);
bool time_manager_is_valid(void);
int64_t time_manager_get_timestamp(void);
esp_err_t time_manager_get_device_time(char *buf, size_t len);
esp_err_t time_manager_get_snapshot(time_manager_snapshot_t *snapshot);
