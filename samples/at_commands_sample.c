/**
 * @file at_commands_sample.c
 * Construct device commands and handle their asynchronous responses.
 * Verify command strings and field formats against the target device manual.
 */

/*********************
 *      INCLUDES
 *********************/

#include "at_chat.h"

#include <stdint.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*********************
 *      DEFINES
 *********************/

#define AT_REQUEST_TIMEOUT_MS 1200U
#define AT_QUERY_RETRY        1U

/**********************
 *  STATIC PROTOTYPES
 **********************/

static bool at_parse_baudrate(const char * text_p, uint32_t * baud_p);
static void at_ready_response(at_response_t * resp_p);
static void at_address_response(at_response_t * resp_p);
static void at_baudrate_response(at_response_t * resp_p);
static void at_set_baudrate_response(at_response_t * resp_p);
static void at_output_response(at_response_t * resp_p);

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/**
 * Check that the device accepts AT commands before protocol-specific queries.
 * @return true if queued; an OK response is reported as "ready"
 */
bool at_device_check_ready(void)
{
    at_obj_t * device_p = NULL;
    at_attr_t attr;

    if (!at_device_get_object(&device_p)) {
        return false;
    }
    at_attr_deinit(&attr);
    attr.suffix   = "OK";
    attr.cb       = at_ready_response;
    attr.timeout  = AT_REQUEST_TIMEOUT_MS;
    attr.retry    = 0;

    return at_send_singlline(device_p, &attr, "AT");
}

/**
 * Query the Bluetooth address using a constant command string.
 * @return true if queued; the callback reports the device result
 */
bool at_device_query_address(void)
{
    at_obj_t * device_p = NULL;
    at_attr_t attr;

    if (!at_device_get_object(&device_p)) {
        return false;
    }
    at_attr_deinit(&attr);
    attr.prefix   = "+LBDADDR";
    attr.suffix   = "OK";
    attr.cb       = at_address_response;
    attr.timeout  = AT_REQUEST_TIMEOUT_MS;
    attr.retry    = AT_QUERY_RETRY;
    attr.priority = AT_PRIORITY_HIGH;

    /* Single-line requests retain the string pointer until completion. */
    return at_send_singlline(device_p, &attr, "AT+LBDADDR?");
}

/**
 * Query the configured baud rate.
 * @return true if queued; the callback parses the device response
 */
bool at_device_query_baudrate(void)
{
    at_obj_t * device_p = NULL;
    at_attr_t attr;

    if (!at_device_get_object(&device_p)) {
        return false;
    }
    at_attr_deinit(&attr);
    attr.prefix   = "+BAUD";
    attr.suffix   = "OK";
    attr.cb       = at_baudrate_response;
    attr.timeout  = AT_REQUEST_TIMEOUT_MS;
    attr.retry    = AT_QUERY_RETRY;
    attr.priority = AT_PRIORITY_HIGH;

    return at_send_singlline(device_p, &attr, "AT+BAUD?");
}

/**
 * Send a command with a numeric parameter using printf-style formatting.
 * Example: at_device_set_baudrate(115200) sends AT+BAUD=115200 followed by CRLF.
 * @param baudrate positive baud value supported by the target module
 * @return true if queued; completion is reported by at_set_baudrate_response
 * @note This syntax is illustrative: confirm it in the module manual.
 *       Change the host UART at the time specified by the device protocol.
 */
bool at_device_set_baudrate(uint32_t baudrate)
{
    at_obj_t * device_p = NULL;
    at_attr_t attr;

    if (baudrate == 0) {
        return false;
    }
    if (!at_device_get_object(&device_p)) {
        return false;
    }
    at_attr_deinit(&attr);
    attr.suffix   = "OK";
    attr.cb       = at_set_baudrate_response;
    attr.timeout  = AT_REQUEST_TIMEOUT_MS;
    attr.retry    = 0; /* The device may change baud immediately. */

    /* at_exec_cmd copies the formatted text; no shared command buffer needed.
     * Do not append CRLF: the framework appends it when sending.
     */
    return at_exec_cmd(device_p, &attr, "AT+BAUD=%" PRIu32, baudrate);
}

