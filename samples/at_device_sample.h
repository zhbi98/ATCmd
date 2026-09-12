/**
 * @file at_device_sample.h
 * Device, driver and command interfaces used by the C examples.
 */

#ifndef AT_DEVICE_SAMPLE_H
#define AT_DEVICE_SAMPLE_H

/*********************
 *      INCLUDES
 *********************/

#include "at_chat.h"
#include <stdint.h>

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * Implement these queue operations in the application's UART driver.
 * TX copies and accepts all bytes; RX returns available bytes without waiting.
 */
unsigned int board_uart_tx_enqueue(const void * data_p, unsigned int len);
unsigned int board_uart_rx_dequeue(void * data_p, unsigned int len);

/**
 * AT Write/Read callbacks, implemented in at_transport_sample.c.
 */
unsigned int at_device_write(const void * data_p, unsigned int len);
unsigned int at_device_read(void * data_p, unsigned int len);

/**
 * Initialize or release the object from the owning execution context.
 */
bool at_device_init(void);
void at_device_deinit(void);

/**
 * Update the millisecond tick and process the object from the main loop.
 */
void at_device_tick_inc(uint32_t elapsed_ms);
uint32_t at_device_get_tick(void);
void at_device_process(void);

/**
 * Access the initialized object and its request-processing state.
 */
bool at_device_get_object(at_obj_t ** device_out_p);
bool at_device_is_busy(void);

/**
 * Probe basic AT communication before device-specific operations.
 */
bool at_device_check_ready(void);

/**
 * Submit independent queries; check each result for successful enqueueing.
 */
bool at_device_query_address(void);
bool at_device_query_baudrate(void);

/**
 * Send AT+BAUD=<baudrate>; confirm syntax and UART switching with your module.
 */
bool at_device_set_baudrate(uint32_t baudrate);

/**
 * Submit an illustrative control command; adapt AT+OUTIO to your device.
 */
bool at_device_set_output(uint32_t value);

/**
 * Defined in at_commands_sample.c; edit it to process application results.
 */
void at_device_on_response(const char * query, at_response_t * resp_p);

#endif /* AT_DEVICE_SAMPLE_H */
