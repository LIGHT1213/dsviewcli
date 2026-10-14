# 测试报告 — M0 + M1（2026-10-14）

> 结论先行：**软件链路全部 PASS；实机（U2Basic）采集 NOT_RUN**——本机
> USB 枚举中不存在任何 DreamSourceLab 设备（VID 2A0E）。在设备接入并按
> 本报告 §硬件清单 复测之前，不宣称实机可用。

## 运行环境

| 项 | 值 |
|---|---|
| OS | Windows 10 (26200) x64 |
| 工具链 | MSYS2 UCRT64：gcc 16.2.0、cmake、ninja、pkg-config |
| 库 | glib-2.0、libusb-1.0（含 Windows 热插拔支持）、zlib |
| 上游 | DSView @ `2e9e2c8e726df4ef5687d39b83d4f797cc44b574` |
| USB | 无 DreamSourceLab 设备连接（PowerShell Get-PnpDevice 全量 VID 核对） |

## 构建（M0 验收项）

- 命令：`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build`
- 结果：**PASS**（46 个编译单元，0 error；上游 libsigrok4DSL + common 全部编入，
  仅存在上游自身的警告，如 lib_main.c 的事件指针转换警告）
- 产物：`build/dslab.exe`（含随包 DLL）+ `build/dslab_tests.exe`
- M0 文档：docs/source-audit.md（含文件:行号证据）、docs/build.md、
  docs/licensing.md、docs/upstream.md、docs/device-setup.md

## 软件测试（全部 PASS，2026-10-14）

| 测试 | 类型 | 结果 | 覆盖 |
|---|---|---|---|
| `unit`（dslab_tests） | unit | **PASS**（6 组用例） | LA_CROSS_DATA 位序/组偏移/通道槽映射/golden 2ch 正交方波、时长/速率/通道解析、整数时间换算 |
| `cli_contract` | replay | **PASS**（11 组断言） | 每命令 stdout 恰好 1 个 JSON、schema 字段、错误码 UNSUPPORTED_CONFIG / NO_DEVICE、退出码 2/3 |
| `demo_pipeline` | replay（合成数据） | **PASS** | 用库内置 demo 设备跑通与实机完全相同的采集管线：事件序列 armed→start→running→df_end→end、capture.raw 字节数=样本数×通道/8、capture.json 元数据、解包统计非零 |

稳定性：无设备 `capture once` 连续 5 次运行输出/退出码一致（曾发现未初始化
`res.printed` 导致的偶发丢输出，已修复并回归）。

## 无设备场景（M1 验收项：找不到设备不崩溃）

| 命令 | stdout | 退出码 |
|---|---|---|
| `devices --json` | `{"ok":true,...,"count":0,"devices":[]}` | 3 |
| `doctor --json` | checks=[usb_enumeration=warn] | 0 |
| `capabilities --json` | `error.code=NO_DEVICE` | 3 |
| `capture once … --json` | `error.code=NO_DEVICE` | 3 |

## 实机测试：**NOT_RUN（U2Basic 未连接）**

阻塞项：本机无 U2Basic。以下清单为设备接入后的复测步骤（对应计划 M1 验收）：

1. `dslab doctor --json` → `usb_enumeration=ok`，资源检查 fpga 位流存在；
   若 `usb_open=fail` 按 docs/device-setup.md 处理驱动（Zadig/WinUSB）。
2. `dslab devices --json` → 确认 VID 2A0E、实际 PID（0x0029/0x0031/0x0035 之一）、
   `firmware_loaded:true`。
3. `dslab capabilities --json` → 记录采样率表、通道模式、hw_depth。
4. 已知信号基准：MCU GPIO 输出 1kHz 方波（3V3，共地），接 D0。
   `dslab capture once --channels D0 --rate 20M --duration 10ms --json`：
   预期 sample_count≈20000（对齐 1024）、D0 transitions≈2×周期数±1、
   channels_detail.D0.first_edge_sample 可定位。
5. 重复 3 次，比对一致性；再用 DSView GUI 采同一信号对照波形周期。
6. 把 runs/ 目录、doctor/devices/capabilities JSON 存档进本报告。

## 已知问题与限制

1. `capture once` 的 DSLogic 专有配置（OPERATION_MODE/CHANNEL_MODE）对 demo
   设备走旁路（`--device demo` 仅显式指定可达，输出永远标注 "Demo Device"）。
2. demo 设备的 raw 流存在非整组尾部（1250B vs 1248B 对齐预期）；实机驱动按
   1024 样本对齐，不受影响；解包对部分组的安全忽略已在单测覆盖。
3. 触发配置（M2 范围）未暴露；M1 记录硬件自动触发包的 remain_cnt。
4. buffer 模式、阈值/VTH 设置、多台设备选择、异步采集均未实现（按计划属 M2+）。
5. `capabilities` 的 `hw_depth_bytes` 等读数尚未与实机交叉验证。
6. Windows 未验证 WinUSB 驱动场景（需实机）；Linux/macOS 未构建（untested）。

## 汇报格式（计划 §8）

```text
实施阶段：M0 + M1（代码与软件测试完成，实机未验证）
上游 commit：2e9e2c8e726df4ef5687d39b83d4f797cc44b574
项目 commit：<见 git log：chore/source-audit、feat/device-capture>
运行平台：Windows 10 26200 x64 / MSYS2 UCRT64 / USB 无 DSL 设备
设备信息：未连接（VID 2A0E 无枚举）→ NOT_RUN
构建：PASS（ctest 3/3：unit、cli_contract、demo_pipeline）
单元测试：PASS（unpack+util 6 组）
回放测试：PASS（demo 管线端到端 + CLI 契约）
实机测试：NOT_RUN（设备未接入；接入后按本文件清单复测）
Skill 端到端：NOT_RUN（M4 范围）
已知问题：见上节
下一阶段：接入 U2Basic 完成 M1 实机验收；然后 M2 异步采集+触发
```
