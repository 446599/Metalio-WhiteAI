# WhiteAI 分区刷机页

面向 Metalio E-Ink 4 / ESP32-S3 的简约静态网页。通过 Web Serial 选择本地文件并刷写选定分区，不需要网页后台或 npm 构建。不包含任何固件、字体、设备凭据或账户服务。

## 本地启动

在仓库根目录执行：

```sh
python3 -m http.server 8080 --bind 127.0.0.1 --directory web-flasher
```

用桌面版 Chrome / Edge 打开 `http://localhost:8080`。不要直接双击 HTML，也不要从普通 HTTP 局域网地址打开。macOS 请使用 Chrome / Edge，而不是 Safari。网页仍可在窄屏浏览，但不承诺手机 USB 刷写支持。

页面连接时才加载固定版本 `esptool-js 0.7.0`，默认源为 jsDelivr。文件在浏览器中读取，不上传固件。没有账号、统计或固件下载接口。连接芯片后自动读取真实 Flash 容量、安全状态和 `0x8000` 的分区表；获取失败时不能把默认容量当作实测值。

## 使用

1. 关闭串口监视器，用数据线连接设备，选择“连接设备”。自动复位失败时，可手动进入下载模式后选择“已手动进入下载模式”。
2. 选择方案或逐项勾选分区，再为每个已选分区选一个本地文件。系统分区默认折叠；“自定义地址”可填写起始地址和目标容量，接受 `0x80000`、`524288`、`4K`、`5M` 等形式。
3. 点击“检查刷写计划”。核对芯片、每个文件的地址、大小、SHA-256 和实际擦除范围。普通写入需要确认；敏感分区、首次部署和自定义范围还必须输入 `FLASH`。
4. 写入期间不要关闭页面或拔线。SDK 和页面都执行写后 MD5 校验；完成后可自动或手动复位。哈希通过不代表固件功能已通过真机验收。

### 快捷方案与文件

地址由当前仓库的 `partitions/v1/16m.csv` 固定预设；运行时以设备读回结果检查。用户可导入其他 CSV / 二进制分区表，或点击“使用设备布局”。**导入只改变页面布局，不会自动烧写分区表。**

| 目标 | 地址 | 本项目常见文件 |
| --- | --- | --- |
| bootloader | `0x000000` | `build/bootloader/bootloader.bin` |
| partition-table | `0x008000` | `build/partition_table/partition-table.bin` |
| otadata | `0x00D000` | `build/ota_data_initial.bin` |
| ota_0 | `0x080000` | `build/metalio-hw-test.bin` |
| ota_1 | `0x580000` | 明确选择另一应用槽时使用相应应用镜像 |
| font_data | `0xAE4000` | `build/font_data.bin` 或对应完整 `.fontpack` |

“仅应用”只选 ota_0；“应用＋字体”选 ota_0 和 font_data；“首次部署”选 bootloader、partition-table、otadata、ota_0 和 font_data，**不选择 NVS**。没有镜像时必须先用 ESP-IDF 构建，网页不负责编译代码。分区表、引导程序、应用和字体应来自同一次有效构建。

**刷写 ota_0/ota_1 不会自动切换启动槽。** 网页不解析 OTA 选择记录，也不推断哪个槽当前运行。曾经 OTA 升级的设备请先确认启动槽；刷入初始 otadata 会重置选择，属于另一个明确确认的操作。

## 备份与安全边界

每个非自定义分区均有“备份”按钮。只允许备份与设备读回定义一致的范围，分段读取完成后输出一个本地 `.bin`，不发送到任何服务器。NVS 备份可能含 Wi-Fi、小智凭据和个人数据，应妥善保管。USB 中断不输出不完整备份。

- 不提供整片擦除，SDK 固定 `eraseAll: false`，Flash mode/frequency/size 参数均 `keep`，避免无意修改镜像头。
- 校验真实容量、文件容量、4 KiB 擦除边界、重叠范围、分区匹配、ESP 固件头和 ESP32-S3 chip ID；分区表带 MD5 时验证 MD5。
- 实际擦除粒度为 4 KiB，文件末尾所在扇区的剩余字节也会被擦除；不会自动清空该分区的其他尾部扇区。
- 不支持 Secure Boot、Flash Encryption、安全下载模式，或写入 encrypted 分区。安全状态读取失败也拒绝刷写，不进行绕过或 eFuse 修改。
- 刷写前再次读分区表，与确认时的内容比较。布局改变必须重新连接，不复用旧计划。
- 自定义地址是高级恢复入口，不是“安全刷任意镜像”的承诺。它要求额外确认，不替代硬件型号、固件可信来源和迁移方案核对。此页不接受把合并全 Flash 镜像作为 bootloader 文件；应拆成各分区镜像。
- 文件头、SHA-256 和传输 MD5 不提供固件签名认证。只刷可信且适配板子的镜像。断电造成的部分写入没有自动回滚保证。
- 更换分区表可能影响旧数据的解释，即使数据扇区本身没有被擦除。迁移前请备份。
- 单次文件总量与单个备份限制为 16 MiB，符合本设备；UI 最多 8 个自定义范围。

