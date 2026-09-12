/**
 * @file at_port_sample.c
 * Provide memory allocation and the AT millisecond clock.
 * Copy the needed functions into the application platform port.
 */

/*********************
 *      INCLUDES
 *********************/

#include "at_chat.h"

#include <stdint.h>
#include <stdlib.h>

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/**
 * Allocate memory for the AT framework.
 * @return allocated block, or NULL on failure
 */
void * at_malloc(unsigned int size)
{
    return malloc(size);
}

/**
 * Release a block previously allocated by at_malloc.
 */
void at_free(void * memory_p)
{
    free(memory_p);
}

/**
 * Return the millisecond count used for command timeouts.
 * Advance it with at_device_tick_inc from the application system tick.
 */
unsigned int at_get_ms(void)
{
    return (unsigned int)at_device_get_tick();
}
