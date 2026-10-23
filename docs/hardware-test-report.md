# 测试报告 — M0–M5（2026-10-14 第二轮；2026-10-23 实测反馈增补）

> **结论（2026-10-14）**：软件全链路 PASS（ctest 5/5）；实机验收按用户指示 SKIPPED。
>
> **2026-10-23 实测反馈（用户在真机调试中发现，见其 err.md）**：dslab 0.2.0
> 在真实 U2Basic 上**任何 capture 在 "Arm FPGA done" 后一致段错误**——设备与
> 驱动无罪（官方 DSView 同机采集正常）。根因：libusb ≥1.0.24 的 Windows 后端
> 移除了 pollfd 仿真，`libusb_get_pollfds()` 恒返回 NULL，libsigrok4DSL
> `dev_acquisition_start` 不判空直接遍历导致空指针读。官方 DSView Windows 包
> 静态链 libusb 1.0.23 故不受影响；上游 master 同段也未判空。
> **已修复（vendor 补丁，见 docs/upstream.md）**：判空 + fd=-1 空转事件泵 +
> 500µs 限速；软件回归 ctest 5/5 仍绿。用户侧 PoC（假 pollfd 激活空转泵，
> 100ms×2ch 回流 25088 字节真实样本）已证明数据通路完好，与我们的修复方向一致；
> 该 PoC 的第三层 use-after-free 源于对静态数组做 g_free——正式补丁由
> dev_acquisition_start 统一分配/释放，不涉及此问题。
> **修复后真机复测：待设备插入后执行
> `tests/hardware/run_hw_acceptance.sh`（一条命令完成发现/能力/3 次采集/
> 触发超时/异步会话/离线分析，产物落 tests/hardware/results/）。**

## 运行环境

| 项 | 值 |
|---|---|
| OS | Windows 10 (26200) x64 |
| 工具链 | MSYS2 UCRT64：gcc 16.2.0、cmake、ninja、pkg-config（经代理安装） |
| 库 | glib-2.0、libusb-1.0、zlib |
| 上游 | DSView @ `2e9e2c8e726df4ef5687d39b83d4f797cc44b574` |
| USB | 无 DreamSourceLab 设备（Get-PnpDevice 全量核对，无 VID_2A0E） |

## 里程碑与提交

| 阶段 | commit | 内容 | 软件 | 实机 |
|---|---|---|---|---|
| M0 | `bab1a9e` | 源码审计（docs/source-audit.md，file:line 证据）、vendor、构建方案 | PASS | n/a |
| M1 | `676fc58` | devices/capabilities/doctor/capture once、无损 raw+元数据 | PASS | SKIPPED |
| M2 | `001d9ec` | 分离 worker 会话、start/status/stop、边沿/电平触发、超时/取消/并发拒绝 | PASS | SKIPPED |
| M3 | `fc6c2ca` | analyze timing、UART/I2C/SPI 解码、JSON 断言、golden fixtures | PASS | n/a（纯离线） |
| M4 | `251d4e4` | skills/dslogic/SKILL.md + I2C NACK 排查示例 | PASS（文档型） | SKIPPED |
| M5 | （本次提交） | VCD 导出、architecture/README/CHANGELOG、最终回归 | PASS | SKIPPED |

## 构建

`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build`
→ 0 error；干净重建复现通过；`build/dslab.exe` 自带运行时 DLL，可脱离
MSYS2 PATH 运行。

## 测试明细（ctest 5/5，2026-10-14）

