#pragma once
#include <thread>
#include <chrono>
#include "freertos/FreeRTOS.h"
inline void vTaskDelay(TickType_t ticks) { if (ticks) std::this_thread::sleep_for(std::chrono::milliseconds(ticks)); }
