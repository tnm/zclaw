#ifndef DRIVER_GPIO_H
#define DRIVER_GPIO_H

typedef int gpio_num_t;

#define GPIO_MODE_OUTPUT 1
#define GPIO_MODE_INPUT 2

static inline int gpio_reset_pin(gpio_num_t pin)
{
    (void)pin;
    return 0;
}

static inline int gpio_set_direction(gpio_num_t pin, int mode)
{
    (void)pin;
    (void)mode;
    return 0;
}

static inline int gpio_set_level(gpio_num_t pin, int level)
{
    (void)pin;
    (void)level;
    return 0;
}

static inline int gpio_get_level(gpio_num_t pin)
{
    (void)pin;
    return 0;
}

#endif // DRIVER_GPIO_H
