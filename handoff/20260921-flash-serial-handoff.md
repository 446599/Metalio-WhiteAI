# 交接：Metalio AI Ink Dashboard 烧录与串口诊断

日期：2026-09-21
接手对象：下一个排查者（无需本会话上下文，本文自包含）

## 后续更新（同日已解决，先读这一段）

本文正文记录的是问题**未解决时**的证据与假设。当天稍后已定位并解决，结论如下：

- 直接原因：**Microsoft Edge 持有 `/dev/cu.usbmodem21201`**（网页烧录器经 Web Serial 抓走串口），
  该进程从 9 月 16 日起一直存在。`idf.py app-flash` 曾报 `Resource busy`，`lsof` 可直接看到占用。
  网页烧录器会持续断言 DTR/RTS，把芯片按在 ROM 下载模式，因此应用永不启动、屏幕全白、串口零输出。
- 处置：关闭该网页后，用**全量 `idf.py flash`** 重写 bootloader / 分区表 / otadata / 应用 / 字体。
- 结果：`@@FONT_ACK ready=1 glyphs=45248`、`@@STATE mode=0 page=home`、`FRAME?` CRC 校验通过，
  首页 framebuffer 渲染正常。
- 因此本文第 10 节的假设 1–4 **均未成立**；假设 6（USB 链路/主机侧因素）方向正确。
- 同时落地了两项与本文第 7、9 节直接相关的修复，见提交说明：
  `main/system/boot_diag.{h,cc}`（早期诊断服务）与 `main/display/raw_display.cc` 的
  `/dev/secondary` RX 门控修复；另有 `IOExpander` 的 USB_MUX_SEL 映射与安全上电修正。

## 0. 一句话结论

编译成功、应用烧录成功且写入时哈希校验通过；但设备对串口指令**零响应**、硬复位后**零启动日志**，
随后设备从 USB 上**彻底掉线**。目前无法判定是「应用没跑起来」还是「应用在跑但串口通路断了」。

补充线索：设备挂在 `USB2.0 HUB` 后面，**短读（≤256 KB）成功、长读（≥1 MB）必失败**，
并出现过一次自愈式 USB 掉线。因此**USB 链路可靠性本身可疑**，接手时应先排除它。

两个判别动作，按成本排序：**① 摘掉 hub 直连并换线重测串口；② 看设备屏幕**（见第 11 节）。

## 1. 任务来源

用户要求：补 `main/xiaozhi/mcp_server.cc` 文件末尾缺失的换行符 → 编译一版固件 →
烧录到已连接设备并做上机测试。前两步已完成，第三步卡在上述现象。

## 2. 仓库状态

| 项 | 值 |
| --- | --- |
| 路径 | `/Users/henry/Documents/ChatGPT/miaoink4` |
| 分支 | `codex/ai-reader-home` |
| HEAD | `f4216eaeb116c05ce127319742104dd3a4d8805d` |
| remote | `origin https://github.com/446599/Metalio-WhiteAI.git`（私有） |
| 未提交改动 | 仅 `main/xiaozhi/mcp_server.cc`（补末尾换行，1 行） |

最近提交：

```text
f4216ea feat(memory): add source-preserving voice work memory
e4c3a78 feat: Metalio AI Ink Dashboard firmware with product UI, AI conversation, dashboard, reminders, notes, and system tools
38cd57f feat: add raw product UI navigation and safe refresh state
```

未提交 diff 内容（完整）：

```diff
@@ -210,4 +210,4 @@ std::string McpServer::Handle(const std::string& payload, int64_t now) {
     }
     return result;
 }
-}  // namespace xiaozhi
\ No newline at end of file
+}  // namespace xiaozhi
```

主机契约检查（改动前基线，全部通过）：

```text
UI contract OK
AI contract OK
Dashboard contract passed
Reminders/MCP OK
System MCP OK: 26 tools / 9 pages
Memory contract PASS: 504 checks; cJSON 1.7.19; real MCP 26 tools/9 pages
```

## 3. 构建

- ESP-IDF `v6.0.1`，target `esp32s3`，16 MB flash
- 命令：`source /Users/henry/.espressif/v6.0.1/esp-idf/export.sh && idf.py build`
- project `metalio-hw-test`，version `2.0.51`
- 构建输出：`Build delivery OK: 5 images; font 45248 glyphs`

产物（时间 2026-09-21 19:47）：