## 本地 SDK / 离线使用

不便访问 CDN 时，在可联网的机器上执行一次：

```sh
python3 web-flasher/vendor_sdk.py
```

脚本从 Espressif 官方 GitHub Release 下载固定的 `esptool-js-0.7.0.tgz`，先核对固定 SHA-256，再只提取 `package/bundle.js` 和 Apache-2.0 许可。也可以预先下载官方压缩包：

```sh
python3 web-flasher/vendor_sdk.py --archive /path/to/esptool-js-0.7.0.tgz
```

生成的 `web-flasher/vendor/esptool-js-0.7.0.bundle.js` 优先于 CDN。离线部署需要同时带上 vendor 目录和许可文件。脚本不会访问固件或设备，仓库默认不提交下载的第三方二进制/打包产物。本次测试环境未联网下载该发布包；实际 SDK 加载及真实串口通信仍需用户环境验证。

## 测试

核心测试无需安装 npm 依赖，Node.js 20+：

```sh
node --test web-flasher/tests/core.test.mjs
python3 web-flasher/tests/vendor_test.py
```

浏览器测试需要 Python Playwright 和 Chromium：

```sh
python3 web-flasher/tests/browser_test.py --chromium /path/to/chromium
```

USB 始终为模拟对象，不会请求或操作真实设备。浏览器禁止所有网络导航的环境可使用 `--in-memory`：用实际页面源文件和 CSS 运行数据模块，额外模拟安全上下文、SHA-256 绑定和下载，不修改生产页面 CSP。测试范围详见 [VALIDATION.md](VALIDATION.md)。

## GitHub / 静态部署

可把整个 `web-flasher/` 目录作为静态站点部署到 HTTPS 主机，无构建命令。仓库已有 `gh-pages` 站点，网页文件位于该分支根目录；发布修复需同时更新主分支 `web-flasher/` 和站点分支，不能只改固件源码。仅发布此静态目录，不要发布 SD 备份、NVS、凭据或私人资料。SDK Apache-2.0 许可来自 [Espressif esptool-js](https://github.com/espressif/esptool-js/tree/v0.7.0)。


## 1.1.0：选中串口后无响应

连接按钮现在依次显示刷机库、串口打开、下载模式、芯片检查和分区表阶段；选择完端口后可用“取消连接”退出。连接不会擦写 Flash。下载模式握手最长 35 秒，读取分区表最长 15 秒，串口 I/O 和清理也有界。超时后不绕过安全或分区检查。

若自动复位失败，按住 BOOT（AI）并复位/重新上电，松开后在连接选项中选择“已手动进入下载模式”，重新选择当前串口。USB 复位可能重新枚举端口；不要继续使用旧端口。关闭其他网页和串口监视器。错误仍在时保存页面日志，记录具体失败阶段；必要时拔插 USB 后重试。

使用原生 VID 303A / PID 1001 时保持 115200 连接，不为了所选的高速值关闭/重开原生 USB；外接 USB-UART 保留所选波特率。已验证写锁错误后的清理、取消、拔出、重试、晚到 open/握手隔离。取消未完成的系统调用后，同一个端口只有在旧操作和关闭都完成后才允许重用；不能释放时明确要求重新插拔。

线上使用自托管的官方 esptool-js 0.7.0；没有更换 SDK、固件或取消敏感分区确认。首次本地测试执行 `python3 vendor_sdk.py` 安装固定库，再运行 `npm test`、`python3 tests/vendor_test.py` 和 `python3 tests/browser_test.py`。浏览器测试需 Playwright/Chromium；CI 会从锁定的既有 Pages 提交获取并核对 SDK Git blob。

完整主机回归不等于已验证实体 USB、DTR/RTS、电源或 Flash 写入；没有关闭证书检查或使用虚构分区来让连接通过。
