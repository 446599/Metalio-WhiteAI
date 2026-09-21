# AI 操作本机系统

设备沿用小智 WebSocket 中的 MCP JSON-RPC 通道，在原来 6 个时间/提醒工具上新增 15 个工具，共 21 个，分 7 页发现，每页 3 个。无需另装手机插件或新增密钥。自然语言理解和工具选择由连接的小智服务完成；固件执行实际操作并返回结果。

## 可以怎么用

| 应用 | 示例 | 实际使用的能力 |
|---|---|---|
| 设备管家 | “音量设成 30”“打开录音”“还剩多少电” | 设备状态、音量、应用切换 |
| 专注计时 | “开始 25 分钟番茄钟” | 持久化本地闹钟，到时轻铃提醒 |
| 购物与工作备忘 | “保存一份购物清单：牛奶、面包”“在刚才的清单加上水果” | 笔记目录、读写和删除，屏幕分页查看 |
| 学习摘记 | “把这段话整理成学习笔记并保存” | 小智生成文字后调用笔记保存工具 |
| 晨间简报 | “今天有哪些安排，天气怎样” | 组合查询时钟、真实日程和已有天气数据 |
| 今日计划 | “查看我的备忘和日程，整理今天的计划并保存” | 组合读取日程/笔记并保存文字；提醒需另行创建 |
| 录音助手 | “开始录音”“停止录音”“播放最近录音” | 在音频空闲后控制本地录音或回放 |
| 日历导航 | “打开下周一的日历” | 先查本机日期，再打开指定年月日 |

## 工具目录

| 工具 | 参数及作用 |
|---|---|
| `self.clock.get_time` | 北京时间、星期、时钟有效性 |
| `self.reminders.list` | 已保存闹钟与日程 |
| `self.reminders.create` | `kind,title,when` 或 `after_seconds`，支持重复规则 |
| `self.reminders.update` | `id` 及需更新字段 |
| `self.reminders.delete` | `id` |
| `self.reminders.stop` | 明确停止正在响铃的提醒 |
| `self.device.get_status` | 电量、充电、音量、页面、网络、SD、闹钟、录音状态；不返回凭据 |
| `self.device.set_volume` | `volume: 0..100` |
| `self.display.open` | `app: home/alarm/calendar/recorder/assistant/voice_note/notes/capsules/reader/apps/device/status` |
| `self.display.calendar` | `date: YYYY-MM-DD`，当前月份前后 10 年 |
| `self.dashboard.get` | 已获取天气、额度、更新时间和缓存/过期状态 |
| `self.dashboard.refresh` | 请求后台更新，之后查询结果 |
| `self.recorder.get_status` | 录音状态、时长、是否有片段、SD 保存结果 |
| `self.recorder.control` | `action: start/stop/play` |
| `self.system.action_status` | 查询异步操作 `id` 的执行结果 |
| `self.system.cancel_action` | 取消尚未开始的操作 `id` |
| `self.notes.list` | 仅目录、ID、更新时间 |
| `self.notes.read` | 按 `id` 读全文 |
| `self.notes.save` | `title,text`；提供 `id` 更新，不提供则新建 |
| `self.notes.delete` | 按 `id` 删除 |
| `self.focus.start` | `minutes: 1..180`，默认 25；可提供 `title`，返回 `reminder_id` |

## 执行和存储约定

- 页面、音量、开始录音/回放返回 `queued` 和操作 ID。用新 JSON-RPC 请求查询 `action_status`，仅 `succeeded` 表示已执行；还可能返回 `failed/cancelled/expired`。队列最多保留 8 条记录，未开始操作 30 秒过期。
- 录音/回放等小智回答结束、音频空闲至少 2 秒后再开始。录音不上传，最长 30 秒，替换最近一段；回放和开始录音可取消排队，已开始则调用 `stop`。`succeeded` 表示已启动，文件保存状态另查 `recorder.get_status`。
- 本地录音遇到闹钟会停止并保存，闹钟优先。响铃期间不能用应用切换盖住提醒；普通按键不会取消闹钟。
- 文字笔记需要已挂载 SD 卡，最多 8 条；每条标题 96 字节、正文 1536 字节（UTF-8）。保存在 `/sdcard/notes/mcp.0`、`mcp.1` 两个轮换快照，带版本、序号和 CRC，写入同步完成才返回成功。开机选最新有效快照；若两份损坏则保留文件并禁用写入，不静默清空。未写入 16 KB 的共享 NVS，避免与闹钟和联网配置争空间。
- 闹钟/日程和专注计时仍用原有 NVS 持久化，断网开机可提醒，关机不自动唤醒。专注计时可通过 `reminders.delete` 取消。
- JSON 参数按工具白名单检查，不暴露任意文件路径、命令执行、刷写或清空配置。无 ID 的通知不执行写操作；当前连接中最近请求重试缓存受数量和字节数限制。
- 天气是设备已获取地点的当前数据，不是任意城市搜索或预报。晨间简报和学习笔记是已有工具的组合用法，不另建后台定时服务。
- 本地 WAV 转写、第三方日历同步、邮件和云端笔记同步尚未接入。`voice_note` 使用实时小智语音识别并保留最近胶囊；如需多条文字笔记，要明确让小智调用保存工具。阅读页仍是原有演示流程。

## 界面与图标

首页保持时钟、四个主要应用入口和两个独立目录。应用目录有闹钟、日历、录音、小智、AI 笔记、我的胶囊、阅读；设备状态、刷新数据、系统信息和屏幕测试保留在设备选项。

首页、目录、箭头与电池使用 [Lucide](https://github.com/lucide-icons/lucide) 官方 SVG 生成的 1-bit 位图。来源版本、原文件哈希、ISC/MIT 许可及生成方法见 `assets/icons/lucide/`。电池按可见像素与百分比数字对齐，充电时使用闪电图标；固件无需 SVG 运行库。

## 主机验证

```sh
python3 tools/check_system_tools.py
python3 tools/check_reminders_contract.py
python3 tools/check_serial_write.py
python3 tools/check_input_transport.py
python3 tools/check_ui_contract.py
python3 tools/render_ui_preview.py --out build/ui-system-preview
```

前两个运行实际 C++ Store/MCP 解析器，覆盖发现分页、参数错误、重复请求、保存失败回滚、重启恢复、专注到期和队列取消/过期；笔记文件测试覆盖新快照损坏时恢复前一份、两份损坏时拒绝覆盖。界面预览执行实际固件绘图函数，检查缺字与越界。
