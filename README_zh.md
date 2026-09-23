# Metalio AI 墨水屏固件

面向 **Metalio E-Ink 4**（ESP32-S3 + 800×480 墨水屏）的产品固件。设备上电后直接进入
首页，通过 AI 语音键与小智完成对话、笔记归档、闹钟设置等操作。

绘制完全走 **raw 1-bit framebuffer** 路径，当前目标**不编译也不链接 LVGL**；
色彩只有黑与白，没有灰阶、没有动画过渡，刷新策略以"少闪、少残影"为优先。

English: [README.md](README.md)。二次开发请看 [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)。

| 项目 | 说明 |
|------|------|
| 工程名 | `metalio-hw-test` |
| 芯片 | ESP32-S3（QFN56，8 MB PSRAM，16 MB Flash） |
| 开发板 | Metalio E-Ink 4 |
| 屏幕 | 800×480，SSD1677（逻辑 UI 为 480×800 竖向坐标） |
| 触摸 | CST816S（电容，IRQ 模式） |
| 已验证环境 | ESP-IDF **6.0.1** |
| 应用分区地址 | `0x80000` |
| 字体分区地址 | `0xae4000`（独立分区，首次部署必须单独烧写） |

---

## 一、系统功能

### 1. 首页

顶部状态栏显示网络状态与电量百分比；中部是静态点阵大时钟与日期；下方四个常用入口
（**闹钟 / 日历 / 笔记 / 小智**）；底部固定**应用目录**与**设备选项**。

时钟按分钟刷新，只重绘变化的区域（`refresh=partial`），避免整屏闪烁。
首页平时不显示底部提示条，只有通知出现时临时占用该区域。

### 2. 应用目录

共 **6 项**：闹钟、日历、录音、小智、AI 笔记、我的胶囊。
其中「录音」只在应用目录里，首页那一格换成了「笔记」。

### 3. AI 语音对话（小智）

- 按住机身 **AI 键（BOOT / GPIO0）** 说话，松开后发送
- 服务端返回识别文本、思考状态、语音回答；回答通过扬声器播放
- 支持**继续对话**、**文字提问**（用内置拼音键盘输入）
- 每一轮结束后原文与回答自动进入**对话历史**（见第 6 节）

整轮有界：等待 hello 10 秒、松手后等待识别 20 秒、无有效进展 45 秒、整轮 180 秒。
超时会明确报错并重置传输，**不会无限卡在"正在识别"**。

### 4. AI 笔记

对某一轮对话做二次加工，四个快捷操作（回答完成后出现在 AI 页）：

| 操作 | 行为 |
|------|------|
| 整理灵感 | 把原文整理成通顺的条目 |
| 待办草稿 | 提炼成待办清单（**只生成草稿，不会自动创建提醒**） |
| 翻译英文 | 把原文翻译成英文 |
| 存为笔记 | 直接保存为笔记，保留原文 |

笔记**最多 8 条**，每条标题 96 / 正文 1536 UTF-8 字节，按字节限制、不截断多字节字符。
支持新建、搜索、编辑；离开有未保存草稿时需要确认。保存走独立任务，
提交成功才进详情页，失败会留在编辑页保留草稿。

> 注意：整理/待办/翻译依赖云端模型**主动调用设备的 `self.chat.get_task` 工具**读取原文。
> 若所连服务端的模型不按工具说明调用，这三项会明确报错，而不是伪装成功。

### 5. 闪念胶囊

按住 AI 键说话，**识别原文自动保存**；AI 回答完成后补存回答。
胶囊保存在 NVS，用于"最近说过什么"这类快速回看，与笔记、对话历史相互独立。

### 6. 对话历史

每一轮问答自动落盘到 SD 卡：

- 上限 **64 个会话**，每个会话 **128 轮**；满 128 轮后下一轮新建会话，
  总会话满则提示删除，**不静默淘汰历史**
- 每轮原文 1536 / 回答 6144 UTF-8 字节上限，超长显示截断标记
- 支持**新建 / 打开 / 继续 / 删除**会话；每轮记录保留原始来源
- 待写队列至多 8 轮，SD 不可用时暂存重试，队列满则拒绝开始下一轮，避免悄悄丢内容

