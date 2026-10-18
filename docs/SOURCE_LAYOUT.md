# 源码交付布局

工程根目录保留可编辑代码、两份演示配置、构建脚本、文档和构建用原厂固件分卷。不存在预编译 EXE/DLL、内嵌 Python、Flash 或旧存档。无需 Git LFS。

公共 QEMU 源码固定在 `vendor/qemu-common.tar.xz` 和 `vendor/qemu-support.tar.xz`，每个小于 25 MB，版本及哈希见 `vendor/manifest.json`。两型号代码保留在 `source/overlays/`，构建时自动展开为完整源码。`vendor/bes-reference.tar.xz` 保存 BES 原始模型、工具、来源和许可，用于溯源；本工程构建使用已经适配的型号覆盖文件。

生成目录 `.build/` 和 `dist/` 不属于源码交付。公共发布检查只接受两份固定的演示配置，真实身份应作为工程外输入。每次生成新的运行目录，旧目录中的身份、绑定和存档不受影响。

`firmware/<型号>/` 包含参考固件 ZIP 的分卷及 `manifest.json`，每份小于 25 MB。组装工具默认自动校验和解压；每份清单只登记原厂 AP、资源、OTA 和可选 BL2，不携带出厂配置或用户存储。

校验源码：

```powershell
python -B tools/check_publication.py
```

确认代码变更后更新文件校验，再导出源码包：

```powershell
python -B tools/check_publication.py --refresh-checksums
python -B tools/package_release.py --output C:/build/Miband-qemu-simulator-source.zip
```

导出工具将源码及参考固件分卷写入 ZIP，不打包编译缓存、Python/DLL 运行依赖或私人数据。ZIP 和解包后的项目目录均可用于构建。
