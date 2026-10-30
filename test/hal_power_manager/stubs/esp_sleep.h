#pragma once
using gpio_num_t = int;
constexpr int GPIO_MODE_OUTPUT = 1;
inline void gpio_set_direction(int, int) {}
inline void gpio_set_level(int, int) {}
inline void gpio_hold_en(int) {}
inline void gpio_deep_sleep_hold_en() {}
inline void esp_sleep_config_gpio_isolate() {}
inline void esp_deep_sleep_start() {}
