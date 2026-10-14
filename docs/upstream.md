# 上游来源记录

| 项 | 值 |
|---|---|
| 仓库 | https://github.com/DreamSourceLab/DSView |
| 分支 | master |
| Commit SHA | `2e9e2c8e726df4ef5687d39b83d4f797cc44b574` |
| 提交时间 | 2024-11-05 10:43:10 +0800 |
| 提交标题 | print the warning that the decoder direcotry not exists. |
| 克隆日期 | 2026-10-14（经代理 http://192.168.31.165:7890） |

## 引用方式（vendor）

本仓库采用 **vendor（受控拷贝）** 方案，而非 submodule / fork：

- `vendor/DSView/libsigrok4DSL` — 上游 `libsigrok4DSL/` 全量拷贝（GPL-3.0，含 COPYING），仅删除其自带 tests 目录。
- `vendor/DSView/common` — 上游 `common/`（minizip + xlog），libsigrok4DSL 的 `log.h` 依赖 `<log/xlog.h>`、`session_driver.c` 依赖 `<minizip/unzip.h>`。
- `vendor/DSView/res` — 设备运行时资源（FX2 固件 `*.fw` 与 FPGA 位流 `*.bin`），拷贝自上游 `DSView/res/`。

选 vendor 而非 submodule 的理由：
1. 离线可复现构建（submodule 需要网络与 GitHub 可达性）。
2. 上游无独立 CLI 构建目标（顶层 CMakeLists 把库源码直接编进 Qt 应用，见 `CMakeLists.txt:683-692`），我们需要一个独立的最小编译清单，vendor 后可以自由维护该清单而不动上游目录。
3. 本项目以 GPL-3.0 衍生作品发布（见 docs/licensing.md），允许完整携带上游源码。

本地开发时可保留完整克隆在 `upstream/DSView`（已 .gitignore），用于比对与升级；升级时同步更新本文件与 `vendor/` 并重新审计。
