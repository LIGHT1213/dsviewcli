# CHANGELOG

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
- 实机验证状态：见 docs/hardware-test-report.md（当前宿主机未连接 U2Basic）。
