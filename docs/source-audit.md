# M0 源码审计报告 — DSView / libsigrok4DSL（U2Basic 采集链路）

> 上游：DreamSourceLab/DSView @ `2e9e2c8e726df4ef5687d39b83d4f797cc44b574`（master, 2024-11-05）
> 所有 `文件:行号` 相对上游仓库根目录。结论分两级：**[源码确认]**（读到并核对过代码）与 **[待实机验证]**（依赖真实硬件行为，尚未验证）。

## 1. 总体结论

**libsigrok4DSL 自带完整的、GUI 无关的采集 API（`ds_*` 前缀）**，包括：设备扫描/热插拔线程、采集线程、session 事件循环（`g_poll`）、datafeed 回调与触发处理。DSView 的 Qt 界面只是这套 API 的一个消费者。因此本项目 CLI **无需修改上游任何代码**，直接链接 libsigrok4DSL 源码并按 DSView 相同的调用序列驱动即可实现无 GUI 采集。

关键证据：
- `libsigrok4DSL/lib_main.c:115` `ds_lib_init()`：初始化 libusb context、注册全部驱动、启动 USB 热插拔线程（`lib_main.c:177`）、首次全量扫描（`lib_main.c:172`）。
- `lib_main.c:721` `ds_start_collect()`：创建采集线程 `collect_run_proc`（`lib_main.c:776`），线程内依次调用 `dev_acquisition_start`（`lib_main.c:798`）与 `sr_session_run()`（`lib_main.c:810`），结束时回调 `DS_EV_COLLECT_TASK_END`（`lib_main.c:830`）。
- `libsigrok4DSL/session.c:193` `sr_session_run()`：用 `g_poll` 驱动 USB pollfd（`session.c:154`），**事件循环在库内部**，不依赖任何 GUI 框架。

## 2. 设备识别与 U2Basic 配置 [源码确认]

- 厂商 VID：`libsigrok-internal.h:58` `#define DS_VENDOR_ID 0x2A0E`。
- U2Basic 设备表项（`libsigrok4DSL/hardware/DSL/dsl.h`）：

| PID | 行号 | 固件(fw) | FPGA 位流 | 特性位 |
|---|---|---|---|---|
| `0x0029` | dsl.h:562 | DSLogicU2Basic.fw | DSLogicU2Basic.bin | VTH+BUF |
| `0x0031` | dsl.h:751 | DSLogicU2Basic.fw | DSLogicU2Basic-pgl12.bin | VTH+BUF+MAX25_VTH+SECURITY |
| `0x0035` | dsl.h:805 | DSLogicU2Basic-pgl12-2.fw | DSLogicU2Basic-pgl12-2.bin | 同上 |

- 通道模式（`dsl.h:369-441` `channel_modes[]`，U2Basic 支持位图见 dsl.h:568/757/811）：
  - `DSL_STREAM20x16`（16ch，stream，≤20MHz）
  - `DSL_STREAM25x12`（12ch，stream，≤25MHz）
  - `DSL_STREAM50x6`（6ch，stream，≤50MHz）
  - `DSL_STREAM100x3`（3ch，stream，≤100MHz）
  - `DSL_BUFFER100x16`（16ch，buffer，≤100MHz，板载缓冲）
- 采样率表 `samplerates100`（dsl.h:201-229）：10Hz…100MHz 共 27 档；U2Basic 的 `hw_depth = SR_MB(64)`（按 dsl.h:571 处字段序，对应 `dsl_channel_depth()` 的每通道深度计算 dsl.c:435）。
- USB 均要求 High-Speed（dsl.h:562 等处 `LIBUSB_SPEED_HIGH`）；scan 时低速设备直接跳过并上报事件 `DS_EV_DEVICE_SPEED_NOT_MATCH`（dslogic.c:352-355, 477-479）。

## 3. 枚举 / 打开 / 固件加载 / FPGA 初始化 [源码确认]

