# 构建说明（Windows / MSYS2 UCRT64 实测）

## 依赖

| 依赖 | 用途 | MSYS2 包 |
|---|---|---|
| gcc (C99) | 编译 | `mingw-w64-ucrt-x86_64-gcc` |
| cmake ≥3.16 + ninja | 构建 | `mingw-w64-ucrt-x86_64-cmake` / `-ninja` |
| pkg-config | 依赖发现 | `mingw-w64-ucrt-x86_64-pkg-config` |
| glib-2.0 | libsigrok4DSL 核心依赖 | `mingw-w64-ucrt-x86_64-glib2` |
| libusb-1.0 | USB 通信 | `mingw-w64-ucrt-x86_64-libusb` |
| zlib | 会话文件/输入输出模块 | `mingw-w64-ucrt-x86_64-zlib` |
| python3 | 仅运行测试脚本（非链接） | 系统任意 Python 3 即可 |

不需要 Qt / boost / FFTW / Python 开发库——它们只属于 DSView 的 GUI 部分。

## 安装与构建（本机实测命令）

```bash
# 1) 安装 MSYS2（本机装在 C:\Users\pan39\msys64；官方默认 C:\msys64 亦可）
#    winget 走不了代理时手动下载安装器：
curl -x http://192.168.31.165:7890 -L -o msys2.exe \
  https://github.com/msys2/msys2-installer/releases/download/2026-06-11/msys2-x86_64-20260611.exe
./msys2.exe in --confirm-command --accept-messages --root C:/Users/pan39/msys64

# 2) 安装依赖（MSYS2 UCRT64 shell，或任意 shell 调用其 pacman；需代理时先 export）
export http_proxy=http://192.168.31.165:7890 https_proxy=http://192.168.31.165:7890
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake \
          mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkg-config \
          mingw-w64-ucrt-x86_64-glib2 mingw-w64-ucrt-x86_64-libusb \
          mingw-w64-ucrt-x86_64-zlib

# 3) 构建（在仓库根目录；Git Bash 中先把 ucrt64/bin 加入 PATH）
export PATH=/c/Users/pan39/msys64/ucrt64/bin:$PATH
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 4) 测试
ctest --test-dir build --output-on-failure
```

本机实测（2026-10-14，Windows 10 26200 x64，gcc 16.2.0 / cmake 4.x / ninja）：
配置 + 全量编译链接成功；`ctest` 3/3 通过（unit / cli_contract / demo_pipeline）。

## 运行时 DLL

构建脚本自动把 `libusb-1.0.dll`、`libglib-2.0-0.dll`、`libpcre2-8-0.dll`、
`libintl-8.dll`、`libiconv-2.dll`、`libwinpthread-1.dll`、`zlib1.dll` 拷贝到
`build/`，因此 `build/dslab.exe` 可脱离 MSYS2 PATH 直接运行。
分发时把 `dslab.exe` 与这些 DLL、`vendor/DSView/res` 一起打包即可。
（曾尝试全静态链接：glib 静态依赖 pcre2/gettext/iconv 链条过深，收益低，
改为随包分发 DLL，与 DSView 官方 Windows 包做法一致。）

## 固件资源目录

默认使用编译期内置路径 `vendor/DSView/res`（`DSLAB_RES_DIR_DEFAULT`），运行时
可用环境变量 `DSLAB_RES_DIR` 覆盖；`doctor` 会检查每台已连接设备需要的
`.fw/.bin` 是否存在。

## 其他平台（未实机验证，untested）

- Linux：相同依赖用系统包管理器安装（`libglib2.0-dev libusb-1.0-0-dev zlib1g-dev`）。
- macOS：`brew install glib libusb zlib cmake ninja`。
- 两者均只验证到"可移植的 CMake + pkg-config 结构"层面，尚未在本项目实测构建。

## 发布（双远端）

两个远端的发布机制不同：

- **GitHub**（LIGHT1213/dsviewcli，公开）：有 CI。推送 `v*` tag 后
  `.github/workflows/build.yml` 的 release job 自动构建 zip/7z 并挂到
  GitHub Release，全程无需人工操作。
- **Gitea**（light/dsviewcli，自建）：**没有 CI runner**。用
  `scripts/publish-gitea-release.sh` 把 GitHub CI 已发布的产物镜像过去：

  ```bash
  export https_proxy=http://192.168.31.165:7890   # 访问 GitHub 需要
  GH_TOKEN=<GitHub PAT> scripts/publish-gitea-release.sh v0.2.1
  ```

  脚本流程：按 tag 从 GitHub Release 下载 CI 产物 → 计算 sha256 →
  Gitea 上不存在该 release 则创建（notes 含各产物 sha256，单行 ASCII）→
  逐个上传缺失资产（同名已存在则跳过，可重复执行）。Gitea token 读
  `~/.config/gitea-ops/config.json`，或用 `GITEA_TOKEN`/`GITEA_HOST` 覆盖。

  发版顺序：commit → 双远端 push → 打 tag 并 push → 等 GitHub CI 的
  release job 完成 → 跑上面的镜像脚本。

## 升级上游

更新 `upstream/DSView`（完整克隆，gitignored）→ 同步拷贝到 `vendor/DSView` →
更新 docs/upstream.md 的 SHA → 重跑 ctest 与实机回归。
