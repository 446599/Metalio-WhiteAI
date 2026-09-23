# 二次开发指南

本文面向要在这个固件上**加功能、改界面、接工具**的人。读完应该能独立完成一次
"改代码 → 主机自检 → 构建 → 烧录 → 上机验证"的闭环。

配套阅读：[PROJECT_MAP.md](PROJECT_MAP.md)（运行时链路）、
[BUILD_DELIVERY.md](BUILD_DELIVERY.md)（烧录交付）、
[UI_QA_CONTRACT.md](UI_QA_CONTRACT.md)（几何契约）。

---

## 一、快速开始

### 1. 环境

```sh
export IDF_PATH=/Users/henry/.espressif/v6.0.1/esp-idf
source /Users/henry/.espressif/v6.0.1/esp-idf/export.sh

idf.py set-target esp32s3   # 仅首次
idf.py build
```

**必须用 `. ./export.sh` 的方式配环境**。只手工设 `IDF_PATH` /
`IDF_PYTHON_ENV_PATH` 会得到 `command not found: idf.py`。

### 2. 找到你的板子

```sh
ls /dev/cu.usbmodem*                 # 端口号每次插拔都可能变化，不要硬编码
ioreg -p IOUSB -l -w0 | grep -A2 'USB JTAG'
```

多块板同时接入时，用 `ioreg` 里的 **USB Serial Number**（等于芯片 MAC）区分，
不要靠端口号猜。

### 3. 改完之后的固定流程

```sh
# ① 先跑相关的主机检查（不需要设备，秒级反馈）
python3 tools/check_ui_contract.py

# ② 构建（会自动跑字库校验 + 内存预算守卫 + 镜像边界）
idf.py build

# ③ 只烧应用（字体未变时）
idf.py -p <PORT> app-flash

# ④ 上机确认
python3 tools/miaoink4_serial.py --port <PORT> command 'FONT?'
```

**不要跳过第 ② 步的内存守卫**——它会拒绝把返工烧到设备上。见第五节。

---

## 二、代码结构

### 启动链路

```
app_main                               main/main.cc
  ├─ boot_diag::Init()                 最早启动的串口诊断服务（失败也能问）
  ├─ nvs_flash_init()
  └─ Application::Start()              main/application.cc
       ├─ GetHAL().Init()              → Board 构造函数（板级外设全部在这里初始化）
       ├─ xTaskCreate("main_event")    事件任务：SCHEDULE / UI / AI_FOCUS / CLOCK_TICK
       ├─ RawDisplay::ShowProductHomeScreen()
       └─ Dashboard / Notes / Reminders / Xiaozhi 各自启动
```

**关键点**：`Board` 的构造函数里完成 I2C、IO 扩展器、电源、屏幕、触摸、按键初始化。
这些步骤大多用 `ESP_ERROR_CHECK`，**失败即 abort 重启**。所以"板子看起来没反应"时，
第一件事是抓启动日志看停在哪一步。

### 任务与锁

| 任务 | 职责 | 注意 |
|------|------|------|
| `main_event` | 事件分发、UI 工作队列、时钟 tick | 栈 8 KiB，**别在这上面做重活** |
| `raw_touch` | 触摸轮询与页面路由 | 栈 8 KiB |
| `frame_dump` | 串口命令与 framebuffer 导出 | 同时负责 USB 协议收发 |
| `xz_capture` / `xz_playback` | Opus 采集与播放 | **栈必须在内部 RAM**（见第五节） |
| 各服务的 worker | 笔记保存、SD 写入、天气查询等 | 通过队列/Revision 通知 UI |

**三条硬约定**：

1. **不在屏幕刷新锁内做网络、TLS、NVS 或长时间阻塞操作**
2. **输入回调只提交有界事件**；绘制和持久化放到所属任务里做
3. **跨线程共享的状态用 `Revision()`（原子计数）通知**，UI 侧比对 revision 决定是否重绘

