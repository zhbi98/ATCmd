/**
 * @file at_urc_sample.c
 * Handle unsolicited module notifications in the AT polling context.
 * Adapt the message formats and business actions to the module protocol.
 */

/*********************
 *      INCLUDES
 *********************/

#include "at_chat.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/**********************
 *  STATIC PROTOTYPES
 **********************/

static int at_ready_report(at_urc_info_t * info_p);
static int at_power_report(at_urc_info_t * info_p);
static void at_startup_response(at_response_t * resp_p);

/**********************
 *  STATIC VARIABLES
 **********************/

static bool module_ready            = false;
static bool startup_query_submitted = false;

static const urc_item_t urc_table[] = {
    {
        .prefix  = "+IM_READY",
        .endmark = '\n',
        .handler = at_ready_report
    },
    {
        .prefix  = "+POWER:",
        .endmark = '\n',
        .handler = at_power_report
    }
};

/**********************
 *  APPLICATION USAGE
 **********************/

/**
 * 1. Enable AT_URC_WARCH_EN and set .urc_bufsize = 128 in the adapter
 *    before creating the object. Adapt the capacity to the longest message.
 * 2. Start UART reception, create the object, then call at_urc_register().
 * 3. Call at_urc_reset_ready() before powering on or resetting the module.
 * 4. Call at_urc_process(device_p) repeatedly from the owning loop/task.
 *    It polls the object even while module_ready is false, then submits ATI
 *    once after +IM_READY. Adapt ATI to a query supported by the module.
 * 5. Handle the query result in at_startup_response(). Queue acceptance and
 *    the startup notification do not prove that this query succeeded.
 *
 * If the application already polls this object, copy only the readiness
 * check/submission below after its existing poll; do not add another poller.
 * This path waits for +IM_READY instead of submitting the usual AT probe
 * immediately after object creation. Keep the appropriate path for the module.
 *
 * These snippets also work with Linux or FreeRTOS objects; no extra task is
 * needed. Notifications already lost before reception starts cannot be caught.
 * +IM_READY must end in LF here (CRLF is accepted). Its readiness guarantees
 * come from the module manual, not from the framework.
 */

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/**
 * Register once on a valid object before the module sends notifications.
 */
void at_urc_register(at_obj_t * device_p)
{
    if (device_p == NULL) {
        return;
    }
    at_obj_set_urc(device_p, urc_table,
                  (int)(sizeof(urc_table) / sizeof(urc_table[0])));
}

/**
 * Clear stale startup state before each module power-on or reset.
 */
void at_urc_reset_ready(void)
{
    module_ready            = false;
    startup_query_submitted = false;
}

/**
 * Read in the polling context; use an event/queue for other tasks.
 */
bool at_urc_is_ready(void)
{
    return module_ready;
}

/**
 * Poll continuously and use module_ready to start one device query.
 * Call from one loop/task after registering URCs and powering on the module.
 * A failed enqueue may be retried on the next iteration; an accepted query
 * is not repeated automatically, even if its response reports an error.
 */
void at_urc_process(at_obj_t * device_p)
{
    at_attr_t attr;

    if (device_p == NULL) {
        return;
    }

    /* Poll before checking readiness so the startup URC can be received. */
    at_obj_process(device_p);

    if (!at_urc_is_ready() || startup_query_submitted) {
        return;
    }

    at_attr_deinit(&attr);
    attr.cb      = at_startup_response;
    attr.suffix  = "OK";
    attr.timeout = 1200;
    attr.retry   = 0;

    startup_query_submitted = at_send_singlline(device_p, &attr, "ATI");
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

/**
 * Match a complete startup line, excluding surrounding CR/LF characters.
 * The core selects handlers by substring; validate the full message here.
 * Returning zero finishes this notification without requesting more bytes.
 */
static int at_ready_report(at_urc_info_t * info_p)
{
    const char * text_p;
    uint32_t     length;

    if (info_p == NULL || info_p->status != URC_RECV_OK ||
        info_p->urcbuf == NULL || info_p->urclen <= 0) {
        return 0;
    }
    text_p = info_p->urcbuf;
    length = (uint32_t)info_p->urclen;

    while (length != 0 && (*text_p == '\r' || *text_p == '\n')) {
        text_p++;
        length--;
    }

    while (length != 0 && (text_p[length - 1] == '\r' ||
                          text_p[length - 1] == '\n')) {
        length--;
    }

    if (length == sizeof("+IM_READY") - 1 &&
        memcmp(text_p, "+IM_READY", length) == 0) {
        module_ready = true;
        /* Notify application startup logic here; do not block. */
    }

    return 0;
}

/**
 * Illustrative numeric report: +POWER:80 followed by CRLF.
 * This example accepts a decimal value from 0 to 100; adapt its meaning/range.
 * Read only urclen bytes, without depending on a terminating NUL.
 */
static int at_power_report(at_urc_info_t * info_p)
{
    const char * text_p;
    uint32_t     length;
    uint32_t     value = 0;

    if (info_p == NULL || info_p->status != URC_RECV_OK ||
        info_p->urcbuf == NULL || info_p->urclen <= 0) {
        return 0;
    }
    text_p = info_p->urcbuf;
    length = (uint32_t)info_p->urclen;

    while (length != 0 && (*text_p == '\r' || *text_p == '\n')) {
        text_p++;
        length--;
    }

    if (length < 8 || memcmp(text_p, "+POWER:", 7) != 0) {
        return 0;
    }
    text_p += 7;
    length -= 7;

    if (*text_p < '0' || *text_p > '9') {
        return 0;
    }
    while (length != 0 && *text_p >= '0' && *text_p <= '9') {
        value = value * 10U + (uint32_t)(*text_p - '0');
        if (value > 100U) {
            return 0;
        }
        text_p++;
        length--;
    }

    while (length != 0 && (*text_p == '\r' || *text_p == '\n')) {
        text_p++;
        length--;
    }

    if (length == 0) {
        printf("power=%u\n", (unsigned int)value);
        /* Copy the parsed value into application state or send an event. */
    }

    return 0;
}

/**
 * Report the query result separately from the module startup notification.
 */
static void at_startup_response(at_response_t * resp_p)
{
    if (resp_p->code != AT_RESP_OK) {
        printf("startup query failed: %d\n", (int)resp_p->code);
        return;
    }
    printf("device information: %.*s\n",
           (int)resp_p->recvcnt, resp_p->recvbuf);
    /* Continue device-specific initialization here. */
}
