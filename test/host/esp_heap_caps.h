#ifndef ESP_HEAP_CAPS_H
#define ESP_HEAP_CAPS_H

#include <stddef.h>

#define MALLOC_CAP_8BIT 1U

size_t heap_caps_get_largest_free_block(unsigned int caps);

#endif // ESP_HEAP_CAPS_H
