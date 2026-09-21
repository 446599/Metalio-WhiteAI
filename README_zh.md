# Metalio AI 墨水屏首页

面向 **Metalio E-Ink 4**（ESP32-S3、GDEM0397T81 / SSD1677）的 AI 墨水屏固件。
设备上电后直接进入低刷新、易阅读的首页，绘制完全走 raw framebuffer 的
1-bit 路径，当前目标不会编译或链接 LVGL。

| 项目 | 说明 |
|------|------|
| 工程名 | `metalio-hw-test` |
| 芯片 | ESP32-S3 |
| 开发板 | Metalio E-Ink 4 |
| 屏幕 | 800×480，SSD1677（逻辑 UI 为 480×800 竖向坐标） |
| 触摸 | CST816S |
| 已验证环境 | ESP-IDF 6.0.1（目标版本 5.5.2 及以上） |
| 应用分区地址 | `0x80000` |

English: [README.md](README.md)。产品信息架构与后续路线见
[docs/AI_PRODUCT_PLAN.md](docs/AI_PRODUCT_PLAN.md)。

## 界面与首页

当前产品界面采用 Nothing 风格的黑白极简排版：32 px 统一边距、静态点阵大时钟、
四格常用应用、小圆形选中编号以及简洁列表。点阵数字直接由几何圆点绘制，
仅随分钟变化刷新；中文沿用固件内置位图字体。

首页保留时钟、日期、网络电量，以及闹钟、日历、录音、小智四格入口。
底部将“应用目录”和“设备选项”分开；天气、额度和同步状态集中到设备状态页。
闹钟列表支持开关与分页；月历可翻月、选日期查看日程，通过语音添加闹钟或日程。

录音页同时提供本地录音回放和新建语音文字笔记。本地录音最长 30 秒，保存最近一段：
SD 可用时写入 `/sdcard/recordings/latest.wav` 并保留恢复副本，重启后可回放；
没有 SD 存档时明确显示仅本次开机保留。来闹钟和离开页面会停止录音/回放。
“新建语音文字笔记”进入原文视图，按住 AI 键说话、松开识别，文字沿用胶囊自动保存。
这两种模式独立，目前不支持上传已经录好的 WAV 文件转写。

CST816S 支持内容区点击；盖板 `HOME / PREV / NEXT` 分别回首页、返回/上一页、
下一项/下一页，日历页 PREV/NEXT 翻月。BOOT（AI）键按住说话、松开发送。
首页平时隐藏底部提示，通知临时显示在该区域。音量键继续只控制音量。

闪念胶囊已支持识别原文自动保存、AI 回答完成后补存，以及打开最近一条胶囊。
保存状态按内容版本确认，写入失败会提示。阅读、卡片详情和任务确认仍有演示
流程，尚未实现真实 SD 书库、多条胶囊历史或任务同步。

运行 `python3 tools/check_ui_contract.py` 检查几何；使用装有 Pillow 的 Python
运行 `python3 tools/render_ui_preview.py`，可直接执行固件绘制函数生成所有页面
预览。结果位于 `build/ui-preview/`，不需要连接开发板。详细命中坐标和验证边界见
[UI QA 契约](docs/UI_QA_CONTRACT.md)。

## 转化好的位图字体

`main/display/font/ai_ui_assets.c/.h` 是按工作区另一个 EegoRead 项目的
格式转换并随固件编译的字体资产：码点表排序，字形按行存储，使用
MSB-first 2bpp 覆盖率。raw renderer 在写入 SSD1677 的 1-bit framebuffer 时
做阈值化，因此设备端不需要运行时字体栅格器，也不依赖 LVGL。
`generate_assets.py` 保留了基于 Pillow 的转换流程；烧录时只使用生成好的
C 文件，不需要 Python 或字体包。

仓库里原有的 LVGL 适配器和硬件测试页仍保留，方便后续板级诊断；当前产品
目标在 `main/CMakeLists.txt` 中只编译 raw 首页所需的源文件。

## 天气、额度与离线快照

`dashboard::DashboardService` 是独立 FreeRTOS 任务：默认请求 Open-Meteo
北京接口，也支持配置额度 JSON 接口；所有响应按 16 KiB 上限分块读取，成功
后把最近一次天气和额度保存到 NVS，断网时设备状态页仍显示最近快照。日程、摘要和
自定义卡片同样可由 NVS 写入。

可用配置键：

