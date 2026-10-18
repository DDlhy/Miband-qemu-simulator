# 组件与许可

- QEMU 11.1.50：完整公共源码在 `vendor/qemu-common.tar.xz`、`vendor/qemu-support.tar.xz`，型号改动在 `source/overlays/`。文件头、GPL/LGPL 及其他组件许可继续适用，根 `LICENSE` 为 QEMU GPL 参考文本。
- BES 模型：`vendor/bes-reference.tar.xz`，来源 <https://github.com/0x504B0304/bes-qemu>，基线 `e57911d5fa00d28a3f69321c7bdbdca5abda8016`。保留原始来源、许可及本地适配。
- Python、SDL2、GLib、pixman、libfdt、MinGW、LittleFS 等依赖的参考许可保留在 `LICENSES/` 与源码归档中。实际运行依赖由构建者提供，分发生成产物时保留各依赖的完整许可和对应源码义务。
- littlefs-python 0.19.0 为外部构建输入，运行组装时保留 wheel 的 dist-info 许可。
- Intel SFI/DDC 为可选外部输入，组装时保留 `LICENCE.intel`。
- `firmware/` 中的小米 AP、资源、字体、OTA 和 BL2 是原厂二进制资产，按型号分卷保存；版本、文件大小及 SHA256 见各自清单。其权利不由本项目或 QEMU 源码许可证授予。

统一构建入口、运行适配和镜像生成是本地整合内容。原有 Studio/BES 来源说明用于溯源。源码目录仅携带合成演示身份。
