/**
 * @file at_transport_sample.c
 * Connect the AT framework to the application's communication driver.
 */

/*********************
 *      INCLUDES
 *********************/

#include "at_device_sample.h"

#include <stddef.h>

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/**
 * Copy outgoing bytes into the UART transmit queue.
 * @param data_p source bytes, not necessarily NUL-terminated
 * @param len requested byte count
 * @return number of bytes accepted by the driver
 * @note The driver must accept the entire request; core does not retry a
 *       short write. Copy bytes before returning for asynchronous DMA use.
 */
uint32_t at_device_write(const void * data_p, uint32_t len)
{
    if (data_p == NULL || len == 0) {
        return 0;
    }
    return board_uart_tx_enqueue(data_p, len);
}

/**
 * Drain the bytes already available in the UART receive queue.
 * @param data_p destination buffer
 * @param len maximum number of bytes to read
 * @return received byte count, or zero if the queue is empty
 * @note Queue implementation must never copy more than len bytes.
 */
uint32_t at_device_read(void * data_p, uint32_t len)
{
    if (data_p == NULL || len == 0) {
        return 0;
    }
    return board_uart_rx_dequeue(data_p, len);
}
