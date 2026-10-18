# nearlink-sle-beacon

星闪 SLE 无连接广播组件。

16 字节状态随广播包下发，对端扫描即得，无需配对与建连。

官方示例 `sle_uart` 以连接为前提，未连接即丢弃数据。本库移除该前提，数据全部经广播承载。

## 特性

- 免配对、免连接
- 广播间隔 12.5 ms
- 载荷去重，刷新下限 200 ms
- 静态分配，无堆内存
- 单任务模型，协议栈调用集中

## 环境

| 项 | 值 |
|---|---|
| 芯片 | WS63 / BearPi-Pico H3863 |
| SDK | WS63 LiteOS SDK |
| 框架 | SLE |
| 许可 | MIT |

## 文件

| 文件 | 职责 |
|---|---|
| `beacon_adv.c` | 广播参数、载荷替换、节流 |
| `beacon_rx.c` | 扫描回调、帧定位、自发过滤 |
| `beacon_uart.c` | 串口组帧、消息队列、任务循环 |

## 接口

| 函数 | 说明 |
|---|---|
| `beacon_register_cbks()` | 注册广播与扫描回调，仅调用一次 |
| `beacon_init()` | 配置参数、装载载荷、启动广播 |
| `beacon_send(buf, len)` | 提交载荷，内容相同时不触发刷新 |
| `beacon_flush()` | 补发被节流的载荷，需周期调用 |
| `beacon_rx_set_handler(cb)` | 设置接收回调 |
| `beacon_uart_start()` | 启动串口任务，串口路径下代替上述流程 |

## 集成

三个源文件更名为 `sle_uart_server_adv.c`、`sle_uart_server.c`、`sle_uart.c`，放入 SDK 示例目录。

```c
static void beacon_entry(void)
{
    (void)beacon_register_cbks();
    beacon_uart_start();
}
app_run(beacon_entry);
```

```bash
python build.py ws63-liteos-app -j8
```

产物位于 `output/ws63/fwpkg/ws63-liteos-app/`。

`SLE_ADV_CHANNEL_MAP_DEFAULT` 未由 SDK 头文件导出，定义见 `beacon_adv.h`。

## 线格式

| 字段 | 值 |
|---|---|
| 发现等级 TLV | `02 01 01` |
| 厂商 TLV | `11 FF` + 16 字节 |
| 帧头 | `AA 55` |
| 载荷长度 | 16 字节 |
| 广播间隔 | `0x64`（125 μs 单位，即 12.5 ms） |

帧头用于接收端定位，不做 TLV 长度遍历。详见 `docs/protocol.md`。

载荷第 0–1 字节为帧头，第 2–15 字节由应用定义。

## 注意事项

- 串口日志按变化打印。逐帧打印占满 115200 带宽，阻塞接收任务。
- 多设备烧录同一固件时，本机地址取芯片 MAC，否则互相过滤。
- 运行中修改载荷需 stop → set → start。
- 两次刷新间隔不小于 200 ms。

## 边界

明文广播，不标记发送方。无确认、无重传、单跳。射频常开。

## 许可

MIT，见 `LICENSE`。

衍生自海思 WS63 SDK `sle_uart` 示例（Apache-2.0）。原版权声明保留于各源文件头及 `NOTICE`。
