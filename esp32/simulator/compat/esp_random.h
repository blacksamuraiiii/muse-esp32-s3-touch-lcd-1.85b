/* Host stand-in for ESP-IDF's esp_random.h (the interact page and the pet
 * worker draw random numbers; the simulator only needs them to exist). */
#pragma once

#include <stdint.h>
#include <stdlib.h>

static inline uint32_t esp_random(void)
{
    return (uint32_t)rand();
}
