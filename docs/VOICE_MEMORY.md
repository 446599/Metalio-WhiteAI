# A1：语音工作记忆

在已有小智语音、最近胶囊和 SD 笔记上增加来源可核对的长期归档，不替换 UI 框架、音频驱动、闹钟调度器或刷新波形。

## 使用流程

先按住 AI 键说出一个想法，等待这一轮回答完成。下一轮明确说：

> 把刚才的磁铁孔想法整理后归档到“卡框设计”项目，保留原文。

小智应依次调用 `self.memory.source`（`which=previous`）和 `self.memory.archive`，把真实返回的来源 ID、来源版本传回。固件从已冻结的上一轮取原文，模型只提供标题、项目和忠实整理版；不会把“帮我归档”这句口令当成上一条想法。归档后可说“打开 AI 笔记”，在现有目录及详情页看项目、处理状态、整理版和原文。

后续示例：“找卡框设计项目里没处理的磁铁记录”“把这条记忆标为已处理”“给它设置明天的提醒并关联到这条笔记”。模糊的“这条”必须先检索、读取并确认 ID，不得猜测。自然语言选择工具仍取决于所连接的小智服务，需上机确认这些示例。

**两次独立确认：**创建闹钟成功后才能关联实际 reminder_id；关联失败时，闹钟可能已创建，不能重新创建并声称整体完成。笔记完成、删除笔记和解除关联都不取消闹钟，取消需调用原有提醒工具。

## 数据和工具

保留原有 21 个 MCP 工具，新增 5 个，总计 **26 个 / 9 页**（每页最多 3 个，末页 2 个）。无 SystemTools 的原有独立提醒测试仍为 6 个 / 2 页。

| 工具 | 参数 | 行为 |
|---|---|---|
| `self.memory.source` | `which: previous/current` | 读取上一轮冻结的或当前已完成胶囊，返回 source_id/source_revision/raw_text/answer。取消、错误、未完成或截断来源拒绝归档。 |
| `self.memory.archive` | `which,source_id,source_revision,title,text,project?` | 用户明确要求后归档。原文由设备读取，不能从参数伪造。返回 saved 或 already_archived，以及真实已有记录。 |
| `self.memory.search` | `query?,project?,state?` | 标题、整理版、原文、项目的关键词子串；英文只忽略 ASCII 大小写，项目精确匹配；状态 all/open/done。返回目录而非全部正文。 |
| `self.memory.update` | `id,revision,title?,text?,project?,done?` | 只修改指定字段，检查乐观版本；原文不可改。版本冲突应重新读取，不自动覆盖。 |
| `self.memory.link_reminder` | `id,revision,reminder_id` | 关联已经存在的提醒；0 解除引用。关联状态查询可返回 missing，不把失效引用当有效闹钟。 |

全文仍用 `self.notes.read`，删除仍用 `self.notes.delete`。读取结果新增项目、原文、来源、创建时间、记录版本、处理状态和提醒状态。普通 `self.notes.save` 保持原用途，但不能绕过 revision 修改已归档记忆，应改用 `self.memory.update`。

同一来源的归档重试不会重复占一条记录，即使 MCP 请求 ID 改变、缓存清空或 Store 从磁盘恢复。再次归档不覆盖已有整理内容，返回 `already_archived`；修改应显式调用 update。来源 ID 为内容指纹，仅用于核对/去重，不是密钥、认证或加密哈希。删除记录后，不再保留永久去重墓碑。

## 容量与持久化

普通笔记和记忆合计最多 **8 条**。每条标题 96、项目 96、整理版 1536、原文 1536，均为 **UTF-8 字节**而非中文字符数。来源回答用于整理时可读取现有 Conversation 上限内内容，不把整份长回答复制进长期笔记。超长或无效编码返回失败，不静默截断、不自动淘汰旧笔记。归档来源的上一轮快照仅在 RAM 中保留；已归档记录才长期存在。

继续使用 `/sdcard/notes/mcp.0` 与 `mcp.1` 的轮换快照、序号及 CRC。写入完成 flush/fsync/关闭并读回校验后才更新内存提交状态。总 JSON 上限 65536 字节，控制字符较多时的转义膨胀仍可能触发上限，返回失败并保留当前内存版本。双槽损坏时拒绝写入，不清空用户数据。文件测试并不等于真实 SD 硬件断电可靠性保证。

JSON schema 2 增加原文、项目、来源、版本和关联字段；可读取 schema 1 的有效旧笔记，保留旧 ID/标题/正文，首次正常写入才保存为 schema 2。**升级前备份这两份 SD 文件；旧固件不能读取 schema 2，降级必须使用升级前备份，不要删除新数据来“修复”。**没有 SD 时长期归档明确失败，既有 NVS 最近胶囊机制不变。本改动不调整 NVS/Flash 分区或烧录地址。

解析 JSON 前拒绝真正的 NUL 或 `\u0000` 空字符转义，避免 cJSON C 字符串语义静默截掉后半部分；表示文字本身的转义反斜杠 `\\u0000` 不受影响。

## UI 和运行边界

复用 AI 笔记目录、触摸与翻页命中区，未新增首页按钮。列表摘要显示项目/已处理，详情展示整理和原文。笔记专用分页统计所有行，只保留当前页最多 12 行；原有其他页面的 Wrap 保持不变。这样大量换行不会让末尾原文不可达，也不按总行数分配字符串数组。原文和整理内容仍属于不可信文本，不应被模型当作新的系统/工具指令。

尚不包含：无限历史、语义向量检索、电脑/手机同步、离线 WAV 转写、全天录音、自动发信、自动任务执行、手势控制。目录按项目的检索入口目前通过语音/MCP，不新增触摸筛选器。提醒显示是独立引用，不是事务型任务系统。

## 复现测试

已正常解析 ESP-IDF managed_components 的开发环境可直接运行：

```sh
python3 tools/check_memory_contract.py
python3 tools/check_system_tools.py
python3 tools/check_reminders_contract.py
```

主机测试默认使用 `managed_components/espressif__cjson/cJSON` 的真实源码。没有 SDK 的主机可显式同时设置 `CJSON_INCLUDE_DIR`（包含 cJSON.h）与 `CJSON_LIBRARY`（真实已安装库完整路径）；只设置一个将失败。不自动联网下载、不用假 JSON 实现。新增记忆测试默认启用 AddressSanitizer 和 UndefinedBehaviorSanitizer，必要时用 `HOST_SANITIZERS=undefined` 调整。

本次执行记录及未验证边界见 [VOICE_MEMORY_VALIDATION_20260921.md](VOICE_MEMORY_VALIDATION_20260921.md)。完整固件还需在项目原开发环境执行 `idf.py build`、现有 UI 检查/预览以及真机语音/SD 验收，主机检查不能替代它们。