### 7. 闹钟与日程

- 支持一次性与重复（每天 / 工作日 / 周末 / 自定义星期）
- 到点弹出全屏提醒界面，播放铃声并振动
- 支持稍后提醒（snooze），重复提醒的原始时间不会被 snooze 改写
- 月历可翻月、选日期查看当天日程
- 可通过语音或 MCP 工具创建、修改、删除、停止

铃声路径与语音播报共用同一个播放任务；两者都验证过实际出声。

### 8. 天气

通过 Open-Meteo 查询**指定城市**的当前天气（城市名搜索，不预设北京等固定位置）。
成功后写入带来源绑定的本地缓存；查询失败会保留上一次有效数据并标注新鲜度，
**不会用过期数据冒充最新**。语音回合进行中会推迟天气等可选 HTTP 请求，避免抢占带宽。

### 9. 录音

本地录音最长 30 秒，只保留最近一段。SD 可用时写入 `/sdcard/recordings/latest.wav`
并保留恢复副本，重启后可回放；没有 SD 存档时明确提示"仅本次开机保留"。
来闹钟或离开页面会停止录音与回放。

### 10. Wi-Fi 配网

设备选项 → Wi-Fi：

- 自动扫描附近 2.4 GHz 网络，按信号排序，按名称+安全类型去重，**最多 24 项、每页 5 项**
- 点击网络 → 输入密码 → 连接；隐藏网络可手动添加
- 仅支持开放网络与 WPA/WPA2/WPA3 Personal；WEP、企业网络显示为不支持
- **凭据只在目标 SSID 成功获取 IP 之后才提交保存**，失败或取消不会写入
- 4G 与 Wi-Fi 模式切换需要二次确认并重启

### 11. 本地输入（拼音键盘）

内置离线输入法，**不联网、不调用云端**：

- 支持中文拼音、英文大小写、数字、全部可打印 ASCII 符号、空格、退格、左右光标
- 中文基于固定 GB2312 字库范围的拼音表（413 音节 / 6763 字），输入完整拼音后点候选词，右侧翻页
- `v` 表示 `ü`，单引号作音节分隔
- 密码使用独立英文键盘，默认遮罩，可显式显示；**不发送给模型、不写日志**，退出或提交时清理编辑缓冲

### 12. 快捷控制

在内容区**下拉**唤出快捷面板，可调节音量、查看/切换网络、系统提示等。
面板与页面互斥：来提醒时优先显示提醒，快捷面板自动收起。

### 13. 锁屏与休眠

- **短按 POWER 键**锁屏/唤醒；长按仍是关机
- 空闲 **5 分钟**自动锁屏；锁屏界面可显示壁纸（默认或自定义 PBM）
- 进入受保护的**浅睡眠**：暂停 Wi-Fi、DMA、功放，有闹钟时按闹钟时间唤醒
- 录音、播放、提醒、笔记写入、SD 操作进行中不会休眠

### 14. MCP 工具（27 个 / 9 页）

小智服务端可通过 MCP 通道调用设备能力，覆盖闹钟日程、笔记、音量、应用跳转、
录音控制、天气、状态查询、对话任务读取等。**27 个工具分 9 页**返回，
异步操作有操作 ID 与明确完成状态（`queued` 只代表已排队）。

详见 [docs/AI_SYSTEM_MCP.md](docs/AI_SYSTEM_MCP.md)。

---

## 二、按键与操作

| 输入 | 行为 |
|------|------|
| 内容区点击 | 进入对应条目 / 按钮 |
| 内容区下拉 | 唤出快捷控制面板 |
| `HOME`（盖板键） | 回首页 |
| `PREV` | 返回上一级 / 上一页 |
| `NEXT` | 下一项 / 下一页（日历页翻月） |
| `AI`（BOOT，按住） | 说话；松开发送 |
| `POWER` 短按 | 锁屏 / 唤醒 |
| `POWER` 长按 | 关机 |
| 音量 +/− | 只调节音量 |

---

## 三、构建与烧录

### 环境