| 测试 | 类型 | 结果 | 覆盖 |
|---|---|---|---|
| `unit` | unit | **PASS** | cross 位序/组偏移/槽映射/golden 正交方波；时长/速率/通道/触发解析；整数时间换算 |
| `cli_contract` | replay | **PASS** | 单 JSON stdout、错误码（UNSUPPORTED_CONFIG/NO_DEVICE/SESSION_NOT_FOUND）、退出码 2/3 |
| `demo_pipeline` | replay（合成） | **PASS** | 真实采集管线端到端（demo 设备，显式指定、明确标注） |
| `async_lifecycle` | replay（合成） | **PASS** | start→armed/capturing→complete 事件序；长采集中途 stop→cancelled+部分数据；并发 start→DEVICE_BUSY；未知会话→SESSION_NOT_FOUND；worker 不拖住调用方管道（句柄继承 bug 已修） |
| `analysis_golden` | replay（独立实现交叉验证） | **PASS** | 2kHz 方波（199/200 边沿、±100 样本通道延迟）、抖动方波（480/520 周期范围）、UART 8N1 含坏停止位（5 帧全解+1 错误标记）、I²C 3 事务（地址 ACK/NACK/地址 NACK）、SPI mode0 2 事务（MOSI/MISO 全对）、assert pass/fail/indeterminate 与退出码 5、VCD 导出往返重建电平 |

golden 的关键性质：fixtures 由 **Python 独立实现**的 cross 打包器生成，C 解包/
分析/解码的通过意味着两套实现相互印证（非自我对拍）。

## 实机复测清单（设备接入后执行；当前 SKIPPED per user）

1. `dslab doctor --json`：usb_enumeration=ok、资源检查、驱动（fail 则按
   docs/device-setup.md 处理）。
2. `dslab devices --json`：VID 2A0E + 实际 PID（0x0029/0x0031/0x0035）+
   firmware_loaded:true。
3. `dslab capabilities --json`：记录采样率表/通道模式/hw_depth。
4. 已知基准：1kHz 方波（3V3 共地）接 D0，`capture once --rate 20M
   --duration 10ms` ×3：预期 sample_count≈20000（1024 对齐）、周期 1000µs
   ±1 样本、`channels_detail.D0` 翻转数≈20±1；与 DSView GUI 同信号对照。
5. 触发：`capture start --trigger 'D0:rising'` + 手动产生边沿；无触发超时
   → TRIGGER_TIMEOUT 清理验证；采集中途拔 USB → failed/USB_DISCONNECTED。
6. `decode`/`assert` 跑一遍已知 UART/I²C 帧（例如 USB-TTL 适配器发
   "U"）与 §6 的已知内容比对。
7. 全部 runs/ 与 JSON 存档回本报告。

## 已知问题与限制

1. 实机整体未验证（SKIPPED）：含驱动占用（DEVICE_BUSY 的真实路径）、
   FPGA 位流加载、触发字硬件行为、溢出计数。
2. 触发仅单级简单触发（边沿/电平 AND）；多级/串行触发未实现（上游
   ADV/SERIAL 模式未接）。
3. buffer 模式未开放；阈值/VTH、RLE 未暴露。
4. demo 设备时序与真实硬件不同（满速生成），仅用于软件管线回归。
5. M3 解码器为自研 C 实现（确定性优先），与 libsigrokdecode4DSL 的输出
   交叉验证待实机样本（计划项）。
6. Linux/macOS 未构建（untested）；Windows 仅本机一种驱动环境验证过
   libusb 枚举（无设备时）。
7. `.dsl` 导入、CSV 导出未实现（M5 可选项，未做）。

## 汇报格式（计划 §8）

```text
实施阶段：M0+M1+M2+M3+M4+M5（软件完成）
上游 commit：2e9e2c8e726df4ef5687d39b83d4f797cc44b574
项目 commit：bab1a9e → 676fc58 → 001d9ec → fc6c2ca → 251d4e4 → <M5 最终提交>
运行平台：Windows 10 26200 x64 / MSYS2 UCRT64
设备信息：U2Basic 未连接；实机验收按用户指示 SKIPPED
构建：PASS（干净重建 + ctest 5/5）
单元测试：PASS（unpack/util/trigger）
回放测试：PASS（契约 / demo 管线 / 异步生命周期 / golden 交叉验证 / VCD 往返）
实机测试：SKIPPED（per user；接入后按上方清单复测）
Skill 端到端：文档+示例完成；真实 Agent 会话演练 NOT_RUN（需设备）
已知问题：见上节
下一阶段：接入 U2Basic 后补实机验收（清单已备）
```
