# AT-client-cmd

[![License](https://img.shields.io/badge/license-Apache%202-green.svg)](./LICENSE)

[简体中文](./README.md)

AT-client-cmd is an asynchronous communication component for modems, Wi-Fi modules, Bluetooth modules, and other devices using AT commands or ASCII command lines.

The application submits requests and polls from its main loop or task; the framework handles transmission, response matching, timeouts, retries, and callbacks.

## How It Works

```text
Application request → Queue → Poll and send → Write → Device
Application callback ← Response matching and timeouts ← Read ← Device
```

Each AT object binds to a set of transport interfaces and executes requests sequentially. The application can perform other work while waiting for a response, but must keep polling. Unsolicited result codes (URCs) from the device can be dispatched through a separate handler table.

The framework schedules communication; the application defines commands, response formats, and device logic.

## Features

- Single-line, batch, parameterized, and custom commands.
- Response timeouts, error retries, and high/low priority queues.
- URC reports and variable-length data reception.
- Multiple devices, custom multi-stage jobs, and bidirectional transparent transfer.
- Job state tracking, memory statistics, and allocation limits.

## Reading Order

The detailed guides are in Chinese:

1. [How the Framework Works](./docs/architecture.md): request queues, state machines, callbacks, and URC.
2. [Platform and Device Integration](./docs/porting.md): memory, clocks, serial drivers, object creation, and polling.
3. [Querying and Controlling Devices](./docs/usage.md): check communication, query devices, send parameterized commands, and parse responses.

Additional documentation: [API Reference](./docs/api-reference.md) and [Advanced Usage](./docs/advanced-usage.md).

## Quick Start

### Integration Requirements

- Dynamic memory allocation, C99, and the GNU extensions used by the code; GCC can use `-std=gnu99`.
- A reliable millisecond clock and non-blocking Write/Read interfaces. Writes must accept the complete data.
- A memory budget sized for responses, URC frames, and pending requests; see [at_port.h](./include/at_port.h).
- Continuous polling for each object. The framework does not create threads, and callbacks run in the polling context.

### C Examples

The following four C files can be included in an embedded application, without a main function, board template, or IDE project. The standalone Linux program is described below.

| File | Contents |
| --- | --- |
| [at_transport_sample.c](./samples/at_transport_sample.c) | Connect Write/Read to UART queues |
| [at_port_sample.c](./samples/at_port_sample.c) | Memory allocation, release, and a millisecond clock |
| [at_device_sample.c](./samples/at_device_sample.c) | Object initialization, ticks, and polling |
| [at_commands_sample.c](./samples/at_commands_sample.c) | AT probing, address/baud queries, parameterized settings, and callbacks |

### Integration Steps

1. **Add the source files**

   Add the four C files above and `src/at_chat.c` to your application, with `include` and `samples` on the header search path. Public declarations are in [at_device_sample.h](./samples/at_device_sample.h). When using the sample port, do not also compile `src/at_port.c`, to avoid duplicate platform definitions.

2. **Connect the transport**

   Implement `board_uart_tx_enqueue` and `board_uart_rx_dequeue`. TX must copy and accept all bytes before returning. RX reads only available data, returns `0` when empty, and never exceeds the requested byte count.

3. **Provide the system tick**

   Initialize the UART and system clock. Call `at_device_tick_inc(elapsed_ms)` from the system tick with the actual elapsed milliseconds.

4. **Initialize and poll**

   Call `at_device_init()` and check its return value. On success, keep calling `at_device_process()` from the main loop or task. The example processes at a minimum interval of 5 ms; do not call it from an interrupt.

5. **Submit commands and handle results**

   Submit an AT probe, then query or control the device after success. Handle results in `at_device_on_response()`.

### Connecting the Tick and Main Loop

The system tick advances time; the main loop handles communication and callbacks. Connect both:

```c
#include "at_device_sample.h"

/* Call from the existing 1 ms timer interrupt or system tick hook. */
void app_tick_1ms(void)
{
    at_device_tick_inc(1U);
}

/* Call once from main, after UART and system timer initialization. */
bool app_at_run(void)
{
    if (!at_device_init()) {
        return false;
    }
    if (!at_device_check_ready()) {
        at_device_deinit();
        return false;
    }

    for (;;) {
        at_device_process();
        /* Run other non-blocking application work here. */
    }
}
```

Connect `app_tick_1ms()` to an existing 1 ms timer interrupt or system tick hook; it is not invoked automatically. For a 10 ms timer, pass `10U` instead. Use one time source, and never advance time by counting main-loop iterations.

Call `app_at_run()` from the application entry point after initializing the UART and timer. Handle startup failure if it returns `false`. The probe is submitted once; wait for the successful `ready` callback before submitting queries or controls. To use an existing main loop, put initialization before it and `at_device_process()` inside it.

`at_device_process()` advances communication at a minimum interval of 5 ms and executes callbacks in the calling context. Keep it outside interrupts, and do not block a callback waiting for another command. With an RTOS, poll from one task and yield appropriately while the system tick updates time independently. Ensure atomic counter reads on the target, adding a critical section where necessary.

The standalone Linux example reads `CLOCK_MONOTONIC` directly and does not call `at_device_tick_inc()`. Its loop in `at_wait_response()` calls `at_obj_process()` and uses `poll()` to wait for serial events.

### FreeRTOS Integration

[at_freertos_sample.c](./samples/at_freertos_sample.c) demonstrates tick-hook timekeeping and polling from a dedicated task. Add it to the four embedded examples above. It converts the configured tick rate to milliseconds; merge existing tick hooks and avoid duplicate time updates. See [FreeRTOS integration](./docs/porting.md#freertos-tick-与任务对接) for configuration, task creation, and tickless idle limitations (Chinese).

### Querying and Controlling

| Example API | Command Sent | Purpose |
| --- | --- | --- |
| `at_device_check_ready()` | `AT` | Check basic communication |
| `at_device_query_address()` | `AT+LBDADDR?` | Query the device address |
| `at_device_query_baudrate()` | `AT+BAUD?` | Query the baud rate |
| `at_device_set_baudrate(115200U)` | `AT+BAUD=115200` | Set the baud rate |
| `at_device_set_output(1U)` | `AT+OUTIO=1` | Demonstrate parameterized control |

Check every submission's return value and inspect `resp_p->code` in the callback; `AT_RESP_OK` means the response matched successfully. Wait for a successful AT probe before submitting device operations. These device commands are illustrative and must be adapted to the target module manual.

### Parameterized Command Example

This function submits a setting request to an initialized object. It formats the `uint32_t` parameter with `PRIu32`; the framework copies the generated command and appends CRLF when transmitting:

```c
#include "at_chat.h"

#include <inttypes.h>
#include <stddef.h>

bool app_set_baudrate(at_obj_t * device_p, uint32_t baudrate,
                     at_callback_t callback)
{
    at_attr_t attr;

    if (device_p == NULL || baudrate == 0) {
        return false;
    }

    at_attr_deinit(&attr);
    attr.cb      = callback;
    attr.suffix  = "OK";
    attr.timeout = 1200;
    attr.retry   = 0;

    return at_exec_cmd(device_p, &attr, "AT+BAUD=%" PRIu32, baudrate);
}
```

> A return value of `true` only means the request was queued; handle the device result in the callback.

`AT+BAUD=<value>` is illustrative syntax: verify the supported command and values, as well as when to reconfigure the host UART, against the module protocol. See [Querying and Controlling Devices](./docs/usage.md) for the complete workflow.

## Linux Serial Example

[at_linux_sample.c](./samples/at_linux_sample.c) is a standalone command-line program with `main`, `termios` serial configuration, non-blocking Write/Read, a TX buffer, and a monotonic clock. Compile it only with the core file; exclude other examples and `src/at_port.c`.

Build from the repository root on Linux:

```sh
cc -std=gnu99 -Wall -Wextra -Iinclude src/at_chat.c samples/at_linux_sample.c -o at_linux_sample
```

| Operation | Command |
| --- | --- |
| Probe, then query device information (default: `ATI`) | `./at_linux_sample /dev/ttyUSB0 115200` |
| Probe, then run a specific query | `./at_linux_sample /dev/ttyUSB0 115200 'AT+BAUD?'` |
| Probe, then send `AT+OUTIO=1` | `./at_linux_sample /dev/ttyUSB0 115200 AT+OUTIO 1` |

The third argument is the command text. With a fourth argument, the program sends `command=value`, accepting values from `0` to `UINT32_MAX`. CRLF is appended automatically; do not include line endings.

The serial port uses **8N1 with no flow control**, supporting 9600, 19200, 38400, 57600, 115200, and 230400 baud. Replace the device path, ensure your user has read/write access to the port, and select commands from the device manual.

The program sends `AT` first and executes the selected command only after success. Each request waits for `OK`, with a 2-second timeout and no retries. Callbacks print result codes and response data. Exit status is `0` on success and nonzero on failure or interruption. Press `Ctrl+C` to exit; cleanup releases the object and restores the original host serial settings. If a command changes the device baud rate, use its new baud rate for subsequent connections.

## Integration Checks

| Symptom | Check First |
| --- | --- |
| Initialization fails or submission returns `false` | Object creation and available memory |
| Commands are not transmitted | Advancing millisecond clock, continuous polling, and TX queue transmission |
| The device replies but requests time out | UART settings, RX queue, response matching, and receive buffer size |
| Other requests stall after a callback | A callback blocking or waiting for another AT request to finish |

The examples use a single execution context without locks. See [Advanced Usage](./docs/advanced-usage.md) for multitasking and URC configuration.

Before calling `at_device_deinit()`, stop all other access to the device object; destruction does not invoke completion callbacks for pending requests.

## License

This project is licensed under the [Apache License 2.0](./LICENSE).
