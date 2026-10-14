# 许可证说明

## 本项目（dsviewcli / dslab）

**GPL-3.0-or-later**。理由：本项目将 DSView 上游的 `libsigrok4DSL`、`common`（minizip、xlog）以源码形式编译链接进 `dslab` 可执行文件（静态/动态链接均构成衍生作品），上游对应目录的许可证为 GPL-3.0（`vendor/DSView/libsigrok4DSL/COPYING` 等）。因此整个分发单元（`dslab` 可执行文件及其源码树）必须以 GPL-3.0-or-later 发布，**不得**标注 MIT/Apache 等宽松许可。

## 代码组成与义务

| 组成 | 来源 | 许可证 | 再分发义务 |
|---|---|---|---|
| `vendor/DSView/libsigrok4DSL` | DSView @ 2e9e2c8e | GPL-3.0 | 保留版权与许可证文本；衍生作品同许可证开放源码 |
| `vendor/DSView/common/minizip` | DSView 内置 minizip（原始作者 Gilles Vollant） | zlib 许可 | 保留版权声明（源文件头已含） |
| `vendor/DSView/common/log` (xlog) | DSView @ 2e9e2c8e | GPL-3.0 | 同 GPL-3.0 |
| `vendor/DSView/res/*.fw,*.bin` | DSView 官方固件/位流二进制资源 | 随 DSView 分发（GPL-3.0 软件包内资源） | 仅随本工具分发使用，不修改 |
| `src/`（本项目原创 CLI） | 本项目 | GPL-3.0-or-later | 同上 |

- 完整许可证文本：根目录 `LICENSE`（GPL-3.0 全文，取自上游 COPYING）。
- 若未来引入 libsigrokdecode4DSL（GPL-3.0）或其 Python 解码器（各解码器文件头许可，多为 GPL-2+），需在此表补充并核对兼容性。
- 引用第三方设计参考（如 DatanoiseTV/logicanalyzer-mcp）时只允许借鉴接口思路，不得复制代码，除非其许可证允许且在此登记。

## 商标与设备资源

"DSView"、"DreamSourceLab"、"DSLogic" 为 DreamSourceLab 商标/产品名，本项目仅为独立构建的互操作工具，非官方产品。
