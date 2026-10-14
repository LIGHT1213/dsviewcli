# dslab — DSLogic U2Basic 无 GUI 自主采集与分析工具链

让 Coding Agent（ZCode / Codex / OpenCode）在没有 DSView GUI 的情况下，直接对
DreamSourceLab DSLogic **U2Basic** 逻辑分析仪做设备发现、参数配置、真实波形采集
和时序/协议分析。上游驱动能力全部复用官方 DSView 的 `libsigrok4DSL`（GPL-3.0），
本项目只做最薄的 CLI 封装，不重新发明 USB 协议。

> 状态：M0–M5 软件链路全部完成（ctest 5/5）。**实机（U2Basic）验收按用户指示
> 跳过（SKIPPED）**——当前宿主机未连接设备；接入后按
> docs/hardware-test-report.md §实机复测清单 执行即可补齐，所有命令已就绪。

## 快速开始（Windows + MSYS2 UCRT64）

```bash
# 1. 依赖：MSYS2 的 ucrt64 环境（gcc/cmake/ninja/pkg-config/glib2/libusb/zlib）
pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkg-config,glib2,libusb,zlib}

# 2. 构建
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 3. 测试（无需硬件）
ctest --test-dir build --output-on-failure

# 4. 检查环境（设备、固件资源、驱动）——实机使用前必跑
./build/dslab.exe doctor --json
./build/dslab.exe devices --json        # U2Basic 插上后应列出 VID 2A0E

# 5. 阻塞采集：D0 通道 1MHz 采 10ms（保守起步）
./build/dslab.exe capture once --channels D0 --rate 1M --duration 10ms --json

# 6. 异步采集：先布防触发，再执行烧录/复位等外部动作，然后取数
./build/dslab.exe capture start --channels D0,D1 --rate 20M \
       --trigger 'D0:rising' --timeout 10s --json     # → session_id
./build/dslab.exe capture status --session <id> --json
./build/dslab.exe capture stop  --session <id> --json

# 7. 离线分析（对任意已保存的 run）
./build/dslab.exe analyze timing runs/<id> --channel D0 --json
./build/dslab.exe decode i2c runs/<id> --scl D0 --sda D1 --json
./build/dslab.exe assert runs/<id> --spec tests/specs/example-clk.json --json
./build/dslab.exe export vcd runs/<id> --json    # 生成 capture.vcd 供对照
```

Linux/macOS：依赖相同（glib-2.0 / libusb-1.0 / zlib / cmake / ninja），Linux 需
udev 规则（上游 `DSView/DreamSourceLab.rules`）。其他平台未实机验证，标注见
docs/build.md。

Agent 使用手册：`skills/dslogic/SKILL.md`（何时测量/如何触发/如何解读/禁止假
通过）；示例任务：`examples/stm32-i2c-nack-walkthrough.md`。

## 输出文件

每次采集生成 `runs/<id>/`：

| 文件 | 内容 |
|---|---|
| `capture.raw` | 无损原始采样流（DSLogic cross 打包，格式见 docs/cli.md §raw-format） |
| `capture.json` | 采样率、通道映射、样本数、触发、完整性、通道统计 |
| `events.jsonl` | 采集生命周期事件（armed/running/trigger/overflow/end…） |
| `state.json` | （异步会话）状态机、心跳、进度、终态与结果 |
| `capture.vcd` | （按需导出）标准 VCD |

## 里程碑

- **M0** 源码审计与最小构建 ✅（docs/source-audit.md，上游 SHA 见 docs/upstream.md）
- **M1** `devices` / `capabilities` / `doctor` / `capture once` ✅
- **M2** `capture start/status/stop` 分离 worker + 边沿/电平触发 ✅
- **M3** `analyze timing` / `decode uart|i2c|spi` / `assert`（golden 交叉验证）✅
- **M4** Agent Skill（skills/dslogic）+ 示例任务 ✅
- **M5** VCD 导出、架构文档、测试报告 ✅（`.dsl` 导入、CSV、MCP 等可选项未做）
- 实机验收：SKIPPED（用户指示；设备未连接）。复测清单见测试报告。

## 许可证

GPL-3.0-or-later（因为静态编译了 DSView 的 libsigrok4DSL）。详见 docs/licensing.md。

