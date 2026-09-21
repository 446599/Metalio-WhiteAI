# 小智本地闹钟与日程

固件在现有小智 WebSocket 中实现设备侧 MCP，用户说话后由小智模型选择工具，
设备负责校验、保存和到点提醒。无需另行部署 MCP 网关。
传输封装遵循[小智 MCP 协议](https://github.com/78/xiaozhi-esp32/blob/main/docs/mcp-protocol.md)：
`features.mcp=true`，`type=mcp`，`payload` 内为 JSON-RPC 2.0。

## 使用

按住 AI 键说话，松手发送。例如：

- “五分钟后提醒我喝水。”
- “每个工作日早上八点提醒我出门。”
- “明天下午三点安排项目会议，持续半小时。”
- “看看有哪些闹钟和日程。”
- “把项目会议改到明天下午四点。”
- “取消喝水提醒。”

设备首页和日程页显示最近三项启用的提醒，包含日期和时间。其余条目可以通过
语音查询。只有工具返回成功才代表已保存；单纯口头回答不构成创建成功。

到点进入独立闹钟页，显示大号时间、日期、标题，以及“停止闹钟”“稍后 5 分钟”两个
72 像素高的触摸按钮。HOME、SELECT、前后翻页、空白区域及按钮间隙均不会取消；
页面出现前的旧触摸也不能误关闹钟。可按住 AI 说“停止提醒”，AI 键本身不会关闭。

默认“轻铃”为本地合成的三个音符，按设备音量播放，并每两秒振动一次。录音和 AI
回答播放优先，暂时压住铃声，结束后仍未处理的闹钟继续响。铃声包含渐入、渐出，
与 TTS 共享唯一播放任务；录音任务栈在连接前预留，避免响铃时无法再申请连续内存。
持续三分钟后自动静音，闹钟页面仍保留，必须明确处理。

“稍后 5 分钟”先保存到 NVS，保存成功后才停止当前提醒；重启也保留稍后时间，
重复闹钟的原定时刻不变。同时到点的提醒一起停止或延后。
日程在开始时间提醒，`duration_minutes` 作为时长保存，不会自动产生结束提醒。

## 本地行为

- 时间采用北京时间（Asia/Shanghai）；系统 Unix 时间保持 UTC，显示和 RTC 使用本地时间。
- 未取得有效 RTC 或网络时间时拒绝新增/修改，避免误设日期。
- 最多保存 16 项（包括已停用/已触发条目），满后需要删除旧条目。
- NVS 保存完整快照；保存失败返回错误，内存中仍保留原值。
- 已保存提醒可离线运行并在重启后恢复。**需要设备保持开机，不支持关机唤醒。**
- 重启时最多补提醒最近五分钟内的到期项；更早的单次提醒过期，重复项前移到下次。
- 同一 MCP 会话中保留最近最多 8 个请求、合计 12 KiB 的重试结果；同一 ID 的相同请求
  在缓存内不会重复执行。重连后不保证相对倒计时的跨会话去重，查询列表后再决定是否重试。
- 标题限制为 96 UTF-8 字节，工具 schema 建议最多 32 字；界面按空间截断显示。
- 这版只管理本设备，未接入手机日历、云日历或跨设备同步。

## 工具

| 工具 | 用途 |
| --- | --- |
| `self.clock.get_time` | 查询北京时间、星期、时间与存储是否有效 |
| `self.reminders.list` | 查询全部条目和 ID |
| `self.reminders.create` | 创建闹钟、倒计时、日程 |
| `self.reminders.update` | 按 ID 修改、启用、停用 |
| `self.reminders.delete` | 按 ID 删除 |
| `self.reminders.stop` | 停止当前提醒 |

`tools/list` 每页 3 项，下一页 cursor 为 `3`。创建需给出 `kind`、`title`，以及
`when` 或 `after_seconds` 之一。`when` 格式为 `YYYY-MM-DD HH:MM[:SS]`，
`after_seconds` 为 1–604800 秒。重复规则为 `once/daily/weekdays/weekends/custom`，
custom 的 `weekdays` 是不重复的整数数组，1=周一，7=周日。

## 串口诊断

`REMINDER_STATE?` 查看存储、时钟、条目数、当前提醒 ID 和远端 MCP 调用计数。
`tone/tone_frames/tone_err` 表示铃声输出状态、帧数和错误计数。
`MCP {json}` 在 USB 本地执行同一套工具，使用独立重试缓存，不增加远端计数；
它可以修改本机条目，测试后应按返回 ID 清理测试数据。单行命令上限 767 字节。

```sh
python3 tools/miaoink4_serial.py command 'REMINDER_STATE?'
python3 tools/miaoink4_serial.py command 'MCP {"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
python3 tools/miaoink4_serial.py command 'MCP {"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}'
python3 tools/miaoink4_serial.py command 'MCP {"jsonrpc":"2.0","id":3,"method":"tools/list","params":{"cursor":"3"}}'
```

主机行为测试：`python3 tools/check_reminders_contract.py`、`python3 tools/check_reminder_chime.py`。
真实链路验收需要观察远端 `initialize/tools_list/calls` 计数，并核对请求后的条目，
不能把 USB 诊断工具成功当成服务器已调用成功。

设备实测和最终固件信息见 [2026-09-19 验证记录](REMINDERS_VALIDATION_20260919.md)。


后续本地录音集成说明：这里早期的“录音”优先级说明指小智说话采集。当前离线录音器遇到闹钟会停止录音并保存，闹钟优先；见 [UI 集成验证](UI_INTEGRATION_VALIDATION_20260919.md) 和 [AI 系统工具](AI_SYSTEM_MCP.md)。
