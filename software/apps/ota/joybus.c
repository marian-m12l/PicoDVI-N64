#include "joybus.h"

uint32_t joybus_rx_get_latest(PIO pio_instance, uint sm_instance)
{
    static uint32_t last_value = 0;

    while (!pio_sm_is_rx_fifo_empty(pio_instance, sm_instance)) {
        last_value = pio_sm_get_blocking(pio_instance, sm_instance);
    }

    return last_value;
}