`DisplayLockGuard lock(this)` 保护 framebuffer 与页面状态；
`ui_lock_depth` 在测试里用来断言"锁内没有副作用"。

### UI 队列

`main/ui_work_queue.h` 是一个定容（8 项）环形队列。
`Application::ScheduleUi(callback)` 投递，满则丢弃并计数。
**异步服务不要直接画屏幕**，应该 `ScheduleUi` 回主事件任务再画。

---

## 三、怎么加一个新页面

以新增 `ProductPage::Foo` 为例：

1. **加枚举**：`main/display/raw_display.h` 的 `enum class ProductPage`
2. **加绘制**：实现 `void RawDisplay::DrawProductFooLocked()`（**函数名必须以 `Locked` 结尾**，
   表示调用者已持有显示锁）
3. **接调度**：在 `DrawProductScreenLocked()` 的 `switch (product_page_)` 里加一个 `case`
4. **接触摸**：在 `HandleTap` 路由链里加 `HandleFooTap(x, y)`；命中判断统一用
   `input::Inside(x, y, left, top, w, h)`
5. **接按键**：在 `HandleKey` 路由链里加 `HandleFooKey(key)`
6. **接状态查询**：`main/display/system_view.cc` 的页面名映射（`STATE?` 会用到）
7. **接应用跳转**（若要通过 `self.display.open` 打开）：同一个文件的 `entries[]` 表
8. **更新几何契约**：改 `tools/check_ui_contract.py` 并在 `docs/UI_QA_CONTRACT.md` 记录
9. **跑预览**：`python3 tools/render_ui_preview.py --out build/ui-preview` 看渲染结果
   （需要 Pillow；没装就 `python3 -m pip install Pillow`，或换带 Pillow 的解释器）

**布局常量**集中在 `raw_display.cc` 顶部（`kUiInset`、`kUiBodyY`、`kUiRowPitch` 等），
新页面请复用这些常量而不是写死像素，否则几何契约检查会漂移。

---

## 四、怎么加一个 MCP 工具

MCP 工具表在 `main/xiaozhi/system_tools.cc`：

1. 在文件顶部的 `constexpr Spec specs[]` 里加一条
   `{"self.xxx.yyy", R"({...json schema...})"}`
   - `description` 要写清**什么时候该调用**，模型是照这句话决策的
   - schema 里用 `additionalProperties:false` 收紧参数，避免模型乱传
2. 在 `SystemTools::Handle()` 的分派链里加一个 `is("self.xxx.yyy")` 分支
   - 参数校验用现成的 `Keys()` / `Get()` / `Number()` / `Text()` 助手
   - **失败要返回明确错误**，不要静默成功
   - 异步操作请返回**操作 ID**，并让调用方用 `self.system.action_status` 查询
3. 更新 `tools/check_system_tools.py` 的断言（它会检查工具数量与分页）

**分页**：工具按每页 3 个返回（当前 27 个工具 / 9 页）。加工具后总数会变，
检查脚本里的期望值要同步。

### 诊断约定

需要排查时用 `XZ_META(...)`（`main/xiaozhi/metadata_log.h`）打**元数据**日志，
例如工具名、参数键、字节数、状态码：

```cpp
XZ_META("mcp call tool=%s ok=%d code=%d", name, ok, code);
```

**绝对不要记录**：用户语音原文、密码、token、书籍内容、私人对话。
现有的 `mcp_server.cc` 已经按这个原则记录了调用名与结果码。

---

## 五、内存预算（**最容易踩的坑**）

这块板子的内部 RAM 非常紧张。当前数字：

| 指标 | 当前 | 上限 | 余量 |
|------|------|------|------|
| IRAM | 91392 | 97280 | ~5.8 KB |
| 共享内部静态（IRAM+DRAM） | 148768 | 151552 | **~2.7 KB** |

`idf.py build` 会运行守卫并在超限时**直接失败**，报告写进 `build/audio_memory.json`。

### 为什么这么紧

