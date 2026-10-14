# 设备接入说明（U2Basic）

## Windows（本仓库首个宿主平台）

1. **USB 线**：用质量可靠的数据线把 U2Basic 接到主机 USB2/USB3 口（设备要求
   High-Speed）。U2Basic 无独立供电，USB 供电不足时可能反复重枚举。
2. **驱动**：DSLogic 设备使用 WinUSB/libusb-win32 类驱动。
   - 若 DSView 官方版曾安装过，驱动通常已就位。
   - 全新主机可用 [Zadig](https://zadig.akeo.ie/)：选中 `DSLogic ...`（VID 2A0E）
     安装 **WinUSB**。注意：Zadig 会替换该 USB 设备的驱动，是系统级改动，
     操作前确认选中的是正确的设备。
   - `dslab doctor --json` 的 `usb_open` 检查会在无法打开设备时提示驱动问题。
3. **权限**：普通用户即可（无需管理员运行 dslab）。
4. **独占性**：同一时刻只能有一个进程占用设备。DSView GUI 打开设备时，
   dslab 报 `DEVICE_BUSY`；反之亦然。

## 资源文件

`dslab` 需要固件/FPGA 资源目录（默认 `vendor/DSView/res`，可用环境变量
`DSLAB_RES_DIR` 覆盖）：
- U2Basic (PID 0x0029)：`DSLogicU2Basic.bin`（FPGA，必须）
- U2Basic (PID 0x0031)：`DSLogicU2Basic-pgl12.bin`（FPGA，必须）
- U2Basic (PID 0x0035)：`DSLogicU2Basic-pgl12-2.bin`（FPGA，必须）
- `*.fw`（FX2 固件）：上游仓库未随包分发 U2Basic 的 .fw；按源码审计，U2Basic
  上电即以 VID 2A0E 枚举（scan 只认该 VID），正常路径不需要运行时上传 .fw。
  `doctor` 对缺失 .fw 仅报 warn；若实机激活失败再从 DSView 官方发行包补齐。

阈值说明：FPGA 位流按电压阈值分 3V3/5.0V 两套（U2Basic 同名文件即 3V3 档）。
M1 不改阈值（默认 3V3 档、`vth=1.0` 由驱动 dev_open 时写入）。

## 安全

- 只接低压数字信号（≤5V，符合设备规格）；接被测系统前**必须共地**。
- 禁止连接市电/高压/未知电平。模拟量测不在本项目范围。

## Linux（未实机验证）

udev 规则（上游 `DSView/DreamSourceLab.rules`，MODE 0666）：
```
SUBSYSTEM=="usb", ATTRS{idVendor}=="2a0e", MODE="0666"
```

## macOS（未实机验证）

无需驱动；libusb 直接可用。