`scan()`（`libsigrok4DSL/hardware/DSL/dslogic.c:264`）流程：
1. `libusb_get_device_list`；逐设备过滤 `idVendor != 0x2A0E` 即跳过（dslogic.c:348）——**裸 Cypress FX2（VID 04b4）根本不会被枚举**，说明 DSLogic 家族设备上电即以 0x2A0E+正确 PID 出现（固件预载）。
2. 速度必须 HIGH/SUPER（dslogic.c:352），再按 (vid,pid,speed) 匹配 `supported_DSLogic[]`（dslogic.c:360）。
3. `dsl_check_conf_profile()`（dsl.c:245）：读字符串描述符，要求 Manufacturer==`"DreamSourceLab"`（dsl.c:279）且 Product==`"USB-based DSL Instrument v2"`（dsl.c:289）。**打开失败时返回 TRUE 并当作已配置处理**（dsl.c:265-270，注释"Mybe the device is busy"）——被占用设备会被列入设备表，激活时才报 `SR_ERR_DEVICE_IS_EXCLUSIVE`（dsl.c:339-343）。
4. 未匹配字符串描述符 → `ezusb_upload_firmware()`（dslogic.c:454，实现在 `hardware/common/ezusb.c`）上传 `DS_RES_PATH/<profile.firmware>`，随后等待设备重枚举（热插拔线程 500ms 重连窗口，lib_main.c:38, 1471-1480）。

`dsl_dev_open()`（dsl.c:1821）流程：
1. `hw_dev_open`（dsl.c:303）：`libusb_open` → 控制传输 `DSL_CTL_FW_VERSION` 读固件版本（dsl.c:355-359）；major != 2 → `SR_ST_INCOMPATIBLE` + `SR_ERR_DEVICE_FIRMWARE_VERSION_LOW`（dsl.c:373-379）。
2. `libusb_claim_interface(0)`（dsl.c:1847）；BUSY/NO_DEVICE/NOT_FOUND 有重试与明确错误码（dsl.c:1849-1881）。
3. 读 `DSL_CTL_HW_STATUS`（dsl.c:1883-1894）得 `bmFPGA_DONE`：
   - 未配置 → `dsl_fpga_config()`（dsl.c:1300）上传 `DS_RES_PATH/<fpga_bit33|50>`（阈值档位对应 3V3/5.0V 两套位流，dslogic.c:1060-1099）；流程为 PROG_B 复位 → bulk EP 0x02 传位流（dsl.c:1422）→ 等 `bmGPIF_DONE`/`bmFPGA_DONE`（dsl.c:1452-1478）。
   - 已配置 → 读 HDL 版本寄存器（`dsl_hdl_version` dsl.c:442，`HDL_VERSION_ADDR=0x04`，command.h:59），必须 == `DSL_HDL_VERSION 0x0E`（dsl.h:47），否则 INCOMPATIBLE（dsl.c:1942-1948）。
4. 带 `CAPS_FEATURE_SECURITY` 的 PID（0x0031/0x0035）还会做 EEPROM 挑战-响应校验（dsl.c:1952-1969, `dsl_secuCheck`）。

**固件资源重要发现**：上游仓库 `DSView/res/` 只随包分发 `DSLogic.fw`、`DSLogicPro.fw`、`DSCope.fw`、`DSCope20.fw`，**没有** `DSLogicU2Basic.fw` / `DSLogicU2Basic-pgl12-2.fw`（dsl.h 引用了它们）。三份 U2Basic FPGA 位流 `.bin` 均在。结合 §3 第 1 点（VID 过滤），U2Basic 正常上电路径不需要运行时上传 .fw——[待实机验证]：若实机 `dslab doctor` 报缺 fw 文件且设备无法激活，再从 DSView 官方发行包补齐对应 .fw。

## 4. GUI 的采集调用链（本项目 CLI 的参照）[源码确认]

DSView 界面（`DSView/pv/sigsession.cpp`）对库的使用序列：
1. `ds_set_datafeed_callback(data_feed_callback)`（sigsession.cpp:157；API 声明 libsigrok.h，实现 lib_main.c:279）。
2. `ds_get_device_list`（lib_main.c:284）→ `ds_active_device(handle)`（lib_main.c:342，内部 `dev_open` 序列见 §3；USB 设备要求先 `ds_set_firmware_resource_dir`，lib_main.c:398-400）。
3. 配置（`ds_set_actived_device_config` → `sr_config_set` → 驱动 `config_set` dslogic.c:740）：`SR_CONF_DEVICE_MODE=LOGIC`（dslogic.c:915，会重建 probe 列表）、`SR_CONF_OPERATION_MODE`（stream/buffer/intest，dslogic.c:981）、`SR_CONF_CHANNEL_MODE`（dslogic.c:1043）、通道使能 `ds_enable_device_channel_index`（lib_main.c:1050）、`SR_CONF_SAMPLERATE`（dslogic.c:883）、`SR_CONF_LIMIT_SAMPLES`（dslogic.c:779）。
4. `ds_start_collect()`（lib_main.c:721）；采集结束以 `DS_EV_COLLECT_TASK_END/..._BY_ERROR/..._BY_DETACHED`（libsigrok.h:1301-1316）事件通知。
5. `ds_stop_collect()`（lib_main.c:840，`sr_session_stop` + join 采集线程）、`ds_release_actived_device()`（lib_main.c:873）、`ds_lib_exit()`（lib_main.c:192）。