```sh
export IDF_PATH=/Users/henry/.espressif/v6.0.1/esp-idf
source /Users/henry/.espressif/v6.0.1/esp-idf/export.sh
```

### 构建

```sh
idf.py set-target esp32s3     # 首次
idf.py build
```

`idf.py build` 会自动完成三件事，任一失败就不应烧录：

1. 校验完整字库（45248 字形）并生成 `build/font_data.bin`
2. **内存预算守卫**：检查 IRAM、共享内部静态内存，并拒绝链接进蓝牙控制器入口
3. 校验镜像边界并写出 `build/delivery_manifest.json`（含各镜像 SHA-256）

### 首次部署（含分区表与字体）

```sh
idf.py -p /dev/cu.usbmodemXXXXX flash
```

会写 bootloader、分区表、otadata、应用与完整字库，**并重置 OTA 槽选择**。
不包含 NVS，不会清空用户数据。

### 在线烧录（不需要开发环境）

网页烧录器已托管在 GitHub Pages，浏览器直接打开即可用：

**https://446599.github.io/Metalio-WhiteAI/**

用桌面版 Chrome 或 Edge 打开（macOS 不要用 Safari），连接设备后在**发布页下载的固件包**里
选择对应的 `.bin`。网页通过 Web Serial 直接刷写，需要 HTTPS 或 localhost，
Pages 默认提供 HTTPS。刷机库已随页面本地化，**不依赖 CDN**。

