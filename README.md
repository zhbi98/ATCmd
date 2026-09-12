# AT-client-cmd

[![License](https://img.shields.io/badge/license-Apache%202-green.svg)](./LICENSE)

[English](./README.en.md)

AT-client-cmd 是一个异步 AT 命令通信组件，适用于 Modem、Wi-Fi、蓝牙等使用 AT 命令或 ASCII 命令行通信的设备。

应用提交请求，由主循环或任务持续轮询，框架完成发送、响应匹配、超时重试和回调通知。

## 工作方式

```text
应用提交指令 → 请求队列 → 轮询发送 → Write → 设备
应用处理回调 ← 响应匹配与超时处理 ← Read ← 设备
```

每个 AT 对象绑定一组通信接口，按顺序执行请求。等待响应期间，应用可以继续处理其他任务，但必须持续调用轮询接口。设备主动发送的 URC（非请求响应消息）可交给独立的处理表。

框架负责通信调度；具体指令、响应格式及设备业务逻辑由应用定义。

## 功能

- 单行、批量、带参数和自定义命令。
- 响应超时、错误重试、高低优先级队列。
- URC 主动上报和不定长度数据接收。
- 多设备、自定义多阶段作业和双向透传。
- 作业状态观察、内存统计与分配限制。

## 阅读顺序

1. [框架工作原理](./docs/architecture.md)：了解请求队列、状态机、回调和 URC。
2. [平台移植与设备驱动对接](./docs/porting.md)：接好内存、时钟和串口驱动，创建对象并持续轮询。
3. [查询与控制设备](./docs/usage.md)：验证通信、查询设备、发送带参数指令并解析响应。

按需查阅：[接口参考](./docs/api-reference.md)、[进阶功能](./docs/advanced-usage.md)。

## 快速接入

### 接入要求

- 支持动态内存分配、C99 和代码使用的 GNU 扩展；GCC 可使用 `-std=gnu99`。
- 提供可靠的毫秒计数和非阻塞 Write/Read 接口，写入接口需要接受完整数据。
- 根据响应长度、URC 帧及待处理请求设置内存预算，配置见 [at_port.h](./include/at_port.h)。
- 为每个对象安排持续轮询；框架本身不创建线程，回调也在轮询上下文中执行。

### C 例程

`samples` 按功能展示对接与使用方法。将需要的片段放入已有驱动、平台层和业务代码，函数声明由应用自己的头文件管理。裸机、FreeRTOS 和 Linux 的时钟及轮询方式按目标平台选用。

| 文件 | 内容 |
| --- | --- |
| [at_transport_sample.c](./samples/at_transport_sample.c) | Write/Read 对接串口收发队列 |
| [at_port_sample.c](./samples/at_port_sample.c) | 内存分配、释放与毫秒时钟 |
| [at_device_sample.c](./samples/at_device_sample.c) | 对象初始化、节拍和轮询 |
| [at_commands_sample.c](./samples/at_commands_sample.c) | AT 探测、地址与波特率查询、带参数设置和回调 |
| [at_freertos_sample.c](./samples/at_freertos_sample.c) | FreeRTOS tick 钩子与任务轮询 |
| [at_linux_sample.c](./samples/at_linux_sample.c) | Linux 串口、单调时钟、对象管理和指令调用 |

### 接入步骤

以下步骤使用嵌入式 `at_device_*` 片段；Linux 对应接口见 [Linux 串口示例](#linux-串口示例)。

1. **选择接入片段**

   根据平台选择通信、时钟、对象和指令片段，将对应逻辑接入应用。核心接口见 `include/at_chat.h`，平台接口见 `include/at_port.h`。

2. **对接通信接口**

   实现 `board_uart_tx_enqueue` 和 `board_uart_rx_dequeue`。发送队列必须在返回前复制并接收全部数据；接收接口只取已有数据，无数据时返回 `0`，最多读取请求的字节数。

3. **提供系统节拍**

   初始化 UART 和系统时钟，在系统节拍中调用 `at_device_tick_inc(elapsed_ms)`，传入实际经过的毫秒数。

4. **初始化并持续轮询**

   调用 `at_device_init()` 并检查返回值；成功后在主循环或任务中持续调用 `at_device_process()`。例程内部以 5 ms 为最小间隔处理一次，不应在中断中调用。

5. **提交指令并处理结果**

   提交 AT 探测，成功后再查询或控制设备，并在 `at_device_on_response()` 中处理结果。

### tick 与主循环对接

系统节拍负责更新时间，主循环负责通信和回调，两处都需要接入：

```c
#include "at_chat.h"

/* Include the application declarations for the at_device_* snippets. */

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

将 `app_tick_1ms()` 接到已有的 1 ms 定时中断或系统 tick 钩子；它不会自动执行。定时周期为 10 ms 时改为传入 `10U`，只保留一个计时来源，不能按主循环次数累加时间。

主入口完成 UART 与定时器初始化后调用 `app_at_run()`，返回 `false` 时由应用处理启动失败。探测只提交一次，收到 `ready` 成功回调后再提交设备查询或控制。已有主循环时，将初始化部分放在循环前，将 `at_device_process()` 放进原循环即可。

`at_device_process()` 内部至少间隔 5 ms 推进一次通信；回调也在该调用中执行，不能放进中断，也不能在回调中阻塞等待另一条指令。RTOS 中由一个任务轮询并适当让出 CPU，系统 tick 独立更新计时。目标平台需保证计数器读取原子性，必要时增加临界区。

Linux 片段通过 `CLOCK_MONOTONIC` 取时间，无需调用 `at_device_tick_inc()`；将 `at_linux_process()` 接入已有事件循环或任务，并持续调用以推进收发和超时。

### FreeRTOS 接入

[at_freertos_sample.c](./samples/at_freertos_sample.c) 演示 tick 钩子计时与独立任务轮询，其中 `at_device_*` 调用对应设备和指令片段。支持按实际 tick 频率换算毫秒；已有 tick 钩子需合并，避免重复计时。配置、任务创建和 tickless idle 限制见 [FreeRTOS 对接说明](./docs/porting.md#freertos-tick-与任务对接)。

### 查询与控制

| 例程接口 | 发送内容 | 用途 |
| --- | --- | --- |
| `at_device_check_ready()` | `AT` | 验证基本通信 |
| `at_device_query_address()` | `AT+LBDADDR?` | 查询设备地址 |
| `at_device_query_baudrate()` | `AT+BAUD?` | 查询波特率 |
| `at_device_set_baudrate(115200U)` | `AT+BAUD=115200` | 设置波特率 |
| `at_device_set_output(1U)` | `AT+OUTIO=1` | 演示带参数控制 |

每次提交都要检查返回值，并通过回调的 `resp_p->code` 判断结果；`AT_RESP_OK` 表示本次响应匹配成功。先等待 AT 探测成功，再提交设备操作。上述设备指令是示例，需按目标模块手册调整。

### 带参数指令示例

以下函数向已经初始化的对象发送设置请求。`uint32_t` 参数使用 `PRIu32` 格式化，框架复制生成的命令，并在发送时追加 CRLF：

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

> 返回 `true` 仅表示入队成功，设备结果在回调中处理。

`AT+BAUD=<数值>` 是示例语法，需核对模块支持的命令和参数；主机 UART 的切换时机也由设备协议决定。完整流程见 [查询与控制设备](./docs/usage.md)。

## Linux 串口示例

[at_linux_sample.c](./samples/at_linux_sample.c) 按功能展示 Linux 接入方法：

| 部分 | 如何接入 |
| --- | --- |
| 串口 | 参考 `at_linux_serial_open("/dev/ttyUSB0")`，配置 raw、8N1、115200、无流控和非阻塞模式 |
| Write/Read | 写入时复制完整数据，循环中发送队列处理部分写入；读取已有字节 |
| 时钟与内存 | 将单调时钟和内存函数放入应用平台层 |
| 对象 | 驱动就绪后调用 `at_linux_init()`，检查创建结果 |
| 轮询 | 在已有事件循环或任务中持续调用 `at_linux_process()`，即使没有收到数据也要调用 |
| 指令 | 先调用 `at_linux_query("AT")`，成功回调后再查询 `ATI` 或调用 `at_linux_set_output(1U)` |
| 清理 | 停止其他访问后调用 `at_linux_deinit()`，串口关闭由应用负责 |

先检查串口打开与对象初始化结果，再提交探测；发送、查询和清理函数都由已有应用流程调用。`at_linux_query()` 的命令字符串需保持有效直到请求完成。

`io_failed` 表示底层通信或时钟异常，应用应停止提交请求并处理恢复。事件循环可用带短超时的 `poll()` 等待，避免空转。指令提交函数返回 `true` 仅代表入队成功；设备结果在回调中处理。Linux 指令片段等待 `OK`，超时 2000 ms，不自动重试；具体指令按设备协议调整。

## 接入检查

| 现象 | 优先检查 |
| --- | --- |
| 初始化失败或提交返回 `false` | 对象是否创建成功、内存是否充足 |
| 指令没有发出 | 毫秒计数是否递增、轮询是否持续执行、TX 队列是否正常发送 |
| 设备有回复但请求超时 | UART 参数、RX 队列、响应匹配条件和接收缓冲区大小 |
| 回调后其他请求停滞 | 回调是否阻塞或等待另一个 AT 请求完成 |

例程采用单一执行上下文，未配置锁；多任务接入与 URC 配置见 [进阶功能](./docs/advanced-usage.md)。

停用设备时，先停止其他访问，再调用对应的 `at_device_deinit()` 或 `at_linux_deinit()`；销毁对象不会为未完成请求触发完成回调。

## 许可证

本项目使用 [Apache License 2.0](./LICENSE)。