DIRAM 是 **IRAM 代码段与 DRAM 堆共享**的同一块内存。曾经有一次回归：
新增的蓝牙扫描功能让链接器把预编译库 `libbtdm_app.a` 里带 `.iram1` 的目标文件
拉了进来，**17.5 KB 控制器代码进了 IRAM**，堆随之少 21 KB，
最大连续块从 30 KB 掉到 13.8 KB，**音频播放任务再也分配不到 24 KB 栈，扬声器彻底无声**。

所以守卫除了查容量，还会**拒绝链接进蓝牙控制器入口符号**。

### 音频任务栈必须在内部 RAM

`AudioCodec::Start()` 会读 NVS 里存的音量，而 **NVS 访问要禁用 flash cache**；
ESP-IDF 在这种情况下断言"当前任务栈必须在内部 RAM"。
把音频任务栈放到 PSRAM（哪怕配了 `MALLOC_CAP_SPIRAM`）会**直接崩**：

```
assert failed: esp_task_stack_is_sane_cache_disabled()
```

`main/xiaozhi/xiaozhi_audio.cc` 里的 `CreateAudioTask()` 就是为这件事存在的注释载体，
改音频任务时请保持现状。

### 判断"能不能再加东西"

上机后用串口看两个数字：

```sh
python3 tools/miaoink4_serial.py --port <PORT> command 'XIAOZHI_STATS?'
# largest=    最大连续内部块
# play_need=  播放任务需要（当前 24576）
```

或者看每秒系统监控：

```
I 系统监控: @@@内存  | 剩余: 48 KB | 历史最小: 35 KB | 最大连续: 27 KB
```

**「最大连续」才是决定音频任务能否启动的数字**，不是「剩余」。
只要 `largest > play_need` 就还有余地；两者接近时要非常小心。

---

## 六、怎么加一个主机检查

仓库用 `tools/check_*.py` 在**主机上**编译并运行真实固件源码（用桩替换硬件层），
这样不接设备也能快速回归。

现成范式（见 `tools/check_system_tools.py`）：

1. 脚本里内嵌一段 C++ 测试源码
2. 用 `cc` 编译 `managed_components/espressif__cjson/cJSON/cJSON.c`
   （cJSON 标志由 `tools/host_cjson.py` 统一提供）
3. 用 `c++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined`
   一起编译**真实源码 + 测试**
4. 运行生成的二进制，断言失败即非零退出

新增硬件依赖时，把桩放在 `tools/tests/<feature>_stubs/` 下，
用 `-I` 让它优先于 `main/` 被找到。

**写检查的原则**：测行为，不测实现。例如"提交失败时书签不得落盘"这类
回滚语义，比"函数被调用了"更有价值。

---

## 七、串口调试

固件在 USB Serial/JTAG（921600）上提供行协议：

### 只读查询

| 命令 | 返回 |
|------|------|
| `FONT?` | `@@FONT_ACK ready=1 glyphs=45248` |
| `STATE?` | 当前页面、测试模式、休眠等 |
| `XIAOZHI_STATS?` | 音频计数、连接状态、堆与栈水位、`largest`/`play_need` |
| `CAPSULE_STATE?` | 会话状态机 `state=` / `turn=` / 字节数 |
| `REMINDER_STATE?` | 闹钟存储、`tone_frames` / `tone_err` |
| `FRAME?` | 导出 480×800 framebuffer（带 CRC 校验） |

### 动作

| 命令 | 用途 |
|------|------|
| `TOUCH TAP x y` / `TOUCH CLICK x y` | 模拟触摸 |
| `TOUCH DOWN/MOVE/UP x y` | 模拟手势（需在同一会话内） |
| `KEY AI DOWN` / `KEY AI UP` | 模拟按住 AI 键说话 |
| `KEY POWER CLICK` | 模拟电源键 |
| `MCP {json}` | 本地执行 MCP 工具（改本机数据，测完请清理） |

### 两个坑

