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

以下四个 C 文件可加入嵌入式应用，不包含 `main`、板级模板或 IDE 工程。Linux 独立程序见下方说明。

| 文件 | 内容 |
| --- | --- |
| [at_transport_sample.c](./samples/at_transport_sample.c) | Write/Read 对接串口收发队列 |
| [at_port_sample.c](./samples/at_port_sample.c) | 内存分配、释放与毫秒时钟 |
| [at_device_sample.c](./samples/at_device_sample.c) | 对象初始化、节拍和轮询 |
| [at_commands_sample.c](./samples/at_commands_sample.c) | AT 探测、地址与波特率查询、带参数设置和回调 |

### 接入步骤

1. **添加源文件**

   将上述四个 C 文件与 `src/at_chat.c` 加入工程，头文件路径添加 `include` 和 `samples`。公共声明见 [at_device_sample.h](./samples/at_device_sample.h)。使用示例平台文件时不要同时编译 `src/at_port.c`，避免重复定义平台接口。

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

将 `app_tick_1ms()` 接到已有的 1 ms 定时中断或系统 tick 钩子；它不会自动执行。定时周期为 10 ms 时改为传入 `10U`，只保留一个计时来源，不能按主循环次数累加时间。

主入口完成 UART 与定时器初始化后调用 `app_at_run()`，返回 `false` 时由应用处理启动失败。探测只提交一次，收到 `ready` 成功回调后再提交设备查询或控制。已有主循环时，将初始化部分放在循环前，将 `at_device_process()` 放进原循环即可。

`at_device_process()` 内部至少间隔 5 ms 推进一次通信；回调也在该调用中执行，不能放进中断，也不能在回调中阻塞等待另一条指令。RTOS 中由一个任务轮询并适当让出 CPU，系统 tick 独立更新计时。目标平台需保证计数器读取原子性，必要时增加临界区。

Linux 独立例程直接通过 `CLOCK_MONOTONIC` 取时间，无需调用 `at_device_tick_inc()`；其循环在 `at_wait_response()` 中调用 `at_obj_process()` 并通过 `poll()` 等待串口事件。

### FreeRTOS 接入

[at_freertos_sample.c](./samples/at_freertos_sample.c) 演示 tick 钩子计时与独立任务轮询，与上述四个嵌入式例程一起使用。支持按实际 tick 频率换算毫秒；已有 tick 钩子需合并，避免重复计时。配置、任务创建和 tickless idle 限制见 [FreeRTOS 对接说明](./docs/porting.md#freertos-tick-与任务对接)。

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

[at_linux_sample.c](./samples/at_linux_sample.c) 是独立的命令行程序，包含 `main`、`termios` 串口配置、非阻塞 Write/Read、发送缓冲区和单调时钟。仅与核心文件编译，不要同时加入其他例程或 `src/at_port.c`。

在 Linux 的仓库根目录编译：

```sh
cc -std=gnu99 -Wall -Wextra -Iinclude src/at_chat.c samples/at_linux_sample.c -o at_linux_sample
```

| 操作 | 运行命令 |
| --- | --- |
| 探测后查询设备信息（默认 `ATI`） | `./at_linux_sample /dev/ttyUSB0 115200` |
| 探测后执行指定查询 | `./at_linux_sample /dev/ttyUSB0 115200 'AT+BAUD?'` |
| 探测后发送 `AT+OUTIO=1` | `./at_linux_sample /dev/ttyUSB0 115200 AT+OUTIO 1` |

第三个参数为命令文本；提供第四个参数时，程序以 `命令=数值` 格式发送，数值范围为 `0`～`UINT32_MAX`。命令自动追加 CRLF，不要自行添加换行。

串口使用 **8N1、无流控**，支持 9600、19200、38400、57600、115200 和 230400 波特率。替换设备路径，确保当前用户具有串口读写权限，并按设备手册选择指令。

程序先发送 `AT`，成功后才执行指定指令；每条请求等待 `OK`，超时 2 秒，不自动重试。回调打印结果码和响应内容，成功退出码为 `0`，失败或中断为非零。按 `Ctrl+C` 可退出，退出时释放对象并恢复主机原串口配置。若发送修改设备波特率的指令，后续连接需使用设备的新波特率。

## 接入检查

| 现象 | 优先检查 |
| --- | --- |
| 初始化失败或提交返回 `false` | 对象是否创建成功、内存是否充足 |
| 指令没有发出 | 毫秒计数是否递增、轮询是否持续执行、TX 队列是否正常发送 |
| 设备有回复但请求超时 | UART 参数、RX 队列、响应匹配条件和接收缓冲区大小 |
| 回调后其他请求停滞 | 回调是否阻塞或等待另一个 AT 请求完成 |

例程采用单一执行上下文，未配置锁；多任务接入与 URC 配置见 [进阶功能](./docs/advanced-usage.md)。

停用设备时，先停止其他访问，再调用 `at_device_deinit()`；销毁对象不会为未完成请求触发完成回调。

## 许可证

本项目使用 [Apache License 2.0](./LICENSE)。
