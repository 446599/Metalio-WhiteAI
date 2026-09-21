# Web flasher 验证记录 · 2026-09-21

基础固件提交：`f4216eaeb116c05ce127319742104dd3a4d8805d`。本次新增独立静态网页，没有修改固件、分区布局、字体或用户数据。

## 已执行

- Node.js 22.16.0：`node --test web-flasher/tests/core.test.mjs`，**84 项测试全部通过**。
- Python：`python3 web-flasher/tests/vendor_test.py`，**5 项离线安装器测试通过**。
- Chromium / Playwright：`browser_test.py --in-memory`，**17 条浏览器交互流程通过**，无未捕获页面错误。
- 所有生产 `.mjs` 文件通过 `node --check`；默认布局与仓库 `partitions/v1/16m.csv` 逐字段比较一致。
- 已实际渲染并检查 1280 px 桌面页面和 390 px 窄屏页面。窄屏只有分区表内部可横向滚动，文档本身不横向溢出。预览不入库。

核心测试覆盖 MD5 标准向量及随机二进制、CSV 自动对齐与字段、二进制表 MD5、空文件、容量越界、错误芯片、擦除扇区重叠、敏感确认、自定义边界、加密与安全状态拒绝、启动数据完整性、首次部署、文件只读加载、端口互斥、状态改变、USB 中断、写后 MD5 失败和分区备份。

浏览器测试覆盖默认选择、连接、两级确认、应用刷写、NVS 风险确认、错误芯片/超限拒绝、文件名 HTML 注入防护、确认后表变化、备份、首次部署、自定义行、导入布局不匹配、断开恢复、窄屏排版和不支持浏览器提示。

## 明确未验证

测试环境禁止浏览器网络导航，因此浏览器用内存加载实际源文件，替换的是 esptool 适配器、SerialPort、安全上下文、SHA-256 测试绑定和下载接收器。**没有连接 USB 硬件，没有实际下载 CDN SDK，也没有验证原生串口权限、驱动、复位时序、Flash 物理写入、设备重启或显示效果。** 这不是“真实刷机通过”的记录。

SDK 接口依据 Espressif `v0.7.0` 原始 TypeScript 源码核对：`Transport`、`ESPLoader.main`、`getSecurityInfo`、`detectFlashSize`、`readFlash`、`writeFlash`、`flashMd5sum`、`after`。固定官方 SDK 安装包 SHA-256 取自其 GitHub Release 元数据；离线安装器测试使用合成归档，不冒充下载验证。

## 设备验收建议

先在可恢复的测试设备上备份分区表和 NVS，进行应用单分区写入，比较写前写后未选区域；再独立测试字体、错误文件、网络加载失败、手动下载模式以及 USB 中断后的恢复。最后才测试首次部署和分区布局迁移。OTA 选择不自动判断，需要人工核对。
