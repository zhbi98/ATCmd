/**
 * @file at_linux_sample.c
 * Standalone Linux serial example; link only with src/at_chat.c.
 * Usage: ./at_linux_sample /dev/ttyUSB0 115200 [command [value]]
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

/*********************
 *      INCLUDES
 *********************/

#include "at_chat.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
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
#define AT_TIMEOUT_MS     2000U

/**********************
 *  STATIC PROTOTYPES
 **********************/

static void at_stop(int signal_number);
static unsigned int at_serial_write(const void * data_p, unsigned int len);
static unsigned int at_serial_read(void * data_p, unsigned int len);
static void at_serial_flush(void);
static bool at_parse_uint32(const char * text_p, uint32_t * value_p);
static bool at_serial_speed(uint32_t baudrate, speed_t * speed_p);
static void at_response(at_response_t * resp_p);
static bool at_wait_response(at_obj_t * device_p);

/**********************
 *  STATIC VARIABLES
 **********************/

static int serial_fd = -1;
static uint8_t tx_buffer[AT_TX_BUFFER_SIZE];
static uint32_t tx_count = 0;
static bool io_failed = false;
static bool response_done = false;
static bool response_ok = false;
static volatile sig_atomic_t stop_requested = 0;

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
 * Return the monotonic millisecond count used for command timeouts.
 * @note Clock failures stop the application polling loop.
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

/**
 * Configure the serial port, probe AT communication, then run one command.
 * @return EXIT_SUCCESS on a successful response, otherwise EXIT_FAILURE
 * @note Restore the original host serial settings before closing the port.
 */
