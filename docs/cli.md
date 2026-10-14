# dslab CLI 契约（M1）

所有 `--json` 命令在 **stdout 输出恰好一个 JSON 对象**；诊断日志走 stderr
（libsigrok4DSL 的 xlog 默认也写 stderr）。详细日志可用环境变量调整。

## 退出码

| 码 | 含义 |
|---|---|
| 0 | 成功 |
| 2 | 用法/参数错误 |
| 3 | 设备错误（未连接 / 被占用 / 权限） |
| 4 | 采集错误（不完整 / 溢出 / 断开 / 超时） |
| 5 | 断言失败（M3 引入） |

## 规范化错误码（JSON `error.code`）

`NO_DEVICE`、`DEVICE_BUSY`、`PERMISSION_DENIED`、`UNSUPPORTED_CONFIG`、
`ARM_TIMEOUT`、`TRIGGER_TIMEOUT`（M2）、`USB_DISCONNECTED`、`CAPTURE_OVERFLOW`、
`INCOMPLETE_CAPTURE`、`DECODE_FAILED`（M3）、`IO_ERROR`、`INTERNAL_ERROR`。

参数不静默降级：不支持的速率 / 通道 / 模式直接报 `UNSUPPORTED_CONFIG` 并在
message 中列出支持值。

## 命令

### `dslab devices [--json]`
纯 libusb 枚举（无副作用，不上传固件）。输出每设备的 bus/address、VID（2A0E）、
PID、速度、型号（对照 dsl.h profile 表）、字符串描述符、`firmware_loaded`
（Manufacturer=="DreamSourceLab" 且 Product=="USB-based DSL Instrument v2"）。
无设备时 `count:0`，退出码 3。

### `dslab capabilities [--device auto|<model子串>] --json`
激活设备（等效 DSView 里选中设备：必要时加载 FPGA 位流）后查询：
`vld_ch_num`、`total_ch_num`、`hw_depth_bytes`、当前 `channel_mode`、stream
标志、驱动认可的 `samplerates[]` 全表、当前模式组可选 `channel_modes[]`。

### `dslab doctor --json`
逐项检查（`checks[].{id,status,detail}`，status ∈ ok/warn/fail）：
USB 枚举、每台已连接设备的固件/FPGA 资源文件、libusb 打开错误（占用/缺驱动）、
`ds_lib_init`、设备激活。任何 fail → 退出码 3。**没有硬件时报告 warn 而非 fail**
（无硬件是环境事实，不是工具故障）。

### `dslab capture once [选项] --json`
阻塞式单次采集。

| 选项 | 默认 | 说明 |
|---|---|---|
| `--device` | auto | 型号子串（如 `U2Basic`） |
| `--channels` | D0 | `D0,D1` / `0,1`，升序去重 |
| `--rate` | 1M | 必须命中设备采样率表，否则 UNSUPPORTED_CONFIG |
| `--duration` | 10ms | `100us`/`10ms`/`1.5s` 等 |
| `--mode` | stream | M1 仅 stream |
| `--timeout` | duration×10+5s | 硬超时 |
| `--output` | runs/<时间戳> | 输出目录 |

行为：
1. 自动选通道模式：满足“最高通道号 + 速率”的最窄 stream 模式
   （16ch≤20MHz / 12ch≤25MHz / 6ch≤50MHz / 3ch≤100MHz），并在结果 JSON 的
   `channel_mode` 字段中显式报告；无法满足时报错并列出支持组合。
2. 采样率先对照设备报告的采样率表校验，再下发并**读回核对**。
3. `limit_samples = ceil(rate × duration)`（驱动内部对齐到 1024 的倍数）。
4. 结果 JSON：`ok`（完整性+无溢出+无错误才为 true）、`sample_count`、
   `byte_count`、每通道 `transitions/rising/falling/first_edge_sample`
   （由 capture.raw 解包统计而来，本身就是通道映射正确性的自检）、
   `warnings[]`（含 `INCOMPLETE_CAPTURE`、`CAPTURE_OVERFLOW`、
   静态波形提示）、`error`。
5. 触发：M1 未开放用户触发；硬件自动触发包（SR_DF_TRIGGER/remain_cnt）记录进
   `capture.json.trigger`。

## raw-format（capture.raw，格式版本 dslogic-cross-v1）

来自 USB 的无损原始流（`SR_DF_LOGIC`/`LA_CROSS_DATA`，见 docs/source-audit.md §6）：

- N = 使能通道数（capture.json `sample_format.channel_slots`）
- 原子组 = N×8 字节：第 g 组第 c 槽 = 字节 `[(g·N+c)·8, +8)`
- 槽内 64 个样本：样本 n = `byte[n/8]` 的 bit(n%8)，LSB-first
- 槽序 = 使能通道按通道号升序（capture.json `enabled_channels[]` 与之一一对应）
- 样本序号 s（全局）的时间 = `s / sample_rate_hz`（整数运算，勿用浮点累加）

## events.jsonl 事件

`armed_config`（完整配置快照）、`collect_start`、`device_running`、`df_header`、
`df_trigger`（含 remain_cnt）、`df_overflow`、`df_end`、`device_stopped`、
`collect_end`、`collect_end_by_error`、`collect_end_by_detached`、`timeout_hit`、
`raw_write_error`。