1. **打开串口可能让设备复位**（macOS 会切 DTR/RTS）。
   `tools/miaoink4_serial.py` 打开后会立刻释放 DTR/RTS，但底层枚举仍可能触发一次复位。
   做时序敏感测试时，**尽量在同一个会话内完成**，不要反复开关端口。
2. **触摸后不要立刻查 `STATE?`**。状态刷新有延迟，紧跟其后的查询可能读到旧页面。
   要确认界面变化，以 `FRAME?` 导出的图像为准，并留出 1~2 秒间隔。

---

## 八、日志

固件主控制台是 `CONFIG_ESP_CONSOLE_NONE`，日志走 **secondary（USB Serial/JTAG）**。

常用抓取方式（自动重连，避免复位导致丢日志）：

```python
# 打开端口 -> 释放 DTR/RTS -> 持续读 -> 断开则重连
```

要点：

- **`SSL receive failed: -76` 不是小智专属**——天气 HTTPS 请求也会打同样的错误码。
  统计断连次数时必须看下一行是不是 `Xiaozhi: websocket connected`
- `WebSocket: peer close code=1005` 表示**对端主动关闭**，与网络故障不同
- 每秒系统监控会打 CPU、内存、电池，是判断"设备是否还活着"的廉价信号

---

## 九、提交前检查清单

- [ ] `idf.py build` 通过，且 **`Audio memory budget OK`**
- [ ] 相关 `tools/check_*.py` 通过；改了 UI 几何要同步 `docs/UI_QA_CONTRACT.md`
- [ ] 改了页面/布局，跑过 `tools/render_ui_preview.py` 并肉眼看过截图
- [ ] 没有把 token、密码、语音原文、书籍内容写进日志或仓库
- [ ] 没有把 `build/`、`managed_components/`、预览产物加进版本控制
- [ ] **上机验证并明确区分证据层级**（见下）

### 证据层级：不要把主机通过当成上机通过

这个项目反复出现的一类误判，是把某一层的成功当成另一层：

| 层级 | 能证明 | 不能证明 |
|------|--------|----------|
| 主机检查通过 | 逻辑、边界、回滚语义正确 | 真机能否跑起来 |
| 构建通过 + 内存守卫 | 静态布局不超预算 | 运行时能否分配出连续块 |
| `Hash of data verified` | 写入的字节与镜像一致 | 固件真的启动、真的出声 |
| `playback task start` | 播放任务被创建 | **扬声器真的出声** |
| framebuffer CRC 正确 | 送给面板的数据正确 | **屏幕光学效果正常** |

**扬声器和墨水屏的最终验收必须由人在设备前确认**，主机侧数字再全也不能替代。

---

## 十、常见问题

**Q: `idf.py` 找不到？**
没有 `. ./export.sh`。

**Q: `Resource busy` 烧不进去？**
有别的进程占着串口。最常见的元凶是浏览器里的网页烧录器（Web Serial）。
`lsof /dev/cu.usbmodemXXXX` 看是谁。

**Q: 板子插上但 `/dev/cu.usbmodem*` 不存在？**
先确认不是充电线（无数据线芯）、不是坏 Hub；`ioreg -p IOUSB -w0` 看总线上有没有设备。
注意 ESP32-S3 的 USB 是芯片内置 PHY，**枚举不到不等于芯片没供电**。

**Q: 构建报内存超限？**
看 `build/audio_memory.json` 里 `iram_bytes` / `internal_static_bytes` 与 `limits` 的差。
新增的 `.iram1` 段代码、大的静态数组、或引入带 IRAM 段的预编译库都可能是原因。
用 `xtensa-esp32s3-elf-nm -S --size-sort` 对比 IRAM 段符号能定位到具体函数。

**Q: 设备不出声？**
按顺序查：`XIAOZHI_STATS?` 看 `largest` 是否大于 `play_need`；
日志里有没有 `playback task creation failed`；
再确认 `playback task start` 与 `tone_err=0`。
历史上"不出声"多次是**内部 RAM 碎片化导致播放任务创建失败**，不是扬声器坏了。
