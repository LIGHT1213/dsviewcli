# dslab — DSLogic U2Basic 无 GUI 自主采集与分析工具链

让 Coding Agent（ZCode / Codex / OpenCode）在没有 DSView GUI 的情况下，直接对
DreamSourceLab DSLogic **U2Basic** 逻辑分析仪做设备发现、参数配置、真实波形采集
和时序分析。上游驱动能力全部复用官方 DSView 的 `libsigrok4DSL`（GPL-3.0），
本项目只做最薄的 CLI 封装，不重新发明 USB 协议。

> 状态：M0（源码审计+最小构建）与 M1（devices / capabilities / doctor / capture once）
> 已实现；实机验证状态见 docs/hardware-test-report.md —— **在 U2Basic 接入并跑通
> 真实采集之前，本仓库不宣称"实机可用"**。

## 快速开始（Windows + MSYS2 UCRT64）

```bash
# 1. 依赖：MSYS2 的 ucrt64 环境（gcc/cmake/ninja/pkg-config/glib2/libusb/zlib）
pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkg-config,glib2,libusb,zlib}

# 2. 构建
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 3. 检查环境（设备、固件资源、权限）
./build/dslab.exe doctor --json

# 4. 枚举设备（无需管理员；U2Basic 插上 USB 后应列出 VID 2A0E）
./build/dslab.exe devices --json

# 5. 最小采集：D0 通道 1MHz 采 10ms（先保守配置）
./build/dslab.exe capture once --channels D0 --rate 1M --duration 10ms --json

# 6. 单元测试 + CLI 契约测试（无需硬件）
ctest --test-dir build --output-on-failure
```

Linux/macOS：依赖相同（glib-2.0 / libusb-1.0 / zlib / cmake / ninja），Linux 需
udev 规则（上游 `DSView/DreamSourceLab.rules`）。其他平台未实机验证，标注见
docs/build.md。

## 输出文件

每次采集生成 `runs/<run-id>/`：

| 文件 | 内容 |
|---|---|
| `capture.raw` | 无损原始采样流（DSLogic cross 打包，格式见 docs/cli.md §raw-format） |
| `capture.json` | 采样率、通道映射、样本数、触发、完整性、通道统计 |
| `events.jsonl` | 采集生命周期事件（armed/running/trigger/overflow/end…） |

## 里程碑

- **M0** 源码审计与最小构建 ✅（docs/source-audit.md，上游 SHA 见 docs/upstream.md）
- **M1** `devices` / `capabilities` / `doctor` / `capture once` ✅（代码完成，实机状态见报告）
- M2 异步采集 `capture start/status/stop` + 触发（计划中）
- M3 时序分析 / UART / I²C / SPI 解码与断言（计划中）
- M4 Agent Skill（计划中）
- M5 健壮性与交付（计划中）

## 许可证

GPL-3.0-or-later（因为静态编译了 DSView 的 libsigrok4DSL）。详见 docs/licensing.md。