```text
dashboard/weather_url              # 可选，默认 Open-Meteo 北京接口
dashboard/weather_loc              # NVS 键名遵守 15 字节上限
dashboard/quota_url                # 可选额度 JSON 接口
dashboard/quota_token              # 可选 Bearer token
dashboard/refresh_minutes          # 1..120，默认 10 分钟
dashboard/custom0_* / custom1_*    # title、value、detail、enabled
dashboard/schedule0_time/title、dashboard/sched0_detail .. sched2_detail
dashboard/summary0 .. summary2
xiaozhi/enabled                    # 默认 true
xiaozhi/version                    # 二进制协议版本，默认 1（裸 Opus）
xiaozhi/ota_url                    # 激活/OTA 端点，默认 api.tenclass.net/xiaozhi/ota/
xiaozhi/url                        # 默认 wss://api.tenclass.net/xiaozhi/v1/
xiaozhi/token                      # 写入部署用 token
```

`w_*` 和 `q_*` 是服务自动写入的快照键，清除它们即可恢复为离线默认内容。

## 小智移植范围

`xiaozhi::Client` 已接入 Xiaozhi v1 WebSocket 的 hello、session、listen、
abort 控制消息，并把 hello、STT、LLM、TTS、绑定和工具事件映射到 AI 卡片。
凭据只从 `xiaozhi` NVS 读取，不写入日志。

`xiaozhi::AudioSession` 负责音频方向，并按参考实现对上下行使用不同时钟：

设备必须先绑定：`xiaozhi::Activation` 在联网后调用官方 OTA 端点
`https://api.tenclass.net/xiaozhi/ota/`，把返回的 `activation.code` 显示在
小智应用的绑定页上（首页小智入口显示“请绑定设备”），并按 `websocket.url`/`token` 切换到
服务端下发的通道、用 `server_time` 校准首页时钟。绑定码在小智控制台
（`xiaozhi.me`）输入后，设备每 10 秒轮询 `<ota_url>/activate`，收到 200
即变为“小智已绑定”。未绑定时服务端不会识别语音，这也是之前一直收不到
STT/TTS 的原因。

二进制帧版本由 `xiaozhi/version` 决定，默认 1：按官方协议文档，版本 1 是
**裸 Opus**，版本 3 才带 `{type, reserved, payload_size_be16}` 包头，HTTP
头 `Protocol-Version` 必须与 hello 里的 `version` 一致。下行两种格式都能
识别。

- 上行固定用板载麦克风时钟（16 kHz / 60 ms），默认版本 1 发送裸 Opus；
  仅版本 3 使用 `{type, reserved, payload_size_be16}` 包头；
- 下行按服务端 hello 的采样率（本部署为 24 kHz）打开解码器，再用线性插值
  重采样回板载 16 kHz 扬声器时钟；
- 收到 `stt` 或 TTS 开始时自动停采集，避免把自己的扬声器声音回灌服务端。

点击 AI 区进入对话页；按住 BOOT（AI）键开始聆听，松开发送。取消未完成对话
会重建会话，连接完成后需重新按键，避免旧响应串入下一轮。编解码器只在对应方向
忙碌时打开，停止聆听或断线后会释放 Opus 堆和
已排队音频；采集/播放任务栈为 40 KiB / 24 KiB（libopus 实测需要约 25 KiB，
8 KiB 会栈溢出），队列 8 包约半秒，满了丢最旧一帧。

`XIAOZHI_STATS?` 返回收发计数、编解码错误、输入峰值、三个采样率、连接与
会话标志、任务栈余量和剩余堆；`AI_TEXT?` 返回 AI 卡当前的状态与三行文字，
因此不用拍照也能确认对话内容。

## 自然语言闹钟与日程

已接入小智设备侧 MCP：可以说“五分钟后提醒喝水”“每个工作日八点提醒出门”，
也可查询、修改、取消本机日程。设置保存在 NVS，重启后恢复；到点进入独立闹钟页，
播放默认“轻铃”并振动。触摸“停止闹钟”或“稍后 5 分钟”处理，普通按键不会取消。
稍后提醒也会持久保存，不改变重复闹钟原来的时间。目前不支持关机唤醒和手机日历同步。

最多 16 项，北京时间；首页显示下一闹钟和安排数量，闹钟每页四项，日历按日期查看；只在工具返回成功后算设置完成。
旧 `dashboard/schedule*` 静态展示值由本地提醒列表替代。
工具字段、重复规则和串口验证见 [本地提醒 MCP 说明](docs/REMINDERS_MCP.md)。

## 编译

```bash
export IDF_PATH=/Users/henry/.espressif/v6.0.1/esp-idf
export IDF_PYTHON_ENV_PATH=/Users/henry/.espressif/python_env/idf6.0_py3.14_env
idf.py set-target esp32s3
idf.py build
```

生成镜像为 `build/metalio-hw-test.bin`。当前镜像小于 5 MiB OTA 应用槽。

## 烧录与串口

应用更新保留 bootloader、分区表和 NVS，只写应用地址 `0x80000`：

```bash
esptool.py --chip esp32s3 --port /dev/cu.usbmodem21101 --baud 921600 \
  write-flash 0x80000 build/metalio-hw-test.bin
```