/**
 * Demonstrate a parameterized control command.
 * Replace AT+OUTIO with a command supported by the target device.
 * @param value output value defined by the device protocol
 * @return true if queued, not whether the control has succeeded
 */
bool at_device_set_output(uint32_t value)
{
    at_obj_t * device_p = NULL;
    at_attr_t attr;

    if (!at_device_get_object(&device_p)) {
        return false;
    }
    at_attr_deinit(&attr);
    attr.suffix   = "OK";
    attr.cb       = at_output_response;
    attr.timeout  = AT_REQUEST_TIMEOUT_MS;
    attr.retry    = 0; /* Avoid repeating a control operation automatically. */

    /* The framework copies the formatted result before this call returns. */
    return at_exec_cmd(device_p, &attr, "AT+OUTIO=%" PRIu32, value);
}

/**
 * Process a completed request in the application's polling context.
 * Copy parsed values here before handing them to another task or UI.
 * @param query request identifier: ready, bdaddr, baudrate, set_baudrate or output
 * @param resp_p response valid only for the duration of this callback
 */
void at_device_on_response(const char * query, at_response_t * resp_p)
{
    uint32_t baud = 0;

    if (query == NULL || resp_p == NULL) {
        return;
    }
    switch (resp_p->code) {
    case AT_RESP_OK:
        break;
    case AT_RESP_TIMEOUT:
        printf("%s: response timeout\n", query);
        return;
    case AT_RESP_ERROR:
        printf("%s: device error\n", query);
        return;
    case AT_RESP_ABORT:
        /* Ordinary callbacks are not invoked by the core abort path. */
        printf("%s: aborted\n", query);
        return;
    default:
        printf("%s: unexpected result %d\n", query, (int)resp_p->code);
        return;
    }

    printf("%s: %.*s\n", query,
           (int)resp_p->recvcnt, resp_p->recvbuf);

    /* Only use this format if the module reports +BAUD: <numeric value>. */
    if (strcmp(query, "baudrate") == 0) {
        if (at_parse_baudrate(resp_p->prefix, &baud)) {
            printf("baud=%" PRIu32 "\n", baud);
        } else {
            printf("baudrate: invalid response format\n");
        }
    }
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

/**
 * Accept a positive decimal baud value occupying its own +BAUD response line.
 * Reject signed values, overflow and trailing non-whitespace characters.
 */
static bool at_parse_baudrate(const char * text_p, uint32_t * baud_p)
{
    const char *  value_p = text_p;
    char *        end_p   = NULL;
    uintmax_t     value   = 0;

    if (value_p == NULL || baud_p == NULL) {
        return false;
    }
    if (strncmp(value_p, "+BAUD:", 6) != 0) {
        return false;
    }
    value_p += 6;

    while (*value_p == ' ' || *value_p == '\t') {
        value_p++;
    }
    if (*value_p < '0' || *value_p > '9') {
        return false;
    }
    errno = 0;
    value = strtoumax(value_p, &end_p, 10);

    if (errno == ERANGE || value == 0 || value > UINT32_MAX) {
        return false;
    }
    while (*end_p == ' ' || *end_p == '\t') {
        end_p++;
    }
    if (*end_p != '\r' && *end_p != '\n' && *end_p != '\0') {
        return false;
    }
    *baud_p = (uint32_t)value;
    return true;
}

/**
 * Report the result of the basic AT communication check.
 */
static void at_ready_response(at_response_t * resp_p)
{
    at_device_on_response("ready", resp_p);
}

/**
 * Forward the Bluetooth address response to application processing.
 */
static void at_address_response(at_response_t * resp_p)
{
    at_device_on_response("bdaddr", resp_p);
}

/**
 * Forward the baud rate response to application processing.
 */
static void at_baudrate_response(at_response_t * resp_p)
{
    at_device_on_response("baudrate", resp_p);
}

/**
 * Report the control result only after the device response is received.
 */
static void at_output_response(at_response_t * resp_p)
{
    at_device_on_response("output", resp_p);
}

/**
 * Handle the result of a parameterized baud rate setting request.
 * Add protocol-specific host UART reconfiguration in application processing.
 */
static void at_set_baudrate_response(at_response_t * resp_p)
{
    at_device_on_response("set_baudrate", resp_p);
}
