# 架构

源码工程分成固定版本公共 QEMU 源码、可直接查看的型号覆盖文件，以及运行适配器。`prepare_source.py` 展开公共归档，先应用 11 NFC 文件，再为 10 Pro 删除和覆盖对应差异；逐项检查型号清单。两个型号分别编译为独立 QEMU 核心。

`assemble_runtime.py` 收集新编译核心及 DLL、外部 Python/LittleFS 依赖和匹配固件，在全新目录调用 `create_images.py`。后者从文本配置新建出厂 LittleFS；11 NFC 再写入擦除态 NOR/NAND，10 Pro 生成 NOR 种子。不会复制旧用户存储。

生成后的原生窗口通过 `models/models.json` 选择型号，`tools/simulator.py` 负责依赖预检、单实例锁、进程监督和正常停止。型号适配器在独立 Python 进程运行，避免重名模块互相覆盖。原厂 AP 通过 MMIO、SPI、I²C、UART、显示和触摸模型运行。

两型号共用 Python、DLL、HCI、屏幕预览及 UI 控件，固件、身份、传感器输入和存档各自隔离。DLL 收集会拒绝同名不同内容的冲突。10 Pro 使用 journal 记录运行期间的 Flash 变更；11 NFC 使用独立的可写 Flash 镜像。

原始 HCI 桥接通过 WinUSB 接管控制器，物理无线锁仍在 `%TEMP%/xiaomi-q66-radio/radio.lock`，与旧工程保持互斥。离线启动不接管控制器。
