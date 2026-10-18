# 构建用参考固件

| 型号 | 版本 | 分卷 | 压缩后大小 |
| --- | --- | --- | --- |
| 11 NFC / `band11` | `4.100.139` | `band11/firmware.zip.part001` 至 `part003` | 66,695,099 字节 |
| 10 Pro / `band10pro` | `3.101.043` | `band10pro/firmware.zip.part001` 至 `part004` | 90,732,964 字节 |

每份小于 25 MB，无需 Git LFS。请下载完整仓库并保留目录及文件名；`tools/assemble_runtime.py` 默认读取对应 `manifest.json`，自动核对分卷和合并 ZIP 的 SHA256，再解压并核对每个固件镜像的大小和 SHA256，无需手动合并。

固件为此前源码构建与冷启动验收使用的同一组 AP、资源和 OTA 镜像；10 Pro 另含 BL2。它们来自原厂固件包，文件内容未改动。出厂身份由 `models/<型号>/config/factory/factory.prop` 单独生成，用户 Flash 由构建工具新建。

可以通过 `--firmware-band11 <路径>`、`--firmware-band10pro <路径>` 指定其他匹配的完整 OTA ZIP、提取目录或分卷清单。依赖和完整步骤见 [构建说明](../docs/BUILD.md)。

这些原厂二进制资产的权利归各自权利人；项目根目录的 QEMU GPL 许可证不授予其版权许可。