| 文件 | 偏移 | 字节 | SHA-256 |
| --- | --- | --- | --- |
| `build/bootloader/bootloader.bin` | `0x0` | 16640 | `48757cd0679047c08a5a3fb7d988fd8f3cdd37ebe85846dba9c0e5636e37a2d5` |
| `build/partition_table/partition-table.bin` | `0x8000` | 3072 | `b2ab8fbfdadaf0bcd91955cae6e646dcc92e698df9a364c7f9481334e5fa3113` |
| `build/ota_data_initial.bin` | `0xd000` | 8192 | `7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f` |
| `build/metalio-hw-test.bin` | `0x80000` | 3052016 | `5804c7869b2799e1e7de38eaf24ae1fc17feb670f69e737d51c1ad557fbcc8dc` |
| `build/font_data.bin` | `0xae4000` | 4328412 | `00fec4502eef536f2f7581b15269bf7b06f6440cca97791118bb70868e2ca3cb` |

分区余量：app 分区 5 MB 用 58%（剩 42%），font_data 5 MB 用 83%（剩 17%）。
交付清单：`build/delivery_manifest.json`（`schema_version: 1`，`flash_encryption_enabled: false`）。

## 4. 设备身份

| 项 | 值 |
| --- | --- |
| MAC | `10:20:ba:6e:08:08`（与 USB Serial Number 一致） |
| Chip | ESP32-S3 (QFN56) rev v0.2，Wi-Fi/BT5 LE，双核+LP 核 240MHz，8MB PSRAM (AP_3v3) |
| Crystal | 40MHz，USB mode USB-Serial/JTAG |
| USB | VID `0x303A` (12346) / PID `0x1001` (4097)，`USB JTAG/serial debug unit`，Vendor "Espressif" |
| 串口 | 本次为 `/dev/cu.usbmodem21201`（**注意：文档和 `tools/miaoink4_serial.py` 默认写的是 `21101`**） |
| 物理拓扑 | 经一个 `USB2.0 HUB`（VID `0x214B` / PID `0x7260`）接入 |

同环境历史上出现过另一块板 `68:EE:8F:5D:3D:A0`，本次不是它。

## 5. 烧录动作（已完成）

按 `docs/BUILD_DELIVERY.md` 的「已有设备更新」流程：

1. 读设备 `0x8000` 分区表 → `tools/check_build_delivery.py --device-partition-table`
   通过，说明设备分区布局与本工程一致。
2. 执行 app-only 烧录：

```sh
idf.py -p /dev/cu.usbmodem21201 app-flash
```

```text
Wrote 3052016 bytes (1710456 compressed) at 0x00080000 in 22.2 seconds (1098.3 kbit/s).
Verifying written data...
Hash of data verified.
Hard resetting via RTS pin...
Done
```

**没有**做完整 `flash`：bootloader、partition table、otadata、NVS 均未改动。

## 6. 设备侧读回校验

| 区域 | 结果 |
| --- | --- |
| bootloader `0x0` 长 `0x4100` | 与构建 SHA-256 完全一致 |
| partition table `0x8000` | 与构建一致（校验脚本通过） |
| font `0xae4000` 头部 32 B | `EFNT` magic、version 1、glyphs `0x0000b0c0`=45248，与构建逐字节一致 |
| otadata `0xd000` | `ota_seq=1`、`ota_state=2`(VALID) → 启动槽为 `ota_0`，正是写入位置 |
| app `0x80000` 全长读回 | **未完成**。四次尝试均被 esptool 中断，报 `Serial data stream stopped: Possible serial noise or corruption` |

结论：应用、字体、bootloader、分区表、启动槽五项全部指向「设备应当能启动本构建」，
唯一缺的是 app 的完整读回哈希（写入时哈希已验证，读回未复现）。

读回长度探测（同一端口、同一波特率，连续执行）：

```text
read 0x10000 (64 KB)   -> 成功
read 0x40000 (256 KB)  -> 成功
read 0x100000 (1 MB)   -> 失败：Serial data stream stopped
read 0x2e91f0 (3 MB)   -> 失败：Serial data stream stopped
```

即**短读稳定、长读必失败**，失败点落在 256 KB 与 1 MB 之间，与目标地址无关。
这更像 USB 链路/供电可靠性问题（设备还挂在 hub 后面），而不是 flash 内容问题。
含义：**不要指望在这条链路上做全镜像读回校验**；要完整校验需换直连口/换线，或改用 `md5sum` 之类的短事务。

## 7. 串口诊断（核心异常）

所有尝试**均返回 0 字节**，没有任何一条命令得到回复：

```text
python3 tools/miaoink4_serial.py --port /dev/cu.usbmodem21201 command 'FONT?'
  -> (未收到协议回复)

同一会话内重复 FONT? x6            -> 每次 0 bytes，端口保持存在
b'\n'            -> 0 bytes
b'STATE?\n'      -> 0 bytes
b'INPUT HELP?\n' -> 0 bytes
b'FONT?\r\n'     -> 0 bytes
b'help\n'        -> 0 bytes
```

硬复位后立即抓取原始串口（`--after hard-reset` 后马上 open + read 6 s）：

