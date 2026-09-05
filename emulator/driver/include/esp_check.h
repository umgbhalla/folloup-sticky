#pragma once
#include "esp_err.h"
#define ESP_RETURN_ON_ERROR(expr, tag, fmt, ...) do { esp_err_t _e = (expr); if (_e != ESP_OK) return _e; } while (0)
