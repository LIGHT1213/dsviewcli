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

## 本地补丁（vendor 相对上游 @2e9e2c8e 的差异）

升级 vendor 时**必须重放/复核**以下补丁：

| 文件 | 内容 | 原因 |
|---|---|---|
| `libsigrok4DSL/hardware/DSL/dslogic.c` `dev_acquisition_start` | `libusb_get_pollfds()` 判空：返回 NULL（libusb ≥1.0.24 的 Windows 后端不再提供 pollfd）时注册 session 的 fd=-1 空转源泵事件，而不是解引用 NULL | 实机崩溃修复（2026-10-23 用户实测报告：任何 capture 在 "Arm FPGA done" 后段错误；官方 DSView Windows 因静态链 libusb 1.0.23 未受影响。上游 master 同段亦未判空，值得报 issue） |
| `libsigrok4DSL/hardware/DSL/dslogic.c` `receive_data` | fd<0（空转泵）时 `g_usleep(500)`，避免 freewheel 紧循环 100% 占满 CPU | 同上补丁的配套限速 |

补丁均以 `[dslab patch]` 注释标记，可用
`diff -u upstream/DSView/libsigrok4DSL/hardware/DSL/dslogic.c vendor/DSView/libsigrok4DSL/hardware/DSL/dslogic.c`
核对。
