# 架构说明

```text
Coding Agent（ZCode / Codex / OpenCode …）
  └── Skill（skills/dslogic/SKILL.md：何时测量/如何配置/如何解读）
        └── CLI：dslab（稳定机器接口，stdout 单 JSON 对象 + stderr 日志）
              ├── devices / capabilities / doctor          # 发现与体检
              ├── capture once                              # 阻塞采集
              ├── capture start/status/stop                 # 异步会话
              │     └── 分离 worker 进程（dslab capture _worker）
              │           └── libsigrok4DSL（vendored，GPL-3.0）
              │                 └── libusb → DSLogic U2Basic
              ├── analyze timing / decode uart|i2c|spi / assert
              │     └── 离线确定性分析（C，作用于 runs/<id>/capture.raw）
              └── export vcd                                 # 独立工具互操作
```

## 分层职责

| 层 | 组件 | 职责 | 失败语义 |
|---|---|---|---|
| 接口 | `dslab` CLI | 参数校验、JSON schema、退出码/错误码规范化 | 用法错 2 / 设备错 3 / 采集错 4 / 断言失败 5 |
| 会话 | `session_mgr` + worker | 分离进程生命周期、state.json 心跳/进度、stop 文件、单活动会话 | worker 死亡由心跳年龄检测（status 报告） |
| 采集 | `capture.c` + vendored libsigrok4DSL | 设备激活、通道/速率/触发配置、datafeed → 文件 | 溢出/断开/超时/不完整显式上报，绝不假成功 |
| 分析 | `runfile/analyze/decode/assert_rules` | 对已保存 run 的确定性重放 | indeterminate 不算 pass；±1 样本量化声明 |
| 互操作 | `export vcd` | 导出标准 VCD 供 DSView 等工具对照 | 源不完整时打 warning |

## 关键设计决策

1. **复用上游而不重写 USB 协议**：采集路径全部通过 libsigrok4DSL 的 `ds_*`
   公共 API（GUI 无关，库内自带采集线程与 g_poll 事件循环），本项目零补丁。
2. **采集数据无损保存**：`capture.raw` 即 USB 收到的 cross 交织原始流，配
   `capture.json`（通道槽映射/速率/样本数）可完整重建任意通道波形；分析层
   从文件重放，保证回归可确定性（同一文件 → 同一结果）。
3. **异步生命周期跨进程**：`capture start` spawn 分离 worker（不继承句柄，
   防止拖住调用方管道），文件 IPC（state.json 原子重写 + stop 文件）。无网络
   服务、无守护进程常驻。
4. **诚实状态机**：`armed` 仅在配置了触发且已确认 running 未收到数据时上报；
   无法确认的状态不会虚构（计划 §4.1 的硬性要求）。
5. **解码器自研（M3 偏差）**：为确定性回归与免 Python 运行时，UART/I2C/SPI
   用 C 实现并用独立 Python 打包器交叉验证（tests/fixtures）。上游
   libsigrokdecode4DSL（Python 嵌入）保留为后续可选后端。
6. **demo 设备仅显式可达**：`--device demo` 才会激活内置虚拟设备，输出永远
   标注 "Demo Device"，杜绝合成数据冒充实机。

## 数据流（capture once/start）

```
ds_start_collect → [collect线程] dev_acquisition_start → FPGA arm(触发字)
   → USB bulk IN → receive_transfer → SR_DF_LOGIC(cross) → datafeed 回调
   → capture.raw (追加) + events.jsonl
   → 满 limit 或 stop/timeout → SR_DF_END → 终态写入 state.json/capture.json
```

分析流：`run_load`（raw+json 校验）→ `dslab_cross_*` 解包 → 边沿/解码/断言 →
stdout JSON（定位信息带 sample 索引与时间）。