```text
=== boot chatter: 0 bytes ===
FONT? try 0: 0 bytes -> ''
FONT? try 1: 0 bytes -> ''
FONT? try 2: 0 bytes -> ''
```

其它观测：

- `lsof /dev/cu.usbmodem21201` → 无进程占用；无 `idf.py monitor` / `screen` / `picocom` 残留
- 连续 15 s 采样：JTAG 设备数恒为 1，端口恒定存在，**没有掉线重连** → 芯片未在崩溃重启
- `system_profiler SPUSBDataType` 在设备正常期间无输出（本机 USB 枚举信息需用 `ioreg` 看）

## 8. 时间线与掉线事件（重要）

1. 设备最初完全不可见（无 `/dev/cu.usbmodem*`，USB 树上无设备）
2. 用户重新连接后，先出现 `USB2.0 HUB`，随后 `USB JTAG/serial debug unit` 出现，端口 `21201`
3. `chip-id`、读分区表、`app-flash`、读 bootloader/字体头均成功
4. 串口指令与启动日志始终 0 字节（第 7 节）
5. 尝试以 `-b 460800` 分三段读回 app 时，三段**全部 FAILED**
6. 此后设备**从 USB 完全消失**：只剩 `USB2.0 HUB`，JTAG 设备数为 0，`/dev/cu.usbmodem*` 不存在
7. 持续观察 20 s 以上未恢复；`esptool chip-id` → `Found 0 serial ports`
8. 稍后设备**自行重新枚举**回 `/dev/cu.usbmodem21201`（JTAG 设备数恢复为 1），无需人工干预
9. 设备恢复后立即重测：`miaoink4_serial.py command 'FONT?'` 与原始 5 s 抓取**仍然 0 字节**

掉线发生在低波特率读回之后，但因果不明：可能是读回触发的异常，也可能是线材/供电/USB hub 的独立问题。
**注意掉线可自愈**；接手前先确认设备当前是否已枚举，并记录当时的端口号（可能变化）。

## 9. 相关代码路径（排查用）

| 位置 | 内容 |
| --- | --- |
| `main/main.cc` | `app_main`：`esp_event_loop_create_default` → `nvs_flash_init` → `Application::Start()`（无阻塞点） |
| `main/application.cc:63` | `Application::Start()`：`Hal::Init()` → 建 `main_event` 任务 → `RawDisplay::ShowProductHomeScreen()` → 启动 providers |
| `main/hal/hal.cc:112` | `Hal::Init()` 仅 `Board::GetInstance()`，真正初始化在板级构造函数 |
| `main/hal/metalio-e-ink-4/metalio_e_ink_4_board.cc:740` | 构造函数初始化顺序：I2c → IOExpander → VibrationMotor → Bq27220 → Pcf8563 → Cx25601n → BTAudio → **SdCard** → **Ssd1677** → Touch → **Display(RawDisplay)** → Buttons → SystemMonitor |
| `main/hal/metalio-e-ink-4/metalio_e_ink_4_board.cc:300` | `InitializeSsd1677()`：`esp_lcd_panel_reset/init` 均用 `ESP_ERROR_CHECK`，失败即 abort |
| `main/hal/metalio-e-ink-4/esp_lcd_panel_ssd1677.c:131,150` | BUSY 等待**有界**：普通 `15000 ms`，refresh 另有超时；超时返回错误 |
| `main/display/raw_display.cc:493` | `RawDisplay` 构造函数 |
| `main/display/raw_display.cc:529` | 创建 `frame_dump` 任务（`xTaskCreatePinnedToCore`，栈 8192，优先级 1，core 0） |
| `main/display/raw_display.cc:1108` | `FrameDumpTask()` 主循环 |
| `main/display/raw_display.cc:1233-1241` | 若驱动未装则 `usb_serial_jtag_driver_install`，**失败即 `return`（任务直接退出）** |
| `main/display/raw_display.cc:1244` | `ESP_LOGI(TAG, "serial input ready ...")` |
| `main/display/raw_display.cc:1247` | 回复写 `/dev/secondary`（`O_WRONLY|O_NONBLOCK`） |
| `main/display/raw_display.cc:1283` | `FONT?` → `@@FONT_ACK ready=%d glyphs=%lu` |
| `main/display/raw_display.cc:420` | `SerialWriteAll`，经 `usb_serial_jtag_write_bytes` 发送 |

控制台配置（`sdkconfig`）：

```text
CONFIG_ESP_CONSOLE_NONE=y
CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED=y
CONFIG_ESP_CONSOLE_UART_NUM=-1
CONFIG_ESP_CONSOLE_ROM_SERIAL_PORT_NUM=-1
CONFIG_USJ_ENABLE_USB_SERIAL_JTAG=y
```

