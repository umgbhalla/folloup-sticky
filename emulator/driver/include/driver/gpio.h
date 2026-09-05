#pragma once
#include <cstdint>
typedef int gpio_num_t;
enum { GPIO_INTR_DISABLE, GPIO_MODE_INPUT, GPIO_MODE_OUTPUT, GPIO_PULLDOWN_DISABLE, GPIO_PULLUP_DISABLE, GPIO_PULLUP_ENABLE };
struct gpio_config_t { int intr_type; int mode; uint64_t pin_bit_mask; int pull_down_en; int pull_up_en; };
int gpio_config(const gpio_config_t*);
int gpio_set_level(gpio_num_t, int);
int gpio_get_level(gpio_num_t);
#ifndef GPIO_NUM_NC
#define GPIO_NUM_NC ((gpio_num_t)-1)
#endif