这个 `0x80000` 是本仓库分区表（`partitions/v1/16m.csv`）的应用槽。工作区里
的 EegoRead/A4 固件使用不同布局（应用在 `0x10000`），因此换板前先读
`0x8000` 处的分区表确认；布局不一致时要连 bootloader 和分区表一起烧写，
只写应用会覆盖到 NVS 区域并且无法启动。

实际操作前先用 `esptool.py chip-id` 核对目标 MAC。串口日志可以证明
SSD1677、CST816S 和任务完成初始化；`Hash of data verified` 只证明 Flash
读回校验，不等于已经观察到屏幕像素或完成实体触摸验收，后两项需要直接看
板子。

## 板级诊断

原有电池、按键、马达、音频、蓝牙、Wi-Fi/4G、SD 等 HAL 与测试源仍在仓库中，
可用于单独的 bring-up 构建；产品启动路径使用上面的 raw AI 首页。

## 串口读屏与模拟触摸

USB Serial/JTAG 在 `/dev/secondary` 上提供一套诊断协议。主机可以读取当前
480×800 逻辑竖屏帧，或注入和真实触摸相同的点击/按下/移动/抬起事件；帧数据
带 CRC32，主机工具会校验后再写入 PBM 文件：

```bash
python3 tools/miaoink4_serial.py frame --out /tmp/home.pbm
python3 tools/miaoink4_serial.py tap 240 300
python3 tools/miaoink4_serial.py gesture 240 300 120 300
python3 tools/miaoink4_serial.py key SELECT CLICK
python3 tools/miaoink4_serial.py key HOME CLICK
```

协议命令还包括 `FRAME_PANEL?`、`STATE?`、`SCREEN_TEST?`、`HOME?` 和
`INPUT HELP?`、`XIAOZHI_STATS?`、`XIAOZHI_AUDIO_TEST?`。每个触摸注入命令
返回 `@@INPUT_ACK`；`KEY/BUTTON` 可注入 `HOME`、`PREV`、`NEXT` 和 `SELECT`
验证盖板触摸键和列表确认；`KEY AI DOWN/UP` 验证 BOOT 按住说话。音量键
只改变音量，其 NVS 写入和通知绘制已移到主事件任务，避免 esp_timer 栈溢出。串口打开
可能让 ESP32-S3 复位，因此连续手势必须使用工具的 `gesture` 子命令保持同一
串口会话。

音频自检不需要服务器：

- `XIAOZHI_AUDIO_TEST?` 先采集约 2 秒麦克风并做 Opus 编解码往返（不播放，
  避免啸叫），再合成 1 秒 1 kHz 正弦波经同一对编解码器播放；
- `XIAOZHI_DOWNLINK_TEST?` 把 24 kHz 正弦按 v3 包头打包后送进真正的下行
  入口，验证「剥包头 → 24 kHz 解码 → 重采样到 16 kHz → 播放」；
- `XIAOZHI_STATS?` 查看计数与资源，`AI_TEXT?` 查看 AI 卡文字，
  `WIFI_CONFIG?` 重新进入配网热点。

```bash
python3 tools/miaoink4_serial.py command XIAOZHI_STATS?
python3 tools/miaoink4_serial.py --timeout 20 command XIAOZHI_AUDIO_TEST?
python3 tools/miaoink4_serial.py --timeout 40 command XIAOZHI_DOWNLINK_TEST?
python3 tools/miaoink4_serial.py command 'AI_TEXT?'
python3 tools/miaoink4_serial.py command 'WIFI_CONFIG?'
```

实测数据（16 kHz / 60 ms，24 kbps）：麦克风 33/33 帧、每帧 180 字节、
编码 10.6 ms/帧、解码 1.7 ms/帧、音调 16/16 帧、下行 16/16 帧；任务栈
余量约 19 KB / 15.8 KB，释放编解码器后内部堆回到约 100 KB。

## 许可

除文件或第三方组件另有说明外，请遵循本仓库及其依赖的适用许可条款。

## 交付与回归

完整字体是独立 `font_data` 分区，标准 `idf.py flash` 现包含字体。既有设备
应用更新使用 `app-flash`，字体更新使用 `font-flash`；先核验设备分区表。详见
[构建交付](docs/BUILD_DELIVERY.md) 和 [2026-09-19 验证记录](docs/DEVICE_VALIDATION_20260919.md)。


## AI 系统工具与新版图标

现在通过小智 MCP 提供 21 个工具，支持系统状态、音量、应用切换、专注计时、SD 文字笔记和录音控制。界面增加 AI 笔记目录与分页阅读，使用 Lucide 图标，并对齐电池与百分比。使用示例、能力边界和验证见 [AI 系统工具](docs/AI_SYSTEM_MCP.md)。