> 网页会读取设备真实分区表并校验，**地址以设备读回结果为准**。
> 它不包含任何固件，需要你先从
> [Releases](https://github.com/446599/Metalio-WhiteAI/releases) 下载固件包。

### 已有设备更新（推荐）

先确认设备分区表与本次构建一致：

```sh
python3 -m esptool --chip esp32s3 --port <PORT> read-flash 0x8000 0x1000 /tmp/part.bin
python3 tools/check_build_delivery.py --device-partition-table /tmp/part.bin
```

然后按需选择：

```sh
idf.py -p <PORT> app-flash     # 只写应用 0x80000（字体未变时用这个）
idf.py -p <PORT> font-flash    # 只写字库 0xae4000
```

两者都不动分区表、OTA 选择与 NVS。完整说明见 [docs/BUILD_DELIVERY.md](docs/BUILD_DELIVERY.md)。

> **串口号每次插拔都可能变化**，不要硬编码。另外如果浏览器里的网页烧录器
> （Web Serial）还占着串口，命令行 esptool 会报 `Resource busy`，先关掉那个页面。

---

## 四、验证

### 主机检查（不需要设备）

```sh
python3 tools/check_ui_contract.py        # 页面几何与命中区
python3 tools/check_ai_contract.py        # AI 会话与保存
python3 tools/check_dashboard_contract.py # 天气与额度
python3 tools/check_reminders_contract.py # 闹钟与 MCP
python3 tools/check_system_tools.py       # 系统 MCP 工具
python3 tools/check_memory_contract.py    # 语音工作记忆
```

仓库共有 **24 个 `tools/check_*.py`**，覆盖 UI、AI、天气、闹钟、笔记、记忆、
输入法、Wi-Fi、录音、休眠、音频、镜像交付等。全部为纯主机运行，
不依赖硬件，也不联网（少数脚本需要 `managed_components` 里的 cJSON 源码）。

### 界面预览

```sh
python3 -m pip install Pillow          # 需要 Pillow
python3 tools/render_ui_preview.py --out build/ui-preview
```

用**真实的固件绘制代码与位图字体**渲染五组场景（normal / offline / empty / long / stale），
当前共 **549 帧**。这些是软件预览，**不能代替屏幕实拍**。

> 预览脚本需要 Pillow。如果系统默认 `python3` 没装，可用带 Pillow 的解释器运行，
> 例如 `python3.12 tools/render_ui_preview.py --out build/ui-preview`。

### 上机诊断（串口协议）

固件在 USB Serial/JTAG 上提供行协议，波特率 921600：

```sh
PORT=/dev/cu.usbmodemXXXXX

python3 tools/miaoink4_serial.py --port "$PORT" command 'FONT?'          # 字库是否就绪
python3 tools/miaoink4_serial.py --port "$PORT" command 'STATE?'         # 当前页面
python3 tools/miaoink4_serial.py --port "$PORT" command 'XIAOZHI_STATS?' # 音频/连接/内存
python3 tools/miaoink4_serial.py --port "$PORT" command 'CAPSULE_STATE?' # 会话状态机
python3 tools/miaoink4_serial.py --port "$PORT" frame --out /tmp/x.pbm   # 导出 framebuffer
python3 tools/miaoink4_serial.py --port "$PORT" tap 240 400              # 模拟触摸
python3 tools/miaoink4_serial.py --port "$PORT" command 'KEY AI DOWN'    # 模拟按住 AI 键
```

工具打开串口后会自动释放 DTR/RTS，避免把 ESP32-S3 按在 ROM 下载模式。
完整命令列表见 [docs/PROJECT_MAP.md](docs/PROJECT_MAP.md)。

---

## 五、已知边界

以下项目**尚未验证或有意不支持**，不要从其他测试通过外推：

- **光学效果未自动化验收**：墨水屏的残影、对比度、刷新观感必须人眼或拍照确认
- **扬声器音质未仪器测量**：只验证过"音频帧确实写出且无错误"，
  音量、爆音、长时间 TTS 稳定性未测
- **真实语音链路依赖服务端**：MCP 工具是否被调用取决于所连服务端的模型行为，
  固件无法强制；翻译/整理/待办三项尤其受影响
- **小智长连接会被服务端关闭**：实测约每 50~70 秒一次（对端 `close code=1005`），
  固件会自动重连，但会话上下文会重建
- **天气 API 实测记录有限**：公共 Open-Meteo 探测曾在 TLS 握手超时，
  未记为可用性成功
- **不支持**：LVGL、灰阶、第三方日历同步、WAV 文件转写、云端输入法

---

## 六、目录结构

```
main/
  application.*     启动编排、事件组、UI 工作队列
  display/          raw framebuffer 绘制、字体、图标、页面路由、快捷控制
  xiaozhi/          小智 WebSocket、MCP、会话状态机、音频会话
  chat/             对话历史持久化与源保留记录
  notes/            AI 笔记存储与保存任务
  reminders/        闹钟/日程存储与到期处理
  dashboard/        天气与额度
  network/          Wi-Fi 配网
  input/            离线拼音编辑器与键盘布局
  power/            锁屏与浅睡眠
  system/           设备控制、启动诊断、构建特性开关
  hal/              板级驱动（墨水屏、触摸、音频、电源等）
  apps/             历史硬件诊断应用，不参与产品启动路径
components/         项目本地 ESP-IDF 组件
tools/              主机检查、UI 预览、串口工具、资源生成
docs/               功能说明、验证记录、交付文档
partitions/v1/      16 MB 分区表
```

各目录职责与运行时链路详见 [docs/PROJECT_MAP.md](docs/PROJECT_MAP.md)。

---

## 七、相关文档

| 文档 | 内容 |
|------|------|
| [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) | **二次开发指南**（新增页面、工具、检查；内存与锁约定） |
| [docs/BUILD_DELIVERY.md](docs/BUILD_DELIVERY.md) | 构建、分区与烧录交付 |
| [docs/PROJECT_MAP.md](docs/PROJECT_MAP.md) | 目录职责与运行时主链路 |
| [docs/AI_SYSTEM_MCP.md](docs/AI_SYSTEM_MCP.md) | MCP 工具清单与调用约定 |
| [docs/CHAT_HISTORY.md](docs/CHAT_HISTORY.md) | 对话历史的容量与持久化 |
| [docs/INPUT_WIFI.md](docs/INPUT_WIFI.md) | 拼音输入与 Wi-Fi 配网 |
| [docs/VOICE_MEMORY.md](docs/VOICE_MEMORY.md) | 语音工作记忆（`self.memory.*`） |
| [docs/SLEEP_CONNECTION.md](docs/SLEEP_CONNECTION.md) | 锁屏、浅睡眠与连接加固 |
| [docs/UI_QA_CONTRACT.md](docs/UI_QA_CONTRACT.md) | 页面几何与命中区契约 |