datafeed 回调收到的包类型（sigsession.cpp:1393 `data_feed_in`；枚举 libsigrok.h:195-204）：`SR_DF_HEADER`（采集开始）、`SR_DF_META`、`SR_DF_TRIGGER`（payload 为 `ds_trigger_pos`，含 `check_id==0x55555555` 与 `remain_cnt` 触发位置，dsl.c:2441-2488）、`SR_DF_LOGIC`（payload `sr_datafeed_logic`，libsigrok.h:384-393）、`SR_DF_OVERFLOW`（流模式溢出，dslogic.c:1325-1334）、`SR_DF_END`（`finish_acquisition` dsl.c:2161-2177）。包级错误用 `packet.status != SR_PKT_OK` 表达（如触发包损坏 dsl.c:2479-2485）。

## 5. 采集启动与 USB 传输 [源码确认]

`dev_acquisition_start`（dslogic.c:1406）：
1. `actual_samples = (limit_samples+1023) & ~1023`（`SAMPLES_ALIGN` = libsigrok.h:106；对齐到 1024 的倍数）；`actual_bytes = actual_samples/64 * ch_num * 8`（dslogic.c:1436-1437）。
2. `dsl_configure_probes`（dsl.c:397）把各通道 trigger 字符串（'0'/'1'/'r'/'f'…，多字符=多级）转成 mask/value 位图。
3. 发 `DSL_CTL_STOP` 清残留采集 → `dsl_fpga_arm`（dsl.c:1015）：组装 `struct DSL_setting`（模式字、分频器、采样计数、触发位置、通道使能位图 ch_en_l/h、16 级触发表），经 `DSL_CTL_BULK_WR` + bulk EP 0x02 写入（dsl.c:1223-1259），轮询 `bmSYS_CLR` 脱位后检查 `bmGPIF_DONE`（dsl.c:1234-1243, 1286-1297）。
4. `dsl_start_transfers`（dsl.c:2490）提交异步 bulk IN（EP 0x86）：1 个触发头传输（512B，`dsl_header_size` dsl.c:2092）+ N 个数据传输。stream 模式缓冲 ≈20ms 数据、512 对齐（`get_buffer_size` dsl.c:2103），总缓冲 ≈100ms（`get_number_of_transfers` dsl.c:2125，上限 64）。
5. 注册 libusb pollfd 到 session（dslogic.c:1489-1503）→ 发 `DSL_CTL_START`（dslogic.c:1505）→ 发 `SR_DF_HEADER`。

数据流：pollfd 就绪 → `receive_data`（dslogic.c:1336）→ `libusb_handle_events_timeout_completed`（dslogic.c:1355）→ libusb 回调 `receive_transfer`（dsl.c:2308）：LOGIC 包 `cur_sample_count = actual_length*8/ch_num`（dsl.c:2347），累计满 `actual_bytes` 后状态机转 DSL_STOP → `free_transfer` → 全部传输回收后 `finish_acquisition` 发 `SR_DF_END`（dsl.c:2200-2203, 2161）。溢出检测：连续 16 次空轮询后读 `DSL_CTL_HW_STATUS` 的 `bmSYS_OVERFLOW`（dslogic.c:1366-1394）→ `SR_DF_OVERFLOW`。

超时与停止：`dsl_get_timeout`（dsl.c:2145，stream 模式=总缓冲时长+25% 余量）；用户停止走 `sr_session_stop`（session.c:248 置 abort 标志）→ 迭代循环里调 `current_device_acquisition_stop`（session.c:178, 228）→ `dsl_dev_acquisition_stop`（dsl.c:2007）发 `bmFORCE_RDY` 让 FPGA 提前交数据。

## 6. 原始数据格式（LA_CROSS_DATA）[源码确认] — **本 CLI 必须正确复刻**

`receive_transfer` 标注 `logic.format = LA_CROSS_DATA`（dsl.c:2349）。解包参照 DSView 界面 `DSView/pv/data/logicsnapshot.cpp:221` `append_cross_payload()`：

