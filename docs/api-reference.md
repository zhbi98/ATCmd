# 命令接口参考

本页用于查阅接口行为。第一次使用请先完成[驱动对接](porting.md)与[查询流程](usage.md)。声明见 [at_chat.h](../include/at_chat.h)，以下行为按当前 [at_chat.c](../src/at_chat.c) 实现说明。

## 对象与请求

| 接口 | 含义 |
| --- | --- |
| `at_obj_create(adapter)` | 创建对象，失败返回 `NULL`；适配器必须长期有效 |
| `at_obj_process(object)` | 执行一次收发和作业轮询 |
| `at_obj_busy(object)` | 检查队列或 URC 接收是否忙碌 |
| `at_obj_destroy(object)` | 释放对象，调用前停止其他访问 |
| `at_work_abort_all(object)` | 标记全部作业终止，之后仍需轮询回收 |
| `at_obj_set_user_data` / `at_obj_get_user_data` | 关联应用数据，不改变驱动函数参数 |

提交接口返回 `bool`，仅说明作业是否入队。普通请求的最终状态由回调或上下文报告；取消和销毁不调用普通完成回调。

## 选择发送接口

| 调用形式 | 场景 | 换行与数据保存方式 |
| --- | --- | --- |
| `at_send_singlline(at, attr, line)` | 固定单行命令 | 保存字符串指针，发送时追加 CRLF |
| `at_exec_cmd(at, attr, fmt, ...)` | 带参数命令 | 复制格式化结果，追加 CRLF |
| `at_exec_vcmd(at, attr, fmt, args)` | 封装已有 va_list | 与格式化命令相同 |
| `at_send_multiline(at, attr, lines)` | 固定命令表 | 保存表和字符串指针，每条追加 CRLF，表以 `NULL` 结束 |
| `at_send_data(at, attr, data, size)` | 指定长度载荷 | 复制原始字节，不追加换行，仍等待响应 |
| `at_custom_cmd(at, attr, sender)` | 自定义发送步骤 | 执行 sender 后由普通状态机等待响应 |
| `at_do_work(at, params, work)` | 自定义整个交互过程 | 保存参数和函数指针，由应用管理状态、超时和完成通知 |

带 `attr` 参数的接口允许传 `NULL` 使用默认属性。`at_do_work` 不接受属性，它内部建立默认属性并关联 params。

## 命令属性

使用 `at_attr_deinit` 初始化 `at_attr_t`，再按需修改字段。

| 字段 | 默认值 | 说明 |
| --- | --- | --- |
| `prefix` | `NULL` | 普通命令响应前缀 |
| `suffix` | `AT_DEF_RESP_OK` | 默认 `"OK"`，响应结束标识 |
| `cb` | `NULL` | 完成回调 |
| `params` | `NULL` | 回调携带的业务指针 |
| `timeout` | `AT_DEF_TIMEOUT` | 默认 500ms，普通命令每次尝试的等待时间 |
| `retry` | `AT_DEF_RETRY` | 默认 2，普通命令首次发送后的额外尝试次数 |
| `priority` | `AT_PRIORITY_LOW` | 也可选 `AT_PRIORITY_HIGH`，不抢占当前作业 |
| `ctx` | `NULL` | 启用上下文功能时使用 |

普通状态机用子串搜索匹配前缀，再从前缀位置搜索后缀，同时检查 `AT_DEF_RESP_ERR`。空前后缀被视为已满足，通常应保留明确后缀，避免过早判定完成。

超时从实际发送开始计算，不包含排队时间。错误和超时共用普通命令的重试计数，重发前有约 100ms 间隔，实际延迟还取决于轮询周期。

## 响应字段

| 字段 | 用法 |
| --- | --- |
| `code` | `AT_RESP_OK`、`AT_RESP_ERROR`、`AT_RESP_TIMEOUT`、`AT_RESP_ABORT` |
| `obj` | 当前通信对象 |
| `params` | 原请求携带的业务指针 |
| `recvbuf` / `recvcnt` | 接收数据及有效字节数 |
| `prefix` / `suffix` | 成功匹配时的解析位置 |

回调类型为 `void (*)(at_response_t *)`。响应结构是临时对象，接收缓冲区由框架复用，需要保留的数据在回调内复制。二进制数据使用明确长度，不用 `strlen` 判断大小。

`AT_RESP_ABORT` 是状态码，但不能据此推断取消会产生回调；取消结果可以通过绑定的上下文观察，并由应用安排清理。

## 批量命令

`at_send_multiline` 将一组固定命令放在一个作业中，适合可容忍单条失败的连续发送。它不是“所有命令全部成功才返回成功”的事务。

当前批量处理行为如下：

- 只搜索后缀判断每条成功，不按前缀验证。
- 每条超时采用 `AT_DEF_TIMEOUT`，忽略 `attr.timeout`；超时直接结束整组。
- 收到错误后累加计数，达到 `retry` 阈值就跳过该条继续，因此计数方式不同于普通命令的额外重试次数。
- 遍历完成时，只要曾有一条成功，就可能报告 `AT_RESP_OK`。
- 最终回调不汇总各条响应。

严格初始化流程应在上一条成功的回调中提交下一条，并检查新的入队结果；需要把多个阶段放在同一个执行单元时，使用[自定义作业](advanced-usage.md#多阶段交互)。

## 自定义发送与原始数据

`at_custom_cmd` 调用 `sender(env)` 发送一次内容，之后由普通状态机负责等待和重试。每次重试都会再次调用 sender，它引用的数据在请求结束前必须有效。

`at_send_data` 发送的数据不会自动追加换行，但它仍是一条需要匹配响应的请求。如果设备只是无确认地收发原始流，应评估透传或自定义作业，而不是假定该函数为立即完成的串口写操作。

格式化接口和 `env->println` 的结果必须小于 `AT_MAX_CMD_LEN`，为 NUL 保留空间。当前核心没有正确拦截超长 `vsnprintf` 结果，不应将未经长度约束的格式化内容交给它们。

## 数据生命周期

| 数据 | 框架如何保存 | 应用何时可以释放或修改 |
| --- | --- | --- |
| `at_attr_t` 本身 | 入队时按值复制 | 提交返回后 |
| 属性中的 `params`、`prefix`、`suffix`、`ctx` | 只复制指针 | 作业不再访问后 |
| 单行命令、批量命令表和各字符串 | 保存指针 | 作业结束后 |
| 格式化命令的结果 | 保存内容副本 | 原格式化参数在提交返回后可释放 |
| `at_send_data` 载荷 | 保存内容副本 | 提交返回后 |
| 适配器、URC 表、透传配置 | 保存指针 | 对象不再使用后 |

局部属性结构可以安全提交，不代表它指向的局部数据也可以。`at_work_abort_all` 只标记状态，复用上下文或释放异步数据前还需要协调回收。
