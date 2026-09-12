/**
 * @file at_freertos_sample.c
 * Connect the FreeRTOS tick and polling task to the AT device examples.
 * Compile with the four embedded examples and the application UART driver.
 */

/*********************
 *      INCLUDES
 *********************/

#include "at_device_sample.h"

#include "FreeRTOS.h"
#include "task.h"

/*********************
 *      DEFINES
 *********************/

#define AT_TASK_POLL_MS 5U

#if configUSE_TICK_HOOK != 1
#error "Enable configUSE_TICK_HOOK in FreeRTOSConfig.h"
#endif

#if configUSE_TICKLESS_IDLE != 0
#error "This tick-hook example requires configUSE_TICKLESS_IDLE == 0"
#endif

/**********************
 * GLOBAL PROTOTYPES
 **********************/

void vApplicationTickHook(void);
void at_freertos_task(void * argument_p);

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/**
 * Advance AT time from the FreeRTOS tick interrupt.
 *
 * @note Merge this body into an existing hook; do not define a second hook.
 * @note Use only this clock source: remove any other at_device_tick_inc call.
 *       Keep framework polling and command submission outside interrupts.
 *       This example assumes a single-core kernel and atomic uint32_t reads.
 */
void vApplicationTickHook(void)
{
    static uint64_t tick_fraction = 0;
    uint32_t        elapsed_ms;

    /* Preserve fractions for rates such as 128 Hz or rates above 1000 Hz. */
    tick_fraction += 1000U;

    elapsed_ms     = (uint32_t)(tick_fraction / configTICK_RATE_HZ);
    tick_fraction %= configTICK_RATE_HZ;

    if (elapsed_ms != 0) {
        at_device_tick_inc(elapsed_ms);
    }
}

/**
 * Own the AT object, request submission and response callbacks in one task.
 * Create this task once with xTaskCreate after UART queues are ready.
 *
 * @param argument_p unused task argument
 * @note Enable INCLUDE_vTaskDelay and INCLUDE_vTaskDelete in FreeRTOSConfig.h.
 *       Size the stack for the command callbacks, including printf usage.
 */
void at_freertos_task(void * argument_p)
{
    TickType_t poll_ticks = pdMS_TO_TICKS(AT_TASK_POLL_MS);

    (void)argument_p;

    /* At low tick rates, 5 ms may round down to zero ticks. */
    if (poll_ticks == 0) {
        poll_ticks = 1;
    }

    /* Create the object after the application UART queues are ready. */
    if (!at_device_init()) {
        vTaskDelete(NULL);
        return;
    }

    /* Submit the communication probe once before entering the loop. */
    if (!at_device_check_ready()) {
        at_device_deinit();
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        at_device_process();

        /*
         * Handle application requests here without blocking the AT loop.
         * Wait for the successful "ready" callback before device operations.
         * Other tasks should send requests to this task through a queue.
         */
        vTaskDelay(poll_ticks);
    }
}
