/**
 * @file at_device_sample.c
 * Initialize the AT device and process requests from the main loop.
 */

/*********************
 *      INCLUDES
 *********************/

#include "at_chat.h"

#include <stdint.h>
#include <stddef.h>

/*********************
 *      DEFINES
 *********************/

#define AT_POLL_PERIOD_MS 5U

/**********************
 *  STATIC VARIABLES
 **********************/

/**
 * Bind the communication driver to the AT framework.
 * The adapter must remain valid for the lifetime of the object.
 */
static const at_adapter_t at_adapter = {
    .lock          = NULL,            /**< Single execution context */
    .unlock        = NULL,            /**< Single execution context */
    .write         = at_device_write, /**< Non-blocking transmit queue */
    .read          = at_device_read,  /**< Non-blocking receive queue */
    .debug         = NULL,            /**< Optional formatted logging */
    .recv_bufsize  = 256              /**< Maximum response plus terminator */
};

static at_obj_t * device_p = NULL;
static volatile uint32_t tick_ms = 0;
static uint32_t last_poll_ms = 0;

/**********************
 *  APPLICATION USAGE
 **********************/

/**
 * Connect these calls to the application's existing entry points:
 *
 * 1. Initialize the UART queues and start the system timer.
 * 2. Call at_device_init() once and check its return value.
 * 3. In the existing 1 ms timer interrupt or tick callback:
 *
 *        at_device_tick_inc(1U);
 *
 *    For a 10 ms timer, pass 10U instead. Use exactly one tick source.
 *    Do not advance time according to the number of main-loop iterations.
 *
 * 4. After successful initialization, submit at_device_check_ready() once
 *    and check whether it was queued. Then keep running the main loop:
 *
 *        for (;;) {
 *            at_device_process();
 *            // Process other non-blocking application work here.
 *        }
 *
 *    Wait for the "ready" success callback before submitting device commands.
 *    Do not resubmit the probe on every iteration.
 *
 * The tick only advances time. at_device_process() performs communication
 * and invokes callbacks, with a minimum interval of AT_POLL_PERIOD_MS.
 * Keep processing in one main-loop/task context, never in the tick interrupt.
 * In an RTOS task, yield between iterations while the system tick continues.
 * The target must provide an atomic counter read or protect it appropriately;
 * volatile alone does not make a 32-bit access atomic on every processor.
 *
 * The Linux integration snippets use clock_gettime(CLOCK_MONOTONIC) instead;
 * it does not call at_device_tick_inc().
 */

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/**
 * Create the AT object after the UART and system tick are initialized.
 * @return true if the object is available, otherwise false
 */
bool at_device_init(void)
{
    if (device_p != NULL) {
        return true;
    }
    device_p = at_obj_create(&at_adapter);
    last_poll_ms = at_device_get_tick();

    return device_p != NULL;
}

/**
 * Release the object after all other access has stopped.
 * Pending requests do not receive a completion callback on destruction.
 */
void at_device_deinit(void)
{
    if (device_p == NULL) {
        return;
    }
    at_obj_destroy(device_p);
    device_p = NULL;
}

/**
 * Advance the clock from the application system tick.
 * @param elapsed_ms actual elapsed time in milliseconds
 */
void at_device_tick_inc(uint32_t elapsed_ms)
{
    tick_ms += elapsed_ms;
}

/**
 * Read the millisecond clock.
 * The target platform must ensure a reliable counter read.
 */
uint32_t at_device_get_tick(void)
{
    return tick_ms;
}

/**
 * Call continuously from the main loop, not from an interrupt.
 * Each invocation performs at most one AT polling cycle.
 */
void at_device_process(void)
{
    uint32_t tick = at_device_get_tick();

    if (device_p == NULL) {
        return;
    }
    if ((uint32_t)(tick - last_poll_ms) < AT_POLL_PERIOD_MS) {
        return;
    }
    last_poll_ms = tick;
    at_obj_process(device_p);
}

/**
 * Obtain the object for submitting an application request.
 * @param device_out_p output pointer, set to NULL when uninitialized
 * @return true if the object is available
 */
bool at_device_get_object(at_obj_t ** device_out_p)
{
    if (device_out_p == NULL) {
        return false;
    }
    *device_out_p = device_p;
    return device_p != NULL;
}

/**
 * Report pending AT work, not the device's physical connection state.
 */
bool at_device_is_busy(void)
{
    if (device_p == NULL) {
        return false;
    }
    return at_obj_busy(device_p);
}
