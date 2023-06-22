#include "joybus.h"

uint32_t joybus_rx_get_latest(PIO pio_instance, uint sm_instance)
{
    static uint32_t last_value = 0;

    while (!pio_sm_is_rx_fifo_empty(pio_instance, sm_instance)) {
        last_value = pio_sm_get_blocking(pio_instance, sm_instance);
    }

    return last_value;
}

void joybus_tx(PIO pio_instance, uint sm_instance, uint tx_offset, uint8_t data)
{
    pio_instance->txf[sm_instance] = (8 << 24) | (data << 16);   // Number of data bits (always 8) + actual data (1 byte)
    uint jmp_to_tx_start = pio_encode_jmp(tx_offset);
    pio_sm_exec_wait_blocking(pio_instance, sm_instance, jmp_to_tx_start);
}