/**
 * @file at_linux_sample.c
 * Linux integration snippets: serial transport, clock, object and commands.
 * Place each part in the corresponding application driver or service.
 * The application opens/configures serial_fd (raw 8N1, O_NONBLOCK), owns
 * its lifetime and handles io_failed. Command strings depend on the device.
 */

/*********************
 *      INCLUDES
 *********************/

#include "at_chat.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*********************
 *      DEFINES
 *********************/

#define AT_TX_BUFFER_SIZE 1024U

/**********************
 *  STATIC VARIABLES
 **********************/

static int       serial_fd = -1;
static uint8_t   tx_buffer[AT_TX_BUFFER_SIZE];
static uint32_t  tx_count = 0;
static bool      io_failed = false;
static at_obj_t * device_p = NULL;

/**********************
 *    PLATFORM PORT
 **********************/

/**
 * Use these implementations in the application's AT platform port.
 */
void * at_malloc(unsigned int size)
{
    return malloc(size);
}

/**
 * Release memory allocated by the AT platform port.
 */
void at_free(void * memory_p)
{
    free(memory_p);
}

/**
 * Read monotonic milliseconds; no manual tick increment is needed.
 */
unsigned int at_get_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        io_failed = true;
        return 0;
    }
    return (unsigned int)((uint64_t)now.tv_sec * 1000U +
                          (uint64_t)now.tv_nsec / 1000000U);
}

/**********************
 *  SERIAL TRANSPORT
 **********************/

/**
 * Example driver setup: raw 8N1, 115200 baud, no flow control.
 * Adapt the device path and speed to the target module.
 * The application closes serial_fd after stopping all AT access.
 */
bool at_linux_serial_open(const char * path_p)
{
    struct termios settings;

    serial_fd = open(path_p, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (serial_fd < 0) {
        return false;
    }
    if (tcgetattr(serial_fd, &settings) != 0) {
        close(serial_fd);
        serial_fd = -1;
        return false;
    }
    cfmakeraw(&settings);
    settings.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
    settings.c_cflag |= CS8 | CLOCAL | CREAD;
    settings.c_cc[VMIN]  = 0;
    settings.c_cc[VTIME] = 0;

    if (cfsetispeed(&settings, B115200) != 0 ||
        cfsetospeed(&settings, B115200) != 0 ||
        tcsetattr(serial_fd, TCSANOW, &settings) != 0) {
        close(serial_fd);
        serial_fd = -1;
        return false;
    }
    return true;
}

/**
 * Copy all bytes: the framework does not retry short adapter writes.
 */
static unsigned int at_serial_write(const void * data_p, unsigned int len)
{
    if (len > sizeof(tx_buffer) - tx_count) {
        fprintf(stderr, "TX queue full\n");
        io_failed = true;
        return 0;
    }
    memcpy(tx_buffer + tx_count, data_p, len);
    tx_count += len;
    return len;
}

/**
 * Read available bytes without waiting; return zero when none are ready.
 */
static unsigned int at_serial_read(void * data_p, unsigned int len)
{
    ssize_t count = read(serial_fd, data_p, len);

    if (count < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            perror("read");
            io_failed = true;
        }
        return 0;
    }
    return (unsigned int)count;
}

/**
 * Retain pending bytes after partial writes, EAGAIN or EINTR.
 */
static void at_serial_flush(void)
{
    ssize_t count;

    if (tx_count == 0) {
        return;
    }
    count = write(serial_fd, tx_buffer, tx_count);
    if (count < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            perror("write");
            io_failed = true;
        }
        return;
    }
    tx_count -= (uint32_t)count;
    memmove(tx_buffer, tx_buffer + count, tx_count);
}

/**********************
 *  DEVICE LIFECYCLE
 **********************/

/**
 * Call once after assigning serial_fd and configuring the UART.
 */
bool at_linux_init(void)
{
    static const at_adapter_t adapter = {
        .write        = at_serial_write,
        .read         = at_serial_read,
        .recv_bufsize = 512
    };

    if (device_p != NULL) {
        return true;
    }
    device_p = at_obj_create(&adapter);

    return device_p != NULL;
}

/**
 * Call repeatedly from the application's event loop or one owning task.
 * The loop may use poll() with a short timeout (for example 5 ms).
 * Keep polling even when RX is empty so command timeouts can advance.
 * On io_failed, stop submitting requests and let the application recover.
 */
void at_linux_process(void)
{
    if (device_p == NULL || io_failed) {
        return;
    }
    at_obj_process(device_p);
    if (!io_failed) {
        at_serial_flush();
    }
}

/**
 * Stop other access first; the application closes its own serial fd.
 */
void at_linux_deinit(void)
{
    if (device_p != NULL) {
        at_obj_destroy(device_p);
        device_p = NULL;
    }
    tx_count  = 0;
    io_failed = false;
}

/**********************
 *  COMMAND OPERATIONS
 **********************/

/**
 * Process or copy response data before this callback returns.
 */
static void at_linux_response(at_response_t * resp_p)
{
    printf("result=%d\n", (int)resp_p->code);
    if (resp_p->recvcnt != 0) {
        fwrite(resp_p->recvbuf, 1, resp_p->recvcnt, stdout);
        putchar('\n');
    }
}

/**
 * Submit "AT" first. After its successful callback, submit "ATI" or a query.
 * The command string must remain valid until the request completes.
 */
bool at_linux_query(const char * command_p)
{
    at_attr_t attr;

    if (device_p == NULL || io_failed || command_p == NULL) {
        return false;
    }
    at_attr_deinit(&attr);
    attr.cb      = at_linux_response;
    attr.suffix  = "OK";
    attr.timeout = 2000;
    attr.retry   = 0;

    return at_send_singlline(device_p, &attr, command_p);
}

/**
 * Send AT+OUTIO=<value>; adapt the command to the target device.
 */
bool at_linux_set_output(uint32_t value)
{
    at_attr_t attr;

    if (device_p == NULL || io_failed) {
        return false;
    }
    at_attr_deinit(&attr);
    attr.cb      = at_linux_response;
    attr.suffix  = "OK";
    attr.timeout = 2000;
    attr.retry   = 0;

    return at_exec_cmd(device_p, &attr, "AT+OUTIO=%" PRIu32, value);
}
