#ifndef GPIO_POLICY_H
#define GPIO_POLICY_H

#include <stdbool.h>
#include <stddef.h>

bool gpio_policy_pin_is_allowed(int pin);
bool gpio_policy_pin_forbidden_hint(int pin, char *result, size_t result_len);
bool gpio_policy_runtime_input_pin_is_safe(int pin);

#ifdef TEST_BUILD
bool gpio_policy_test_pin_is_allowed(int pin,
                                     const char *csv,
                                     int min_pin,
                                     int max_pin,
                                     bool block_esp32_flash_pins,
                                     bool require_valid_gpio);
bool gpio_policy_test_runtime_input_pin_is_safe(int pin,
                                                bool block_esp32_flash_pins,
                                                bool require_valid_gpio);
#endif

#endif  // GPIO_POLICY_H
