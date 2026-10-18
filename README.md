# Mi Band QEMU Simulator

Windows 上运行小米手环原厂固件的 **待编译源码工程**，包含 11 NFC、10 Pro 及两型号的参考固件。编译 QEMU 后，从固件和两份配置重新生成运行镜像。屏幕、触摸、原生传感器窗口及原始 HCI 蓝牙适配器的源码均保留。

| 型号参数 | 设备 / PID | 参考固件 | 屏幕 |
| --- | --- | --- | --- |
| `band11` | 11 NFC · `q66nfc` / `34835` | `4.100.139` | 212×520 |
| `band10pro` | 10 Pro · `p67cn` / `29598` | `3.101.043` | 336×480 |

## 构建

安装 Windows x64 Python 3.11+、Git、MSYS2 UCRT64 开发工具，依赖列表见 [构建说明](docs/BUILD.md)。在项目根目录运行：

```powershell
.\Build.cmd -Toolchain C:\msys64 -Model all
```

首次构建会展开固定版本的 QEMU 公共源码，应用型号覆盖文件并逐文件校验，再分别编译。产物在 `.build/<型号>/build/qemu-system-arm.exe`。修改硬件代码可编辑 `source/overlays/<型号>/`；缓存更新规则见构建说明。

## 生成全新运行目录和镜像

仓库已包含两款设备的参考固件分卷，组装工具默认自动校验、合并使用。另准备 Windows x64 CPython **3.11** embeddable ZIP、littlefs-python **0.19.0 cp311 win_amd64** wheel：

```powershell
python -B tools/assemble_runtime.py --toolchain C:/msys64 `
  --python-embed C:/inputs/python-3.11.9-embed-amd64.zip `
  --littlefs-wheel C:/inputs/littlefs_python-0.19.0-cp311-cp311-win_amd64.whl `
  --output C:/build/miband-runtime
```

生成器收集新编译的 EXE 和依赖 DLL，提取固件，以文本配置重建出厂区。11 NFC 生成 NOR、用户 NAND 和资源 NAND；10 Pro 生成 NV/NOR 种子，首次启动创建 journal。**不会导入旧存档，也不覆盖已存在的输出目录。** 再生成时指定新的 `--output`。

完成后双击输出目录中的 `Launch.cmd`，选择型号并离线启动。左键点击、按住拖动滑动，熄屏后快速双击唤醒。10 Pro 可在原生传感器窗口实时旋转。

源码目录不包含预编译 EXE、DLL 和运行镜像；这些产物由构建生成。参考固件位于 [firmware/](firmware/README.md)，单份小于 25 MB，无需 Git LFS。若使用其他版本，可加 `--firmware-band11 <路径>`、`--firmware-band10pro <路径>`，输入完整 OTA ZIP、提取目录或分卷清单。原厂固件本身不从 QEMU 源码编译生成。

## 身份与蓝牙

两份合成演示配置位于 `models/<型号>/config/factory/factory.prop`，可以离线启动。生成私人运行目录时，用 `--factory-band11 <路径>`、`--factory-band10pro <路径>` 导入自己的对应配置。DID、SN 与密钥需匹配，详见 [身份与存档](docs/IDENTITY.md)。

蓝牙直连使用 WinUSB 独占 Intel `8087:0026`，运行期间该控制器交给模拟器。生成时可加 `--intel-firmware <目录>`，目录需含经过校验的 `ibt-1040-4150.sfi`、`.ddc` 及 `LICENCE.intel`。离线模式无需这些文件。两型号和旧工程共用无线互斥锁。

## 结构

```text
Build.cmd                    编译入口
tools/                       还原、编译、镜像生成、统一运行工具
models/                      两型号的适配器、原生控制窗口、两份演示配置
shared/                      HCI、屏幕预览及公共 UI 源码
source/overlays/band11/       11 NFC 模型和相关 QEMU 改动
source/overlays/band10pro/    10 Pro 模型和相关 QEMU 改动
vendor/                      固定版本公共源码归档和校验清单
firmware/                    两型号参考固件分卷、版本和 SHA256 清单
docs/                        构建、功能、身份及验证说明
LICENSES/                    组件许可及来源说明
```

`.build/`、`dist/`、运行镜像和会话由工具生成，不属于源码交付。公共源码分成两个小于 25 MB 的归档，减少重复文件，型号代码保持可直接阅读；构建时自动完整展开。

[构建和镜像](docs/BUILD.md) · [硬件能力](docs/MODELS.md) · [架构](docs/ARCHITECTURE.md) · [源码布局](docs/SOURCE_LAYOUT.md) · [验证](docs/VERIFICATION.md) · [组件许可证](NOTICE.md)