- 原子块 = 每通道 64 个样本 → 8 字节（`DSLOGIC_ATOMIC_SAMPLES=64`/`DSLOGIC_ATOMIC_SIZE=8`，dsl.h:83-85；`ScaleSize=8`，logicsnapshot.h:53）。
- 字节流布局：`[ch0 的 8 字节][ch1 的 8 字节]…[chN-1 的 8 字节]` 依组重复（logicsnapshot.cpp:326-328 `chans_read_addr[i] = data_src_ptr + i` 以 8 字节为步长交错）。
- **通道槽序 = 使能通道按 index 升序**（FPGA `ch_en_l/h` 位图按 index 打包，dsl.c:1117-1125；接收端 `_channel_num = dsl_en_ch_num()`，dsl.c:225 统计使能通道）。
- **块内位序 = LSB-first**：样本 n（0..63）= 该通道第 8 字节块的 `byte[n/8]` 的 `bit(n%8)`（`get_sample_self`：`1ULL << (index & 63)` 对小端 u64 取位，logicsnapshot.cpp:688-710；写入端为逐字节原样拷贝 logicsnapshot.cpp:273-277）。

即（N 个使能通道、第 g 组）：`sample(g*64+n, ch_slot c) = (raw[(g*N+c)*8 + n/8] >> (n%8)) & 1`。

## 7. stream/buffer 模式与能力约束 [源码确认]

- stream：`SR_CONF_OPERATION_MODE=LO_OP_STREAM`（dslogic.c:1000），数据实时上传，深度只受主机限制；`limit_samples` 任意（对齐 1024）。实测带宽约束 = USB2 HS bulk（[待实机验证] 长采集是否溢出）。
- buffer：`LO_OP_BUFFER`（dslogic.c:986），先存板载 RAM（U2Basic hw_depth 64MB 共享），触发后 `SR_CONF_WAIT_UPLOAD`/`SR_BUF_UPLOAD` 语义（dslogic.c:686-695, bufoption dslogic.c:59-63）。触发位置上限：stream≤10%、buffer≤`DS_MAX_TRIG_PERCENT`（dsl.c:1104-1108）。
- 通道数/采样率联动：`dsl_adjust_samplerate`（dsl.c:199）按 `samplerates_min/max_index` 收紧（channel_modes 各模式 min/max，dsl.h:371-378）。超出表值会失败 [待实机验证报错路径]。
- RLE：`SR_CONF_RLE_SUPPORT`/`SR_CONF_RLE`（dslogic.c:773, 901），M1 不用。

## 8. libsigrokdecode4DSL（解码库）[源码确认]

- 纯 C + Python 嵌入：源码清单 `libsigrokdecode4DSL/`（type_decoder.c 等，见顶层 CMakeLists.txt:482-500），顶层构建链接 `${PY_LIB}`（CMakeLists.txt:614-623）。解码器本体是 Python 包 `libsigrokdecode4DSL/decoders/`（i2c/spi/uart 等均在，安装到 share/libsigrokdecode4DSL，CMakeLists.txt:700）。
- 解码器搜索路径可配置（`srd_decoder_load`/`SRD_DECODERS_DIR`），不依赖 GUI；上游最近一条提交（即我们所固定 HEAD）恰是"decoder 目录不存在时打印警告"，说明路径由宿主程序设置。
- **M3 实施偏差（已声明）**：计划"优先复用原有解码器"；实施时为确定性回归
  （golden fixture 重放稳定）与避免 Python 嵌入进入最小构建，UART/I2C/SPI
  解码器由本项目用 C 自研（src/decode.c），并用独立 Python 打包器做交叉验证
  （tests/fixtures/gen_fixtures.py）。libsigrokdecode4DSL 保留为后续可选后端
  （M5+）：接入时需加 Python 开发库依赖并复跑全部 golden 对拍。

## 9. 构建解耦 [源码确认 → 本项目方案]

上游顶层 `CMakeLists.txt` 把 Qt6/boost/FFTW/Python 与 `libsigrok4DSL_SOURCES`（CMakeLists.txt:456-489）+`common_SOURCES`（minizip/xlog，CMakeLists.txt:505-516）编进同一可执行文件。**libsigrok4DSL 本身只依赖 glib-2.0 / libusb-1.0 / zlib / pthread + common(minizip,xlog)**（输入输出模块 in_wav/srzip 用 zlib；log.h:25 `#include <log/xlog.h>`）。本项目另建最简 CMake 目标只编译库源码 + CLI，不碰 Qt/boost/FFTW（详见 docs/build.md，含实测结果）。

