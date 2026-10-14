# 示例任务：排查 STM32 的 I²C 偶发 NACK

> 本示例展示 Agent 在没有 DSView GUI 的条件下，用 `dslab` 完成一次
> "采集 → 分析 → 修改 → 复测"闭环。文中命令均可复制执行。
> 注意：涉及物理接线的步骤（夹线、共地）Agent 自己无法完成——必须明确
> 请用户操作，不得假设已连接。

## 背景

STM32F4 作为 I²C master 读取传感器（地址 0x50），偶发 NACK 导致读数丢失。
固件仓库中怀疑是时序问题。

## 步骤 1：体检与能力确认（不假设硬件在线）

```bash
dslab doctor --json
dslab devices --json
dslab capabilities --json | head -c 2000
```

- `doctor` 全部 ok（`usb_enumeration=ok`，资源文件齐）。
- `devices`：VID 2A0E PID 0x0031，`firmware_loaded:true`。
- `capabilities`：采样率表包含 20MHz（16 通道模式）。

若 `usb_open=fail`：停止，提示用户安装 WinUSB 驱动（Zadig），不得继续。

## 步骤 2：请用户接线（Agent 不可代劳）

> 请把 U2Basic 的 D0 接 SCL、D1 接 SDA，并连接 U2Basic 的 GND 与
> STM32 板的 GND。（3.3V 电平，共地必需。）

## 步骤 3：布防异步采集，再让用户触发一次读取

读取是偶发的，用触发模式：

```bash
dslab capture start --channels D0,D1 --rate 20M \
       --trigger 'D1:falling' --trigger-position 10 --timeout 30s --json
# 返回 {"session_id":"20261014_...","state":"created",...}
```

`D1:falling` = SDA 下降沿（START 条件的一半；配合 SCL 高电平构成 START，
触发先行近似即可）。布防后请用户执行一次读传感器操作（或烧录触发用
测试固件后复位）。

```bash
dslab capture status --session <id> --json
# state: armed → capturing → complete
```

## 步骤 4：解码定位

```bash
dslab decode i2c runs/<id> --scl D0 --sda D1 --json > /tmp/i2c.json
```

在输出中定位 NACK 的字节：找到 `"ack":"NACK"` 的条目，记下其 `sample`。
再取该事务的 `start_sample`，用窗口复查：

```bash
dslab analyze timing runs/<id> --channel D0 \
       --window <start_us>:<duration_us> --json
```

结论示例（假设）：NACK 发生在地址字节后的第 3 个数据字节；该字节前
SCL 高电平脉宽只有 3.2µs（正常 5µs），且 STM32 的 SDA 提前 1.1µs 拉高
——推断是从机时钟延展（clock stretching）处理不当。

## 步骤 5：修改固件并复测

修改驱动（例如：等待 clock stretching 的超时从 1ms 提到 10ms）后，
**用完全相同的参数**重新采集并解码，然后写断言固化预期：

```json
// tests/specs/i2c-read.json
{"rules": [
  {"id": "addr-ack", "type": "i2c.address_ack", "address": "0x50",
   "scl": "D0", "sda": "D1"},
  {"id": "no-nack", "type": "i2c.no_nack", "scl": "D0", "sda": "D1"}
]}
```

```bash
dslab assert runs/<新id> --spec tests/specs/i2c-read.json --json; echo exit=$?
```

- 退出码 0 且无 fail → 修改有效的证据；
- 仍有 NACK → 回到步骤 4，换窗口继续定位；
- verdict=indeterminate → 说明证据不足（如未采到事务），不得报告修复。

## 报告模板

> 复测完成。基线 runs/A（NACK×3/10 次读取）→ 修改后 runs/B（NACK×0/10），
> assert 通过（rules: addr-ack=pass, no-nack=pass）。采样率 20MS/s，
> ±50ns 量化。两次原始数据在 runs/ 下可复查。

## 边界与诚实声明

- 复位/烧录等外部动作由既有工具完成，本 skill 只负责采集侧；
- 无法触发、波形为空、超时 → 如实报告，不得当作"通过"；
- 若设备未连接（`NO_DEVICE`），报告用户而不是编造数据。
