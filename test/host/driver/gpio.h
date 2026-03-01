#ifndef DRIVER_GPIO_H
#define DRIVER_GPIO_H

typedef int gpio_num_t;

#define GPIO_MODE_OUTPUT 1
#define GPIO_MODE_INPUT 2
#define GPIO_MODE_INPUT_OUTPUT (GPIO_MODE_OUTPUT | GPIO_MODE_INPUT)

enum {
    GPIO_TEST_PIN_LIMIT = 128,
};

static int s_gpio_test_mode[GPIO_TEST_PIN_LIMIT];
static int s_gpio_test_level[GPIO_TEST_PIN_LIMIT];

static inline int gpio_reset_pin(gpio_num_t pin)
{
    if (pin >= 0 && pin < GPIO_TEST_PIN_LIMIT) {
        s_gpio_test_mode[pin] = 0;
        s_gpio_test_level[pin] = 0;
    }
    return 0;
}

static inline int gpio_set_direction(gpio_num_t pin, int mode)
{
    if (pin >= 0 && pin < GPIO_TEST_PIN_LIMIT) {
        s_gpio_test_mode[pin] = mode;
    }
    return 0;
}

static inline int gpio_input_enable(gpio_num_t pin)
{
    if (pin >= 0 && pin < GPIO_TEST_PIN_LIMIT) {
        s_gpio_test_mode[pin] |= GPIO_MODE_INPUT;
    }
    return 0;
}

static inline int gpio_set_level(gpio_num_t pin, int level)
{
    if (pin >= 0 && pin < GPIO_TEST_PIN_LIMIT) {
        s_gpio_test_level[pin] = level ? 1 : 0;
    }
    return 0;
}

static inline int gpio_get_level(gpio_num_t pin)
{
    if (pin < 0 || pin >= GPIO_TEST_PIN_LIMIT) {
        return 0;
    }
    if ((s_gpio_test_mode[pin] & GPIO_MODE_INPUT) == 0) {
        return 0;
    }
    return s_gpio_test_level[pin];
}

static inline void gpio_test_reset_state(void)
{
    for (int i = 0; i < GPIO_TEST_PIN_LIMIT; i++) {
        s_gpio_test_mode[i] = 0;
        s_gpio_test_level[i] = 0;
    }
}

#endif // DRIVER_GPIO_H