int main(int argc, char * argv[])
{
    static const at_adapter_t adapter = {
        .write        = at_serial_write,
        .read         = at_serial_read,
        .recv_bufsize = 512
    };
    struct termios   original;
    struct termios   settings;
    struct sigaction action = { 0 };
    at_obj_t *       device_p = NULL;
    at_attr_t        attr;
    speed_t          speed;
    uint32_t         baudrate;
    uint32_t         value = 0;
    const char *     command_p = argc >= 4 ? argv[3] : "ATI";
    char             command[AT_MAX_CMD_LEN];
    int              length;
    int              exit_code = EXIT_FAILURE;
    bool             restore_settings = false;
    bool             queued;

    if (argc < 3 || argc > 5 ||
        !at_parse_uint32(argv[2], &baudrate) ||
        !at_serial_speed(baudrate, &speed) ||
        (argc == 5 && !at_parse_uint32(argv[4], &value))) {
        fprintf(stderr, "Usage: %s device baud [command [uint32_value]]\n"
                        "Baud: 9600, 19200, 38400, 57600, 115200, 230400\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (*command_p == '\0' || strpbrk(command_p, "\r\n") != NULL) {
        fprintf(stderr, "Provide one command without CR/LF\n");
        return EXIT_FAILURE;
    }
    length = argc == 5
        ? snprintf(command, sizeof(command), "%s=%" PRIu32, command_p, value)
        : snprintf(command, sizeof(command), "%s", command_p);
    /* Check before at_exec_cmd: the core formatting buffer must not overflow. */
    if (length < 0 || (size_t)length >= sizeof(command)) {
        fprintf(stderr, "Command too long\n");
        return EXIT_FAILURE;
    }
    action.sa_handler = at_stop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0) {
        perror("sigaction");
        return EXIT_FAILURE;
    }
    serial_fd = open(argv[1], O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (serial_fd < 0) {
        perror("open");
        return EXIT_FAILURE;
    }
    if (tcgetattr(serial_fd, &original) != 0) {
        perror("tcgetattr");
        goto cleanup;
    }
    settings = original;
    cfmakeraw(&settings);
    settings.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
    settings.c_cflag |= CS8 | CLOCAL | CREAD;
    settings.c_cc[VMIN]  = 0;
    settings.c_cc[VTIME] = 0;
    if (cfsetispeed(&settings, speed) != 0 || cfsetospeed(&settings, speed) != 0) {
        perror("cfsetspeed");
        goto cleanup;
    }
    restore_settings = true;
    if (tcsetattr(serial_fd, TCSANOW, &settings) != 0 ||
        tcflush(serial_fd, TCIOFLUSH) != 0) {
        perror("serial configuration");
        goto cleanup;
    }
    device_p = at_obj_create(&adapter);
    if (device_p == NULL) {
        fprintf(stderr, "AT object allocation failed\n");
        goto cleanup;
    }
    at_attr_deinit(&attr);
    attr.cb      = at_response;
    attr.suffix  = "OK";
    attr.timeout = AT_TIMEOUT_MS;
    attr.retry   = 0;

    puts("TX: AT");
    if (!at_send_singlline(device_p, &attr, "AT") || !at_wait_response(device_p)) {
        goto cleanup;
    }
    response_done = false;
    response_ok   = false;
    printf("TX: %s\n", command);
    if (argc == 5) {
        /* Example: AT+OUTIO and 1 produce AT+OUTIO=1 plus automatic CRLF. */
        queued = at_exec_cmd(device_p, &attr, "%s=%" PRIu32, command_p, value);
    } else {
        queued = at_send_singlline(device_p, &attr, command);
    }
    if (queued && at_wait_response(device_p)) {
        exit_code = EXIT_SUCCESS;
    }

cleanup:
    if (device_p != NULL) {
        at_obj_destroy(device_p);
    }
    if (restore_settings && tcsetattr(serial_fd, TCSANOW, &original) != 0) {
        perror("restore serial settings");
        exit_code = EXIT_FAILURE;
    }
    close(serial_fd);
    return exit_code;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

/**
 * Request shutdown from a signal handler.
 * Cleanup runs in the application context after polling stops.
 */
static void at_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

/**
 * Copy a complete request into the serial transmit buffer.
 * @param data_p source bytes, not necessarily NUL-terminated
 * @param len requested byte count
 * @return number of bytes accepted, or zero if the buffer is full
 * @note Preserve the unsigned int signature required by at_adapter_t.
 */
static unsigned int at_serial_write(const void * data_p, unsigned int len)
{
    if (len > sizeof(tx_buffer) - tx_count) {
        fprintf(stderr, "TX queue full\n");
        io_failed = true;
        return 0;
    }
    /* Core does not retry short writes: accept a complete copy here. */
    memcpy(tx_buffer + tx_count, data_p, len);
    tx_count += len;
    return len;
}

/**
 * Read the bytes currently available from the serial port.
 * @param data_p destination buffer
 * @param len maximum number of bytes to read
 * @return received byte count, or zero when no bytes are available
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
 * Send queued bytes without waiting for the serial port.
 * Keep unsent bytes queued on partial writes, EAGAIN or EINTR.
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

/**
 * Parse an unsigned decimal command-line argument.
 * @return true for a value in the uint32_t range, otherwise false
 */
static bool at_parse_uint32(const char * text_p, uint32_t * value_p)
{
    char *    end_p = NULL;
    uintmax_t value;

    if (*text_p < '0' || *text_p > '9') {
        return false;
    }
    errno = 0;
    value = strtoumax(text_p, &end_p, 10);
    if (errno == ERANGE || *end_p != '\0' || value > UINT32_MAX) {
        return false;
    }
    *value_p = (uint32_t)value;
    return true;
}

/**
 * Map a supported baud rate to its termios constant.
 * @return true if the baud rate is supported, otherwise false
 */
static bool at_serial_speed(uint32_t baudrate, speed_t * speed_p)
{
    switch (baudrate) {
    case 9600U:
        *speed_p = B9600;
        return true;
    case 19200U:
        *speed_p = B19200;
        return true;
    case 38400U:
        *speed_p = B38400;
        return true;
    case 57600U:
        *speed_p = B57600;
        return true;
    case 115200U:
        *speed_p = B115200;
        return true;
    case 230400U:
        *speed_p = B230400;
        return true;
    default:
        return false;
    }
}

/**
 * Report the completed request and print its response bytes.
 * @param resp_p response valid only for the duration of this callback
 */
static void at_response(at_response_t * resp_p)
{
    response_done = true;
    response_ok   = resp_p->code == AT_RESP_OK;
    printf("result=%d\n", (int)resp_p->code);
    if (resp_p->recvcnt != 0) {
        fwrite(resp_p->recvbuf, 1, resp_p->recvcnt, stdout);
        putchar('\n');
    }
}

/**
 * Drive UART output and framework polling until the request completes.
 * @return true on a successful response, otherwise false
 */
static bool at_wait_response(at_obj_t * device_p)
{
    while (!response_done && !io_failed && !stop_requested) {
        struct pollfd serial_poll = {
            .fd     = serial_fd,
            .events = POLLIN
        };
        int result;

        at_obj_process(device_p);
        if (io_failed || response_done) {
            break;
        }
        at_serial_flush();
        if (tx_count != 0) {
            serial_poll.events |= POLLOUT;
        }
        result = poll(&serial_poll, 1, 5);
        if (result < 0 && errno != EINTR) {
            perror("poll");
            io_failed = true;
        }
        if (result > 0 && (serial_poll.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            fprintf(stderr, "Serial device disconnected or unavailable\n");
            io_failed = true;
        }
    }
    return response_done && response_ok && !io_failed && !stop_requested;
}
