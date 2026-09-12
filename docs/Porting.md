# 平台移植与设备驱动对接

理解[工作原理](architecture.md)后，下一步是建立框架到实际设备的数据通道。本篇使用 [at_device_sample.c](../samples/at_device_sample.c) 作为接入骨架：完成本篇后，对象能够正常创建，收发接口可用，轮询能够持续运行。

查询和控制指令在[下一篇](usage.md)实现，避免把硬件接入和业务解析混在一起。

Linux 用户可参考 [at_linux_sample.c](../samples/at_linux_sample.c) 中的串口、时钟和指令片段，接入顺序见 [README](../README.md#linux-串口示例)。

## 第一步：选择需要的接入片段

按功能查阅以下实现，并将需要的逻辑放入应用对应模块：

| 文件 | 作用 |
| --- | --- |
| `src/at_chat.c` | AT 框架核心 |
| `samples/at_device_sample.c` | 对象创建、节拍和轮询 |
| `samples/at_transport_sample.c` | AT Write/Read 到底层串口队列的包装 |
| `samples/at_port_sample.c` | 内存分配、释放与毫秒时钟 |
| `samples/at_commands_sample.c` | 蓝牙查询、控制指令及结果回调 |
| 你的驱动源文件 | 实际串口初始化、发送和接收队列 |

例程按接口职责组织。先选择目标平台的时钟与轮询方式，再将通信、对象和业务片段接入已有应用。跨片段函数由应用头文件声明。

| 平台 | 时间来源 | 轮询位置 |
| --- | --- | --- |
| 裸机 | 定时中断调用 `at_device_tick_inc()` | 主循环调用 `at_device_process()` |
| FreeRTOS | tick 钩子按频率换算毫秒 | 一个任务调用 `at_device_process()` |
| Linux | `clock_gettime(CLOCK_MONOTONIC)` | 事件循环或任务调用 `at_linux_process()` |

核心使用 C99 特性和 GNU 扩展，使用 GCC 时可选择 `-std=gnu99`。非 GCC 工具链需要确认相应语法支持。

平台层提供 `at_malloc`、`at_free`、`at_get_ms`。可参考 `at_port_sample.c` 的实现，并替换为应用使用的内存与时钟接口。

## 第二步：提供内存和时钟

裸机示例已经提供毫秒计数器。[at_port_sample.c](../samples/at_port_sample.c) 已用标准分配器并将核心时钟接到该计数器，下面展示对应实现：

```c
#include "at_chat.h"

/* Include the application declarations for the at_device_* snippets. */
#include <stdlib.h>

void * at_malloc(unsigned int size)
{
    return malloc(size);
}

void at_free(void * memory_p)
{
    free(memory_p);
}

unsigned int at_get_ms(void)
{
    return (unsigned int)at_device_get_tick();
}
```

在已有的系统节拍中调用 `at_device_tick_inc`，参数是实际经过的毫秒数。例如 1ms 中断传 1，10ms 中断传 10。计数持续增加，核心才能判断超时，示例才能按周期轮询。

读取节拍需要在目标 CPU 上可靠；较窄位宽的平台可能需要临界区保护。不要把 AT 协议轮询放入节拍中断，只在中断中更新时间。

如果使用操作系统自己的时钟，应返回单调推进的毫秒计数，避免系统校时影响超时判断。同步调整 `at_device_get_tick()`，让轮询节拍和框架超时使用同一时间来源。内存分配器需要满足平台的对齐和并发要求。

## 第三步：连接真实设备的收发驱动

先在板级代码中完成串口引脚、波特率、数据位、停止位、校验及中断或 DMA 的初始化，再把收发队列接到示例接口。

数据流如下：

```text
发送：AT 框架 → write → 驱动发送队列 → 中断或 DMA → 设备
接收：设备 → 中断或 DMA → 驱动接收队列 → read → AT 框架
```

已有的串口发送和接收队列可以封装在板级接口中。[at_transport_sample.c](../samples/at_transport_sample.c) 展示驱动包装方式。下面使用核心适配器要求的 `unsigned int` 签名：将 `board_uart_tx_enqueue` 和 `board_uart_rx_dequeue` 替换为实际驱动接口，包装方式如下：

```c
#include "at_chat.h"

#include <stddef.h>

/* Include the application declarations for the at_device_* snippets. */

extern unsigned int board_uart_tx_enqueue(const void * data_p, unsigned int len);
extern unsigned int board_uart_rx_dequeue(void * data_p, unsigned int len);

unsigned int at_device_write(const void * data_p, unsigned int len)
{
    if (data_p == NULL || len == 0) {
        return 0;
    }
    return board_uart_tx_enqueue(data_p, len);
}

unsigned int at_device_read(void * data_p, unsigned int len)
{
    if (data_p == NULL || len == 0) {
        return 0;
    }
    return board_uart_rx_dequeue(data_p, len);
}
```

核心适配器的读写签名使用 `unsigned int`，因此这里保持与 `at_adapter_t` 一致；不能因为业务参数使用 `uint32_t`，就直接改变函数指针要求的类型。收发例程中的 `uint32_t` 写法要求目标平台将其定义为 `unsigned int`；若底层类型不同，应按核心签名调整包装函数。

这两个驱动接口应满足以下约定：

| 接口 | 必须满足的行为 |
| --- | --- |
| 读取 | 非阻塞；没有数据返回 0；写入调用者缓冲区的字节数不超过 len |
| 写入 | 将本次数据可靠复制到驱动管理的发送空间；核心不会自动补发部分写入 |
| 数据缓冲区 | 不保存调用者的临时地址后直接用于异步 DMA；所需数据在调用期间复制 |
| 返回值 | 返回实际字节数，不把负数错误码转换成无符号长度 |

驱动需保证发送队列容量与吞吐量。单纯返回一个小于 len 的长度不能解决发送不完整的问题。原始数据也不一定以 NUL 结尾，不应直接用 `%s` 打印任意发送缓冲区。

## 第四步：检查适配器配置

[设备片段](../samples/at_device_sample.c) 展示常驻适配器如何绑定上述收发函数，响应缓冲区为 256 字节。将适配器和对象保存在应用的设备模块中。

| 配置 | 接入时的选择 |
| --- | --- |
| `recv_bufsize` | 按最长响应加终止空间配置，当前实现建议至少 65 字节 |
| `urc_bufsize` | 示例为 0；需要主动消息时设置非零容量并注册订阅表 |
| `lock` / `unlock` | 示例为单执行上下文，未设置；线程方案见进阶文档 |
| `debug` | 可接收格式化日志，也可以为 `NULL` |
| `error` | 示例未使用；具体错误在请求完成回调中处理 |

当前核心每次最多读取 64 字节，而创建函数只把较小缓冲区提升到 32 字节，因此不要依赖这个最小值。响应累计或 URC 数据达到容量时会清空，不能靠它自动扩容。

适配器 `error` 回调目前在响应结构初始化前调用，不能读取其参数字段。业务错误码应从下一篇使用的请求完成回调获取。

## 第五步：初始化并持续轮询

### 接入已有应用

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

Linux 片段直接通过 `CLOCK_MONOTONIC` 取时间，无需调用 `at_device_tick_inc()`；将 `at_linux_process()` 接入应用事件循环，持续推进通信与超时。

不要再对同一对象调用另一套并发轮询。当前流程采用单执行上下文，其他任务有请求时可以通过应用消息队列交给该上下文提交。

[at_commands_sample.c](../samples/at_commands_sample.c) 展示 `at_device_on_response` 业务回调，可按[接收与处理结果](usage.md#接收与处理结果)将解析与通知逻辑接入应用。

## FreeRTOS tick 与任务对接

[at_freertos_sample.c](../samples/at_freertos_sample.c) 展示 tick 钩子和 AT 任务体，内部调用对应设备与指令片段。将钩子合入已有系统钩子，将任务体接入应用；平台层的 `at_get_ms()` 从设备毫秒计数器取时间。

在已有 `FreeRTOSConfig.h` 中设置：

```c
#define configUSE_TICK_HOOK     1
#define configUSE_TICKLESS_IDLE 0
#define INCLUDE_vTaskDelay     1
#define INCLUDE_vTaskDelete    1
```

保留项目实际的 `configTICK_RATE_HZ`。tick 钩子在中断中执行，只更新时间，不执行通信或回调；已有 `vApplicationTickHook()` 时将例程函数体合并进去，避免重复定义。参见 [FreeRTOS 官方例程](https://github.com/FreeRTOS/FreeRTOS/blob/main/FreeRTOS/Demo/Posix_GCC/main.c)。

| `configTICK_RATE_HZ` | AT 时间更新 |
| --- | --- |
| 1000 Hz | 每 tick 增加 1 ms |
| 100 Hz | 每 tick 增加 10 ms |
| 128 Hz | 累积小数余量，128 tick 合计增加 1000 ms |

例程保留换算余量，不直接使用整数 `portTICK_PERIOD_MS` 累加，避免截断造成计时偏差。移除原来 SysTick 或其他定时器中的 `at_device_tick_inc()`，只保留一个计时来源。

UART 收发队列就绪后，在应用初始化中创建一次任务：

```c
#include "FreeRTOS.h"
#include "task.h"

extern void at_freertos_task(void * argument_p);

/* Call once; choose stack depth and priority for the target application. */
BaseType_t app_at_task_create(configSTACK_DEPTH_TYPE stack_depth,
                             UBaseType_t priority)
{
    return xTaskCreate(at_freertos_task, "at", stack_depth,
                       NULL, priority, NULL);
}
```

检查返回值是否为 `pdPASS`；栈深度按 FreeRTOS 栈元素计数，并预留响应回调和 `printf` 的用量。任务中持续调用 `at_device_process()`，再调用 `vTaskDelay()` 让出 CPU；等待时间至少为一个 tick，不会因毫秒换算得到 0 而空转。不要再从主循环或其他任务轮询同一个对象。

先等待 `ready` 成功回调，再在该任务上下文提交设备操作。其他任务通过应用消息队列发送操作请求；当前例程没有配置框架锁。初始化或探测入队失败时，该任务结束，可按应用需要增加错误上报。

此方案适用于单核、持续 tick 的配置，并要求平台能原子读取 32 位计数器。启用 tickless idle 时，被抑制的 tick 不会逐个调用该钩子，因此该钩子计时方式不适用；需要低功耗时应改用休眠期间仍可靠计时的单调时钟，并同步替换 `at_device_get_tick()` 的时间来源，保证轮询节拍和框架超时使用同一时钟。不要仅替换 `at_get_ms()` 而留下不再更新的设备计数器。

## 如何确认驱动已经接通

- 对象初始化返回 `true`，毫秒计数持续增加。
- 没有数据时，读取接口立即返回 0，不会卡住主循环。
- 串口参数、硬件连线和设备模式符合目标模块的要求。
- 主循环持续运行，驱动收发队列没有覆盖或丢弃字节。

这时按[查询与控制设备](usage.md)发送第一条查询，进一步验证完整请求链路。

## 按需要调整资源

配置定义位于 [at_port.h](../include/at_port.h)。先保留默认值，再根据最长响应、URC 帧和并发请求量调整。

| 配置项 | 当前默认值 | 用途 |
| --- | --- | --- |
| `AT_DEF_RESP_OK` / `AT_DEF_RESP_ERR` | `"OK"` / `"ERROR"` | 默认成功后缀与错误标记 |
| `AT_DEF_TIMEOUT` / `AT_DEF_RETRY` | 500ms / 2 | 普通命令默认等待时间及额外重试次数 |
| `AT_URC_TIMEOUT` | 500ms | URC 接收间隔超时 |
| `AT_MAX_CMD_LEN` | 256 | 格式化命令空间，需为 NUL 留位置 |
| `AT_LIST_WORK_COUNT` | 32 | 队列数量限制参数，允许的最小值为 2 |
| `AT_MEM_LIMIT_SIZE` | 3 × 1024 | 全部对象共享的组件内存限制参数 |
| `AT_URC_WARCH_EN` | 1 | URC 功能开关，名称按源码拼写 |
| `AT_URC_END_MARKS` | `":,\n"` | URC 结束字符集合 |
| `AT_MEM_WATCH_EN` | 1u | 内存统计与限制 |
| `AT_WORK_CONTEXT_EN` | 1u | 上下文接口 |
| `AT_RAW_TRANSPARENT_EN` | 1u | 双向透传 |

当前代码的队列判断使用 `>`，不是严格的 32 项硬上限。内存限制和统计的计数头存在边界差异，详见[内存与多实例](advanced-usage.md#内存与多实例)。关闭 URC 编译开关时，`at_obj_busy` 仍引用被条件编译掉的成员；若仅某个对象不需要 URC，可保留编译开关并将其 `urc_bufsize` 设为 0。
