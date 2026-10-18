# 编译和镜像生成

目标为 Windows 10/11 x64。QEMU 版本 11.1.50，两个型号分别编译，各自使用独立缓存。

## 开发依赖

安装 Git、Python 3.11+ 和 MSYS2。在 MSYS2 UCRT64 终端安装：

```bash
pacman -S --needed base-devel git mingw-w64-ucrt-x86_64-toolchain \
  mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-pip \
  mingw-w64-ucrt-x86_64-meson mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-glib2 \
  mingw-w64-ucrt-x86_64-pixman mingw-w64-ucrt-x86_64-SDL2 \
  mingw-w64-ucrt-x86_64-libfdt mingw-w64-ucrt-x86_64-zlib \
  mingw-w64-ucrt-x86_64-zstd mingw-w64-ucrt-x86_64-bzip2
```

QEMU configure 可能下载 Python 构建依赖或缺少的子项目。运行时使用 CPython 3.11，与 MSYS2 的构建 Python 分开。

## 编译

PowerShell，在项目根目录：

```powershell
.\Build.cmd -Toolchain C:\msys64 -CheckOnly
.\Build.cmd -Toolchain C:\msys64 -BuildRoot C:\build\miband -Model all -Jobs 4
```

也可单独选 `band11` 或 `band10pro`。省略 `-BuildRoot` 使用仓库内 `.build`。建议源码、工具链与构建目录使用 ASCII 路径。

等价 Bash 入口（在 UCRT64 / Git Bash 中）：

```bash
TOOLCHAIN_ROOT=/c/msys64 QEMU_BUILD_ROOT=/c/build/miband/band11 bash tools/build-qemu.sh band11
TOOLCHAIN_ROOT=/c/msys64 QEMU_BUILD_ROOT=/c/build/miband/band10pro bash tools/build-qemu.sh band10pro
```

输出分别为 `<BuildRoot>/<型号>/build/qemu-system-arm.exe`。源码在同级 `qemu/`。首次展开的 11 NFC 11,560 个文件、10 Pro 11,648 个文件均按型号完整清单核对 SHA256。

只展开源码：

```powershell
python -B tools/prepare_source.py --model band10pro --output C:/build/p67-source
```

开发时可直接编辑展开后的树并增量编译。修改覆盖目录之后，请换新的 BuildRoot；已有缓存不会自动重做覆盖。修改过的型号覆盖文件可用 `prepare_source.py --no-verify` 展开到新目录，再通过 `QEMU_BUILD_ROOT` 构建该目录；固定清单的默认校验用于确认原始交付，开发后应更新它。

## 运行依赖和固件输入

准备 Windows x64 CPython 3.11 embeddable ZIP，以及 littlefs-python 0.19.0 的 cp311 win_amd64 wheel。可以用开发 Python 下载 wheel：

```powershell
python -m pip download --only-binary=:all: --no-deps --platform win_amd64 --python-version 311 --implementation cp --abi cp311 littlefs-python==0.19.0 -d C:/inputs
```

仓库 `firmware/` 已提供 11 NFC `4.100.139` 和 10 Pro `3.101.043` 的构建用固件分卷，每份小于 25 MB。默认使用对应型号的 `manifest.json`，无需手动下载固件或合并分卷。组装工具先核对各分卷及合并归档的 SHA256，再核对解压后的镜像大小和 SHA256。

也可通过 `--firmware-band11 <路径>`、`--firmware-band10pro <路径>` 使用其他匹配的完整 OTA ZIP、提取目录或分卷清单。提取目录需有 `vela_ap.bin`、`vela_resource.bin`、`vela_ota.bin`，可选 `vela_bl2.bin`。生成器不执行包内升级脚本，出厂区生成会核对 AP 的型号配置。增量补丁包不能代替完整 AP/资源镜像。

## 重建镜像

```powershell
python -B tools/assemble_runtime.py --build-root C:/build/miband --toolchain C:/msys64 `
  --python-embed C:/inputs/python-3.11.9-embed-amd64.zip `
  --littlefs-wheel C:/inputs/littlefs_python-0.19.0-cp311-cp311-win_amd64.whl `
  --output C:/build/miband-runtime
```

默认构建两型号；`--model band11` / `--model band10pro` 只组装指定型号。`--build-root` 必须与编译时一致。生成器也接受 `--python-runtime <已解压的官方 embeddable 目录>` 和 `--littlefs-package <已安装的包目录>`，分别替代两个 ZIP/wheel 参数。

生成器只读取输入，先在临时目录完成组装、导入检查、核心依赖检查和镜像生成，成功后才改名为输出目录。任一输入无效则退出；已有输出始终保留。每次重建选择新的输出目录。

| 型号 | 新建镜像 / 状态 |
| --- | --- |
| 11 NFC | 128 KiB 出厂 LittleFS；16 MiB NOR，包含 AP 与出厂区；136 MiB 用户 NAND；136 MiB 资源 NAND |
| 10 Pro | 128 KiB NV LittleFS（name_max=96）；16 MiB NOR 出厂种子；首次启动创建 `flash.journal` |

AP、资源和 OTA 来自输入的原厂文件，Flash 初始擦除字节为 `FF`；不复用旧 Flash、journal 或账号绑定。`build-report.json` 记录核心与固件哈希，`userdata/<型号>-image-build.json` 记录新生成镜像的大小与哈希。

可加 `--factory-band11 C:/private/11.prop`、`--factory-band10pro C:/private/10p.prop` 导入私人身份；省略则使用源码中的演示配置。原始 HCI 模式还需 `--intel-firmware C:/inputs/intel`，放置 `ibt-1040-4150.sfi`、`ibt-1040-4150.ddc`、`LICENCE.intel`，以及安装好 WinUSB 的已验证 Intel 控制器。

## 启动与检查

双击生成目录中的 `Launch.cmd`。命令行使用该目录的 Python：

```powershell
.\runtime\python\python.exe -X utf8 -B tools/simulator.py check --model band11
.\runtime\python\python.exe -X utf8 -B tools/simulator.py start --model band11 --mode offline
.\runtime\python\python.exe -X utf8 -B tools/simulator.py sensors --model band11
```

屏幕、触摸和传感器仍由原厂固件与硬件模型交互。GPU、完整 NFC 和部分传感器算法尚不完整，详见型号说明。重新生成初始镜像与在 App 内验证 OTA 是不同操作。
