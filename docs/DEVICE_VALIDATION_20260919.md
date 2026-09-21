# 2026-09-19 设备验证

设备：Metalio E-Ink 4 / ESP32-S3，串口 `/dev/cu.usbmodem21101`。

## 音量键重启

旧固件串口抓到了明确的失败序列：

```text
AudioCodec: Set output volume to 60
MetalioEInk4Board: VOLUME_DOWN (P0.7): volume=60
***ERROR*** A stack overflow in task esp_timer has been detected.
Backtrace: ... |<-CORRUPTED
Rebooting...
```

音量键回调在 `esp_timer` 任务中直接写 NVS 并绘制墨水屏通知，超过该任务栈空间。
四个音量操作（加/减、短按/长按）现只向有界 UI 队列提交任务，实际操作在 8 KiB
的应用事件任务中执行。主机回归执行实际回调，并在模拟 timer 上下文中禁止音量修改
和显示绘制，验证延后执行以及 0–100 边界。

原始失败日志保存在本机 `/tmp/miaoink4-volume-live.log`。

## 构建与设备镜像

- ESP-IDF v6.0.1，项目版本 2.0.51，构建通过。
- 应用：2,936,368 字节，写入 `0x80000`，烧录哈希校验通过。
- 应用 SHA-256：`902ef5ee987bd0f699045cdb69f701e26ca7f312736b879f285dc88f124cb1af`。
- 字库：4,328,412 字节，写入 `0xae4000`，烧录哈希校验通过。
- 字库 SHA-256：`00fec4502eef536f2f7581b15269bf7b06f6440cca97791118bb70868e2ca3cb`。
- 已读取设备分区表并与构建分区表核对一致。
- 设备查询返回 `@@FONT_ACK ready=1 glyphs=45248`。

应用和字库分别更新，未写 NVS、bootloader 或分区表。
本机日志：`/tmp/miaoink4-build-final.log`、`/tmp/miaoink4-app-final-flash.log`、
`/tmp/miaoink4-font-flash.log`；交付清单为 `build/delivery_manifest.json`。

## 主机回归

已通过：输入队列与 WebSocket 分片、AI 会话隔离与胶囊持久化、仪表盘数据契约、
UI 契约、音量回调上下文、构建交付边界检查。UI 预览生成 normal、offline、empty、
long、stale 五种场景，共 175 帧。

## 实体回归边界

此前修复版空闲抓取未见崩溃，但也没有收到音量键事件，不能据此声称实体音量键已验收。
新的 110 秒抓取同样没有音量或语音事件，且没有崩溃；记录在
`/tmp/miaoink4-device-regression-20260919.log`。抓取期间 `ws=1 sess=1`，
语音收发帧为零；出现 TLS 读取失败日志，但尚无证据将其与爆音关联。

## 设备音频自检

修复固件在同一串口会话中连续执行两个诊断命令，均返回 `ok=1`：

```text
XIAOZHI_AUDIO_TEST: rate=16000 frame_ms=60 mic_frames=33 mic_fail=0
  mic_peak=1024 encoded=5940 enc_ms=352 dec_ms=56
  tone_frames=16 tone_fail=0 tone_skipped=0 stack_free=24492
XIAOZHI_DOWNLINK_TEST: server_rate=24000 frame_ms=60
  sent=16 enc_fail=0 received=16 decoded=16
```

测试后 `dropped=0 enc_err=0 dec_err=0 cap=0 play=0 enc=0 dec=0`，
播放任务栈余量为 15,848 字节，内部堆为 98,539 字节（与测试前一致）。
未观察到崩溃或重启。音频日志为 `/tmp/miaoink4-audio-regression-20260919.log`。
该测试覆盖本地采集、Opus 编解码、24 kHz → 16 kHz 重采样及 I2S 写入，
不等同于人工听音验收，也不包含真实服务器 TTS 往返。

说话键按下/松开时的爆音尚需与实际音频会话和日志对齐。已知重启会中断音频，但目前
证据不足以把全部爆音归因于音量键栈溢出。空闲时的零收发帧只表示没有语音流量，
不能证明扬声器音质正常。
