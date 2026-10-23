# CHANGELOG

## 0.2.1 (2026-10-23) — 实机崩溃修复

- **修复真机 capture 段错误**（用户实测定根因）：libusb ≥1.0.24 Windows 后端
  `libusb_get_pollfds()` 恒 NULL，libsigrok4DSL 不判空即遍历。vendor 补丁：
  判空 + 注册 session fd=-1 空转事件泵 + freewheel 500µs 限速（防 100% CPU）。
  官方 DSView Windows 不受影响（静态链 libusb 1.0.23）；上游 master 同段未判空。
- 新增 `tests/hardware/run_hw_acceptance.sh`：设备插入后一条命令完成
  发现/能力/3 次一致性采集/触发超时/异步会话/离线分析的真机验收。
- docs：upstream.md 登记 vendor 补丁与升级重放要求；source-audit §13 增补
  实证缺陷条目；测试报告增补实测反馈章节。
- 发布链路补齐：Gitea 无 CI，新增 `scripts/publish-gitea-release.sh` 把
  GitHub CI 产物（含 sha256）镜像到 Gitea Release，幂等可重跑；
  v0.2.1 已发布至双远端（Gitea release id=7，zip/7z 与 CI 产物逐字节一致）。
- 修复后真机复测：待设备插入执行验收脚本（软件回归 ctest 5/5 已通过）。

## 0.2.0 (2026-10-14) — M2–M5（实机验收按用户指示跳过）

### M2 — 异步采集与触发
- `capture start/status/stop`：分离 worker 进程（不继承句柄）、文件 IPC
  （state.json 心跳/进度/config/result + stop 文件）、
  created→configuring→starting→armed|capturing→complete|timeout|cancelled|failed
  状态机；armed 仅在触发已布防且无数据时上报。
- 简单触发（rising/falling/change/high/low，多通道 AND）经审计过的
  ds_trigger_* 序列下发；触发超时 → TRIGGER_TIMEOUT；单活动会话强制。
- 修复：worker 句柄继承会拖住调用方管道读取（agent 场景致命）。

### M3 — 离线分析
- `analyze timing`：边沿/频率/周期/脉宽/占空比/抖动（注明样本数）、双通道
  延迟、时间窗检索；±1 样本量化声明。
- `decode uart|i2c|spi`：C 实现的确定性解码器（帧/事务/ACK/NACK/错误标记）。
- `assert`：JSON 规则（9 种），pass/fail/indeterminate，失败退出码 5。
- 自带最小 JSON 解析器；golden fixtures 由独立 Python 打包器交叉验证。

### M4 — Agent Skill
- `skills/dslogic/SKILL.md`：何时测量、体检先行、先布防后触发 MCU、定位坐标
  引用、假通过禁令。
- `examples/stm32-i2c-nack-walkthrough.md` 完整示例任务。

### M5 — 交付项
- `export vcd`：标准 VCD 导出（golden 测试含往返重建）。
- docs/architecture.md；README/测试报告/CHANGELOG 更新。
- 未做（明确）：`.dsl` 导入、CSV 导出、MCP、Linux/macOS 构建、多级触发。

## 0.1.0 (2026-10-14) — M0 + M1

### M0 — 源码审计与最小构建
- 固定上游 DSView @ `2e9e2c8e726df4ef5687d39b83d4f797cc44b574`，vendor 方式引入
  libsigrok4DSL + common（见 docs/upstream.md）。
- 完成 libsigrok4DSL 采集链路源码审计（docs/source-audit.md）：ds_* API、
  扫描/固件/FPGA 流程、LA_CROSS_DATA 数据格式、stream/buffer 约束、平台风险。
- 独立最小 CMake 构建目标（不依赖 Qt/boost/FFTW/Python），Windows MSYS2 UCRT64
  实测通过（docs/build.md）。

### M1 — CLI 最小 PoC
- `dslab devices`：无副作用 libusb 枚举（VID/PID/型号/固件状态）。
- `dslab capabilities`：激活设备后枚举采样率/通道模式/硬件深度。
- `dslab doctor`：USB、资源文件、驱动、库初始化、激活逐项体检。
- `dslab capture once`：阻塞采集，保存 capture.raw（无损 cross 流）+
  capture.json + events.jsonl；采集后解包统计各通道翻转数（通道映射自检）。
- 单元测试（unpack/util）+ CLI JSON 契约测试（无硬件可跑）。