## 10. 平台风险

- **Windows（本机，M1 宿主）**：libusb 后端为 WinUSB；DSLogic 设备官方驱动为 libusb-win32/WinUSB [待实机验证：设备管理器中实际驱动]。无 udev 权限问题，但首次可能需要 Zadig 装驱动。MSYS2 UCRT64 工具链可提供全部构建依赖。
- **Linux**：需 udev 规则（上游带 `DSView/DreamSourceLab.rules`，CMakeLists.txt:688-698 安装为 60-dreamsourcelab.rules）。
- **macOS**：依赖系统 libusb 无需驱动签名外的特殊处理；未实测，标 untested。

## 11. 数据保存与互操作 [源码确认]

- `output/` 模块含 srzip（.dsl 会话格式）、csv、gnuplot、vcd（CMakeLists.txt:472-476），输入侧 in_binary/in_vcd/in_wav + `ds_device_from_file()`（lib_main.c:543，经 minizip 读 .dsl）。
- 本项目 v1 自有 `capture.raw`（cross 原始流，无损、格式见 §6 + docs/cli.md）+ `capture.json` 元数据；导出 VCD/CSV 与 .dsl 互操作列为 M5 可选项，实现时必须用真实文件对拍，不得凭格式猜测宣称兼容。

## 12. 许可证

- 上游及 vendored 代码：GPL-3.0（`COPYING`、`libsigrok4DSL/COPYING`、`libsigrokdecode4DSL/COPYING` 均为 GPLv3 全文）。minizip（common/minizip）为 zlib 许可（原始版信息见其源码头）。本项目整体按 GPL-3.0-or-later 发布，详见 docs/licensing.md。

## 13. 审计中发现的实现注意点（给 M1/M2 的坑）

1. `ds_lib_init()` 内部就做首次扫描（lib_main.c:172），若设备未固件化需要上传 fw，则 **必须在其之前** 调 `ds_set_firmware_resource_dir()`（它只是写全局字符串，可在 init 前调用）。
2. 设备表默认包含一个 **demo 虚拟设备**（lib_main.c:152 `make_demo_device_to_list`），`devices` 命令必须按 `dev_type` 过滤（`ds_get_actived_device_info` 可取 `dev_type`，lib_main.c:652）。
3. `config_get/set` 大量分支要求 `sdi->status == SR_ST_ACTIVE`（dslogic.c:756-759），配置前必须已激活。
4. `SR_CONF_DEVICE_MODE` 设置会**重建通道列表并清除使能状态**（dslogic.c:915-974 `sr_dev_probes_free`→`dsl_setup_probes`），随后必须重新使能通道。
5. `dsl_check_conf_profile` 在设备被占用时返回 TRUE（dsl.c:265-270），独占冲突推迟到 `libusb_claim_interface`（dsl.c:1847）→ `SR_ERR_DEVICE_IS_EXCLUSIVE`（错误码 11，libsigrok.h:81）。CLI 需把它映射为 `DEVICE_BUSY`。
6. 库日志走 xlog 默认 **stderr**（common/log/xlog.c:74-80），stdout 可安全输出纯 JSON。
7. 触发公共 API：`ds_trigger_probe_set/stage_set_value/set_en/set_pos/...`（libsigrok.h:1261-1272），通道 trigger 字符串 sigrok-cli 语法（dsdevice.c:150 注释）。
8. 采集结束只有事件回调（无阻塞 wait API）；CLI 需自行实现带超时的事件等待（M1 用轮询 `ds_is_collecting` + 事件标志）。
9. **【2026-10-23 实机证实】`dev_acquisition_start` 对 `libusb_get_pollfds()`
   不判空**（dslogic.c:1489）：libusb ≥1.0.24 的 Windows 后端移除了 pollfd
   仿真（恒返回 NULL），直接遍历即段错误——官方 DSView Windows 包因静态链
   libusb 1.0.23 而幸免。dslab 已打 vendor 补丁（判空 + fd=-1 空转泵 + 限速，
   见 docs/upstream.md）；`dscope.c:2091` 存在同型代码，本项目未改（无 DSCope
   设备验证），升级/复用时注意。上游 issue 待报。
