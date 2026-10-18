# 身份与存档

源码仅有两份演示配置：`models/band11/config/factory/factory.prop`、`models/band10pro/config/factory/factory.prop`。支持离线启动；真实账号绑定需对应设备的有效身份。

将自己的两个型号配置保留在工程外，通过组装参数导入：

```text
--factory-band11 C:/private/band11/factory.prop
--factory-band10pro C:/private/band10pro/factory.prop
```

生成器在新运行目录中创建出厂分区和初始存储。DID、序列号与设备密钥需要匹配；修改 DID/SN 不会自动生成有效身份。10 Pro 只导入身份、地址、SKU 和已支持的 GNSS 选择项，不套用实物的晶振/传感器校准。

运行目录的原生传感器窗口带有 DID / 序列号工具页。修改前停止对应型号；工具会保存备份。11 NFC 的 `models/band11/userdata/nand-bluetooth*.img` 和 10 Pro 的 `models/band10pro/userdata/flash.journal` 都属于私人运行状态。

需要全新镜像时，用相同编译产物和固件再次调用组装工具，指定新的 `--output`。原存档保留在旧目录。恢复出厂通常仍保留出厂身份；它不能用于清除公开包中的私人凭据。源码校验工具固定检查演示配置，运行目录请单独保管。
