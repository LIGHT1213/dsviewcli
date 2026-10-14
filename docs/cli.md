# dslab CLI 契约（M1–M3）

所有 `--json` 命令在 **stdout 输出恰好一个 JSON 对象**；诊断日志走 stderr
（libsigrok4DSL 的 xlog 默认也写 stderr）。详细日志可用环境变量调整。

## 退出码

| 码 | 含义 |
|---|---|
| 0 | 成功 |
| 2 | 用法/参数错误 |
| 3 | 设备错误（未连接 / 被占用 / 权限 / 会话不存在） |
| 4 | 采集错误（不完整 / 溢出 / 断开 / 超时） |
| 5 | 断言失败（assert 规则 fail） |

## 规范化错误码（JSON `error.code`）

`NO_DEVICE`、`DEVICE_BUSY`、`PERMISSION_DENIED`、`UNSUPPORTED_CONFIG`、
`ARM_TIMEOUT`、`TRIGGER_TIMEOUT`、`USB_DISCONNECTED`、`CAPTURE_OVERFLOW`、
`INCOMPLETE_CAPTURE`、`DECODE_FAILED`（保留）、`IO_ERROR`、`INTERNAL_ERROR`、
`SESSION_NOT_FOUND`。

参数不静默降级：不支持的速率 / 通道 / 模式 / 触发直接报 `UNSUPPORTED_CONFIG`
并在 message 中列出支持值。

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
| `--device` | auto | 型号子串（如 `U2Basic`）；`demo` 显式指定内置演示设备 |
| `--channels` | D0 | `D0,D1` / `0,1`，升序去重 |
| `--rate` | 1M | 必须命中设备采样率表，否则 UNSUPPORTED_CONFIG |
| `--duration` | 10ms | `100us`/`10ms`/`1.5s` 等 |
| `--mode` | stream | M2 仅 stream |
| `--trigger` | 无 | `'D0:rising,D1:low'`（rising/falling/change/high/low 或 r/f/c/1/0；多通道 AND 组合） |
| `--trigger-position` | 0 | 触发点在缓冲中的百分比 0..100（stream 模式硬件上限 10%） |
| `--timeout` | duration×10+5s | 硬超时 |
| `--output` | runs/<时间戳> | 输出目录 |

行为：
1. 自动选通道模式：满足“最高通道号 + 速率”的最窄 stream 模式
   （16ch≤20MHz / 12ch≤25MHz / 6ch≤50MHz / 3ch≤100MHz），并在结果 JSON 的
   `channel_mode` 字段中显式报告；无法满足时报错并列出支持组合。
2. 采样率先对照设备报告的采样率表校验，再下发并**读回核对**。
3. `limit_samples = ceil(rate × duration)`（驱动内部对齐到 1024 的倍数）。
4. 结果 JSON：`ok`（完整性+无溢出+无错误才为 true）、`state`
   （complete/timeout/cancelled/failed）、`sample_count`、每通道翻转统计
   （通道映射自检）、`warnings[]`、`error`。
5. 触发：无 `--trigger` 时硬件自动触发；有则按审计过的
   reset→set_en→probe_set→set_pos 序列下发；超时且未命中触发报
   `TRIGGER_TIMEOUT`。

## M2：异步采集（跨进程生命周期）

```bash
dslab capture start [--device auto] [--channels D0,D1] [--rate 20M] \
                    [--trigger 'D0:rising'] [--timeout 10s] --json
# → {"session_id":"20261014_...","state":"created",...}（立即返回）
dslab capture status --session <id> --json
dslab capture stop  --session <id> --json
```

- 后台 worker 为分离进程（不继承 CLI 句柄，CLI 退出不影响采集）；文件 IPC：
  `runs/<id>/state.json`（原子重写：state/pid/heartbeat_ms/progress/config/result）、
  `stop` 文件（存在即请求停止）、`worker.log`。
- 状态机：`created → configuring → starting → armed|capturing →
  complete|timeout|cancelled|failed`。**armed 仅在配置了触发且尚未收到数据时
  报告**（不伪造就绪状态）。
- 同一时间只允许一个活动会话（心跳过期或进程已死的残留会话不算）；
  违反报 `DEVICE_BUSY`。未知会话报 `SESSION_NOT_FOUND`（退出码 3）。
- 触发 armed 超时 → 终态 `timeout` + `TRIGGER_TIMEOUT`；用户 stop →
  `cancelled`（保留已收到的部分数据）。

## M3：离线分析（对已保存的 run 确定性执行）

```bash
dslab analyze timing <run-dir> [--channel D0] [--reference D1] \
                     [--window 100us:5ms] --json
dslab decode uart <run-dir> [--channel D0] [--baud 115200] [--bits 8]
                  [--parity none|even|odd] [--stop 1] --json
dslab decode i2c <run-dir> [--scl D0] [--sda D1] --json
dslab decode spi <run-dir> --clk D0 --mosi D1 [--miso D2] --cs D3 \
                 [--mode 0..3] [--order msb|lsb] [--word 8] --json
dslab assert <run-dir> --spec spec.json --json     # 有 fail 时退出码 5
```

- `analyze timing`：边沿计数/前电平、周期（上升沿→上升沿）min/max/mean/
  **抖动（总体标准差，注明样本数）**、高/低脉宽统计、占空比（均值比定义，
  注明周期数）、频率、前 64 条边沿定位（sample 索引+时间）、可选双通道
  延迟（第 k 个上升沿配对）。`--window start:duration` 限定窗口。所有时长
  以整数样本计算，输出注明 ±1 样本量化。
- `decode uart`：起始位定位+位中心采样；帧列表（值/字符/起始样本/时间/
  parity_error/framing_error）；采样率 <2 样本/位时拒解（rate_ok=false）。
- `decode i2c`：START/RESTART/STOP（含样本号）、7 位地址+R/W、每字节
  ACK/NACK、未收到 STOP 的事务标记 warning。
- `decode spi`：CS 低有效分帧、按 mode0-3 采样沿、MSB/LSB、字长 ≤32；
  MOSI/MISO 十六进制、位数非字长整数倍标记 misaligned。
- `assert` 规则（JSON，`{"rules":[...]}`）：
  `timing.period_range`（min_us/max_us）、`timing.freq_range`（min_hz/max_hz）、
  `timing.pulse_high_max`/`timing.pulse_low_max`（max_us）、`timing.duty_range`
  （min/max 0..1）、`timing.channel_delay`（from/to/max_abs_us）、
  `i2c.address_ack`（address/read + scl/sda）、`i2c.no_nack`、
  `uart.frames`（channel/baud/contains hex/min_frames）、`uart.no_errors`。
  判定 pass/fail/**indeterminate**（通道未采集/无翻转/采样率不足/采集不完整
  时不作“通过”结论）；整体 verdict=fail>0?fail:indeterminate>0?indeterminate:pass；
  fail → 退出码 5。捕获不完整时所有 pass 附加 warning 提示不具结论性。
- 解码器为 C 实现（确定性、无 Python 运行时依赖）。与上游
  libsigrokdecode4DSL 的关系：见 docs/source-audit.md §8 与 §M3 说明——
  本项目 M3 优先保证确定性回归；srd 后端作为后续可选集成。

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
