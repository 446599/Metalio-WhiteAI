# AI 墨水屏首页与小智路线

## 已交付的产品形态

Metalio E-Ink 4 的 800×480 SSD1677 面板使用 480×800 的逻辑竖向坐标。启动后
进入 raw framebuffer 首页，保持黑白、留白和低刷新节奏，采用 Nothing Phone
式的克制网格与大字号，信息层级如下：

1. 顶部品牌、网络状态和电量；
2. 几何圆点组成的大时钟与日期/天气信息；
3. 白底细线圆角 AI 今日重点；
4. 线性任务、阅读、应用入口与 Codex 额度。

产品页不再使用屏幕底部 2×2 导航按钮。盖板触摸键 `HOME/PREV/NEXT`
分别负责回首页、返回/上一页、下一项/下一页，BOOT（AI）实体键按住说话、松开发送；音量键只
调整音频音量。底部只保留一行被动提示，避免把导航控件做成视觉卡片。

绘制线程只消费 `dashboard::Snapshot`，网络和 Xiaozhi 回调只更新固定大小
数据模型。这样断网、服务端延迟或文本过长都不会把网络解析带进显示锁。

首页及其余 13 个产品页面统一为 32 px 边距，选中项以小圆形反白编号表示。
天气未就绪时显示待更新，长内容测量字形宽度后换行或省略。源代码渲染预览和
坐标契约见 [UI_QA_CONTRACT.md](UI_QA_CONTRACT.md)。阅读、卡片详情和任务确认仍含演示流程；
闪念胶囊已增加原文与最终回答的本地保存、恢复和保存版本状态，尚无多条历史或外部同步。

## 数据与配置

`DashboardService` 默认使用 Open-Meteo 北京接口，也支持通过 `dashboard`
NVS 命名空间替换天气地址、地点、额度地址和 Bearer token。响应按 16 KiB
上限分块读取；成功数据写入 `w_*` / `q_*`，下次启动
先显示最近快照。日程、摘要和自定义卡片使用同一命名空间的固定键写入。

没有可用额度数据时，首页显示 `--`，不会生成看似真实的百分比。

## 位图字体方案

字体资产沿用工作区 EegoRead 的转换格式：排序码点、字形偏移表、MSB-first
2bpp 覆盖率。生成好的 `main/display/font/ai_ui_assets.c` 随固件编译，设备
只做阈值化写入 SSD1677 的 1-bit 缓冲区；产品目标不编译或链接 LVGL。

## 小智接入边界

### 设备激活（绑定）——链路必须先过这一环

未绑定的设备可以连上 WebSocket，但服务端不会识别语音、也不会回话，所以
`xiaozhi::Activation` 按官方 OTA 协议补上了绑定流程：

- `POST https://api.tenclass.net/xiaozhi/ota/`（可用 NVS `xiaozhi/ota_url`
  覆盖）带上 `Activation-Version`/`Device-Id`/`Client-Id` 和板级系统信息；
- 响应里的 `activation.code` 就是要用户输入的绑定码，`activation.message`
  给出绑定站点（本部署为 `xiaozhi.me`），`activation.challenge` 用于轮询；
- 响应里的 `websocket.url` / `websocket.token` 会写入 `xiaozhi` 命名空间，
  transport 随即可用服务端下发的通道，不再依赖编译期默认地址；
- 响应里的 `server_time` 直接写入系统时间，首页时钟因此第一次有了准确时间；
- 之后每 10 秒 `POST <ota_url>/activate`（`202` 表示仍在等待，`200` 表示
  绑定完成），绑定成功后首页状态变为“小智已绑定”。

实测这台设备（无烧录序列号）调用 `activate` 会返回 **400**，服务端只接受
带序列号/HMAC 的轮询体；因此加了回退：轮询被拒时改为每 10 秒重新拉一次配置，
服务端在绑定完成后不再下发 `activation.code`，据此判定绑定成功。设备已用
这种方式成功识别为“小智已绑定”。

绑定码显示在首页 AI 卡上（状态行“请绑定设备”），同时可用 `BIND?` 从串口
读取，避免必须给屏幕拍照。

`xiaozhi::Client` 已完成：

- v1 WebSocket headers 与 hello 握手；
- session 管理、listen start/stop、abort 控制消息；
- hello、STT、LLM、TTS、goodbye、activation、MCP/IOT 事件到首页摘要的映射；
- NVS 凭据读取、重连、心跳和不输出 token/会话内容的日志策略。

音频方向由 `xiaozhi::AudioSession` 承担，并按参考实现的协议语义区分两个方向：

- 上行固定用板载麦克风时钟（16 kHz），60 ms 一帧，编码后默认发送 v1 裸 Opus；
  配置为版本 3 时才加入 `{type, reserved, payload_size_be16}` 包头；
- 下行按服务端 hello 的采样率（本部署为 24 kHz）打开解码器，再用线性插值
  重采样到板载 16 kHz 扬声器时钟；
- 服务端识别出语音（stt 事件）或开始 TTS 时自动停采集，避免把扬声器声音
  回灌给服务端；断线时清空队列并释放编解码器。

采集/播放任务栈为 40 KiB / 24 KiB（libopus 的栈需求实测约 25 KiB，8 KiB
会直接栈溢出），队列 8 包约半秒，满时丢最旧一帧。

当前证据边界：主机侧构建、UI/串口契约、以及真机上的麦克风采集与上行发送
（131 帧连续发送、每帧约 180 字节）、TTS 下行链路（v3 包头解析 → 24 kHz
解码 → 重采样到 16 kHz 播放，16/16 帧）都已实测；**服务端真实一轮
说出 → STT → LLM → TTS 仍未验证**，需要对着设备说话才能确认。

## 触摸与省电

CST816S 轮询任务处理内容区短按和盖板按键：AI Focus 打开对话页，BOOT 按住说话，列表项
打开页面，`HOME/PREV/NEXT` 路由产品页导航，省电画面任意键唤醒。盖板键在
本板上报告 y=900，和 480×800 framebuffer 命中区分离。`SetPowerSaveMode`
在显示锁外调用板级无线省电接口，避免网络回调与 framebuffer 锁互相等待。

## 后续阶段

### 阶段 A：音频会话（本地链路已验收，待服务端对话验证）

- [x] 选定 `espressif/esp_audio_codec` 的 Opus 编解码器，编码缓冲和任务栈有界；
- [x] 上行按板载时钟读取 HAL PCM，按配置版本发送二进制 WebSocket 帧；
- [x] 下行剥包头、按 hello 采样率解码、重采样回扬声器时钟，处理停止/断线清理；
- [x] 真机 `XIAOZHI_AUDIO_TEST?` 与 `XIAOZHI_DOWNLINK_TEST?` 通过，栈余量 19 KB / 15.8 KB；
- [ ] 用服务端测试账号验证一轮 说出 → STT → LLM → TTS → 播放 的时序。

### 阶段 B：真实数据源

- 接入日历/待办同步，并在本地排序和去重；
- 为 Codex 额度适配实际网关字段，显示更新时间与过期状态；
- 增加配置页或配网脚本，避免手工写 NVS。

### 阶段 C：墨水屏体验

- 记录不同 SSD1677 波形下的残影和刷新耗时；
- 只在快照变化或分钟切换时刷新，继续沿用整屏历史缓冲；
- 用实机相机和触摸事件记录完成光学/交互验收。
