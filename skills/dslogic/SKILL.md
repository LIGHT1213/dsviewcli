---
name: dslogic
description: Use the dslab CLI to capture and analyze real hardware signals with a DSLogic U2Basic logic analyzer when debugging MCU firmware timing or protocols (UART/SPI/I2C), instead of guessing. Teaches when to measure, how to configure captures safely, how to trigger around MCU resets/flash events, and how to interpret results without false positives.
---

# DSLogic 逻辑分析仪采集（dslab）

适用场景：MCU 固件调试中出现**时序/协议/触发类**问题时（GPIO 脉宽不符、
UART 乱码、SPI 片选错位、I²C 偶发 NACK、复位后时序、中断延迟、看门狗复位
原因），用真实波形证据代替猜测。纯逻辑问题（编译错误、算法 bug）不要用本工具。

## 0. 前置事实（必须先确认，不得假设）

1. 逻辑分析仪必须与被测板**共地**；只测 ≤5V 数字信号（3V3 档默认）。
   禁止接市电/高压/未知电平。
2. 宿主机上要有 `dslab` 可执行文件（含随包 DLL），U2Basic 已插好 USB。
3. 首次使用先跑体检，任何 fail 都要先解决再采集：

```bash
dslab doctor --json
dslab devices --json          # 确认 VID 2A0E、实际 PID、firmware_loaded:true
dslab capabilities --json     # 记录采样率表/通道模式，后续参数以此为准
```

`doctor` 报 `usb_open=fail` 通常是驱动问题（见项目 docs/device-setup.md，
Windows 用 Zadig 装 WinUSB——这是系统级改动，需要用户知情）。

## 1. 何时采集（避免滥用）

**应该采集**：现象随时间/相位变化（"偶发"、"大约每秒一次"、"复位后短时间
正常"）；怀疑信号本身（电平/边沿/时序/总线状态）；需要对固件改动做前后对比。
**不该采集**：代码逻辑可静态推导；无物理连线线索；用户明确说没接硬件。
瞬态/偶发问题 → 用触发 + 后台采集（§3）；稳定复现的问题 → `capture once`
最快。

## 2. 稳定信号：阻塞式采集

```bash
# 保守起步：先低速短时确认链路，再提高参数
dslab capture once --channels D0 --rate 1M --duration 10ms --json
# 两路总线（如 I2C）：SCL=D0, SDA=D1
dslab capture once --channels D0,D1 --rate 20M --duration 50ms --json
```

- 通道模式自动选择并在输出 `channel_mode` 中报告（16ch≤20M/12ch≤25M/
  6ch≤50M/3ch≤100M）。要求超出会明确报错，不会静默降级。
- 采样率必须命中 `capabilities` 的表；建议 ≥ 10× 被测信号频率，协议解码
  ≥ 4 样本/位。
- 输出落盘 `runs/<id>/`：`capture.raw`（无损原始流）+ `capture.json` +
  `events.jsonl`。**每次结论都要引用 run 目录路径作为证据。**

## 3. 瞬态问题：先 arm，再触发 MCU 行为

采集会话独立于本 CLI 进程存在——启动后你可以去编译/烧录/复位 MCU，再回来取数：

```bash
dslab capture start --channels D0,D1 --rate 20M \
       --trigger 'D0:rising' --trigger-position 10 --timeout 10s --json
# ← 记下 session_id；此时执行你的烧录/复位/GPIO 测试命令
dslab capture status --session <id> --json     # 轮询；armed=已布防等待触发
dslab capture stop  --session <id> --json      # 需要放弃时（保留部分数据）
```

- `armed` 只在底层确认触发已布防且尚无数据时返回；`capturing`=数据已在流入。
- 触词语法：`'D0:rising'`、`'D1:falling'`、`'D0:high,D1:low'`（AND 组合）。
- 触发位置 0..100（stream 模式硬件上限 10% 预触发）。
- `timeout` + 未命中触发 → `TRIGGER_TIMEOUT`；这是"没等到事件"，**不是**
  测试通过也不是失败，应检查接线/触发条件后重试。
- 同一设备同时只能有一个会话；DSView GUI 打开设备时会 `DEVICE_BUSY`。

## 4. 分析：先缩小范围，再完整解码

```bash
dslab analyze timing runs/<id> --channel D0 --json          # 边沿/频率/脉宽/占空比/抖动
dslab analyze timing runs/<id> --channel D1 --reference D0 --json   # 通道间延迟
dslab decode i2c  runs/<id> --scl D0 --sda D1 --json
dslab decode uart runs/<id> --channel D0 --baud 115200 --json
dslab decode spi  runs/<id> --clk D0 --mosi D1 --miso D2 --cs D3 --mode 0 --json
```

定位异常时**必须给出可复现坐标**：通道 + 样本序号或时间窗（输出里的
`sample`/`time` 字段），可用 `--window 100us:5ms` 收窄复查。

## 5. 断言与回归（修改固件前后对比）

```bash
# tests/specs/clk.json: {"rules":[{"id":"clk","type":"timing.period_range",
#   "channel":"D0","min_us":990,"max_us":1010}]}
dslab assert runs/<id> --spec tests/specs/clk.json --json   # fail 时退出码 5
```

固件修改流程：采集(基线) → 分析定位 → 改代码 → **同参数再采集** →
`assert` 或对比两次 `analyze timing` 的数值。结论必须引用两次的 run 目录。

## 6. 判读纪律（禁止假通过）

以下情况**一律不得报告"测试通过"**，如实说明并重试或上报：
- `ok:false`、`error.code` 非空（INCOMPLETE_CAPTURE/CAPTURE_OVERFLOW/
  USB_DISCONNECTED/TRIGGER_TIMEOUT…）
- 波形全静态（warnings 含 static）→ 先查接线/共地
- `decode` 的 rate_ok=false、framing/parity error、i2c NACK、spi misaligned
  → 是被测信号异常的证据，不是解码器 bug（除非有理由怀疑工具）
- `assert` verdict=indeterminate → 证据不足，不得当作 pass
- 时长精度受 ±1 采样周期限制；采样率不足时改用更高 rate/更少通道重采，
  不要下"绝对精确"结论。

## 7. 安全与资源

- 不要无限时长采集；从短时长起步；超长采集会占满磁盘（runs/ 下大文件）。
- 重复采集前先 `capture stop` 旧会话。
- 不修改设备 EEPROM/固件存储；`dslab` 只做运行时 FPGA 加载（与 DSView 一致）。
- 模拟量测量不在能力范围内。

## 8. 输出规范

向用户报告测量结论时附带：命令行、run 目录、关键 JSON 字段
（sample_rate_hz/sample_count/定位到的 sample 或时间）、与预期的偏差量
（以采样周期为单位）。示例：

> 在 runs/20261014_071631 中（20MS/s，D0），第 3 个 CS 帧的最后一个 SCLK
> 上升沿后 MOSI 保持高 320ns（样本 15432–15438），超出半周期 250ns，
> 判断为从机未及时释放总线；建议下次复测 `--rate 50M --channels D0,D1`。

## 已知边界

- M2 触发为单级简单触发（边沿/电平 AND 组合）；多级序列触发未实现。
- buffer 模式未开放（`--mode stream` only）。
- 实机硬件验证状态见仓库 docs/hardware-test-report.md；若该报告标注硬件
  SKIPPED/NOT_RUN，首次实机使用时先跑 §0 的体检并把结果带给用户确认。