注意：主控制台为 `NONE`，日志只可能走 secondary；ROM 串口关闭。
这解释了为什么看不到 ROM 启动信息，但**不能解释应用日志也为 0 字节**。

## 10. 假设（按当前可能性排序）

1. **应用未运行到 `RawDisplay` 构造之前**。
   但 SSD1677 的 BUSY 等待是有界的（15 s），超时会 `ESP_ERROR_CHECK` abort → 应当重启，
   而观测不到重启循环。除非卡在别的无界等待（如 SD 卡、I2C、IO expander）。**需要看屏幕判别。**
2. **应用在跑，但 `frame_dump` 任务提前退出**（`raw_display.cc:1236-1241` 驱动安装失败即 return）。
   与「零日志 + 零回复」都吻合。
3. **secondary console 与应用的 USB Serial/JTAG 通路冲突**：IDF 控制台可能已安装 VFS/驱动并消费输入，
   导致应用读不到命令；同时应用回复写 `/dev/secondary` 也可能被控制台接管。
4. 应用卡在 `Application::Start()` 之前的某个初始化（NVS/事件循环，可能性较低，失败会 abort）。
5. 第 8 节的掉线是独立事件（线材、供电、hub），与应用行为无关。
6. **USB 链路本身不可靠**（设备挂在 hub 后，长读必失败，且出现过自愈掉线）。
   零字节回复也可能部分归因于此。**建议优先排除链路因素**：换直连口、换数据线、换 hub 后重测串口。
   这一条成本最低，应最先做。

## 11. 下一步建议（按顺序）

1. **先排除 USB 链路**：把设备从 hub 上摘下来直连机身 USB 口，换一根确认能传数据的线，
   再重测串口与长读回。设备当前挂在 `USB2.0 HUB` 后面，且长读必失败、曾自愈掉线，
   链路嫌疑最大且验证成本最低。
2. **恢复连接**：确认设备重新上电、USB 枚举，取新的 `/dev/cu.usbmodemXXXXX` 端口号。
3. **看屏幕**——这是最快的功能判别手段：
   - 正常 AI 首页 → 假设 2/3，问题在串口通路
   - 全白/全黑/停在开机画面 → 假设 1/4，应用卡在启动
4. 若屏幕空白：按首次部署做完整 `flash`（含 bootloader + partition table + ota_data），
   排除任何残留 OTA/启动状态；`ota_data_initial` 会重置槽选择。
5. 若要看日志：临时把主控制台切到 USB Serial/JTAG（`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`）
   再 build + app-flash；**注意 `sdkconfig` 受版本控制，改完需还原**。
   或直接 `idf.py -p <PORT> monitor` 观察是否有 secondary 输出。
6. 若怀疑假设 3：检查 `usb_serial_jtag_is_driver_installed()` 与 IDF 控制台初始化顺序，
   考虑在装驱动前先 `usb_serial_jtag_vfs_use_driver()`，或改用 UART0 做诊断通道。
7. 若怀疑假设 1：在板级构造函数各步之间加日志/延时，或用 JTAG（该口支持 JTAG）定位卡点。

## 12. 待用户确认

- 设备屏幕现在显示什么（正常首页 / 空白 / 卡开机画面 / 花屏）
- 这块板子之前跑的是不是同一固件（若之前是硬件测试固件，需走完整 flash）
- 第 8 节的掉线是否与人为插拔有关

## 13. 证据文件

本次会话留在 `/tmp` 的读回样本（可能已被系统清理）：

```text
/tmp/miaoink4-partition.bin   设备 0x8000 分区表读回
/tmp/dev-bootloader.bin       设备 0x0 bootloader 读回（16640 B）
/tmp/font-hdr.bin             设备 0xae4000 字体头 32 B
/tmp/miaoink4-ota.bin         设备 0xd000 otadata 读回（8192 B）
```

构建侧权威清单：`build/delivery_manifest.json`。

## 14. 复现命令速查

```sh
export IDF_PATH=/Users/henry/.espressif/v6.0.1/esp-idf
source /Users/henry/.espressif/v6.0.1/esp-idf/export.sh

# 构建
idf.py build

# 设备枚举
ls /dev/cu.usbmodem*
ioreg -p IOUSB -w0

# 芯片身份
python -m esptool --chip esp32s3 --port <PORT> chip-id

# 分区表核对（更新前必做）
python -m esptool --chip esp32s3 --port <PORT> read-flash 0x8000 0x1000 /tmp/part.bin
python3 tools/check_build_delivery.py --device-partition-table /tmp/part.bin

# 应用更新（不改分区表/OTA/NVS）
idf.py -p <PORT> app-flash

# 串口协议验证（期望 @@FONT_ACK ready=1 glyphs=45248）
python3 tools/miaoink4_serial.py --port <PORT> command 'FONT?'
```
