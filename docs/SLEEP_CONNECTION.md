# 连接恢复、简约控制栏与锁屏浅睡眠

基线：c95bfef0ae7eba45d3f84d022e411bb8a4409d6d。用户附件为 WhiteAI-Xiaozhi-Translate-Disconnect-20260922.zip。本版保留音频内部栈、BLE 默认关闭、USB/上电修复、字库、分区与黑白刷新波形。

## 1. 附件证据与修复边界

附件确认 TLS 接收错误 -76、一次约 62980 ms 的断连间隔及随后重连；没有抓到完整真人语音翻译或 MCP get_task 调用日志。这个通用接收错误也会出现在天气 HTTPS 请求中，不能将所有 -76 都计为小智断线，不能仅凭一次间隔断定云端有 60 秒限制。翻译失效与断线是否同一根因仍待实机分离验证。

客户端已做以下修正，而不是声称云端问题已被证明修好：

- 空闲 WebSocket 断开不再把已完成回答改成 Error，保留原文、结果和归档/快捷操作入口；进行中断线保留已接收内容并记录错误，不自动重播麦克风或再次执行任务。
- 快捷操作仍用短事件和 self.chat.get_task，不将完整原文塞入 detect。短事件明确请求调用工具；冻结任务、分页、完整读取门控和 45 秒等待边界保留。
- 新增仅含元数据的 XiaozhiMeta 日志：操作编号、原文字节数、任务编号、读取偏移/字节数/complete、工具名、结果状态、MCP 回包是否发送、断连时是否有活动任务。未知工具名统一记为 unknown，不输出正文、密码、token 或原始参数值。
- WebSocket Ping 返回发送结果并记录成功/失败、Pong 和接收帧计数；服务端 Ping 的 Pong 延后由发送任务处理，避免在 TLS 接收回调内同步重入发送。发送加锁并要求写满整帧；接收帧/重组消息限定 65535 字节，控制帧限定 125 字节且必须 FIN。
- TLS 释放改为 shutdown 唤醒接收、等待接收任务退出、最后由 esp_tls_conn_destroy 释放并关闭一次；避免先 close 后 fd 被其他连接复用再关闭的风险。处理接收任务创建失败，WANT_READ/WANT_WRITE 让出 CPU，发送重试有超时。此处的确定性缺陷修复不等于已证明是现场 -76 的原因。

服务端仍必须启用设备 MCP 并完整发现 27 个工具。模型不调用 self.chat.get_task 时，固件会明确报错，不能靠客户端保证模型遵循工具说明。角色说明见 CHAT_HISTORY.md。

## 2. 简约控制中心

继续使用纯黑白、不透明模态页和既有 Lucide 图标。首页及其他产品页面布局不变。控制中心默认只保留：一条音量滑轨和加减键、当前网络入口、并列的铃声/震动、锁屏休眠、收起。移除默认页面的大段 BLE/技术说明和重复音量显示；不重新启用 BLE，不伪装为外置蓝牙音频开关。

顶部下拉或点状态栏打开；上滑、HOME/PREV/BACK 或收起按钮返回原页面。媒体音量仍调用原音量接口，设置和 NVS 在显示锁外执行。编辑过程中不直接跳去网络配置；草稿保留。铃声/震动仍是提醒输出偏好，关掉两者也保留视觉提醒，不关闭触摸短震。

## 3. 电源键与浅睡眠

- 短按 POWER：空闲时锁屏；锁屏后再短按唤醒。按下边沿与唤醒后松手/Click 去重，避免刚醒又睡。长按仍沿用原关机流程，不替换为深睡。
- 默认 300 秒无操作自动锁屏；有未提交编辑表单时不自动锁屏。读取 sleep/idle_sec 的现有 NVS 设置：0 禁用，其他值限制 60–1800 秒。本版不增加设置页或新增远程写入接口。
- 正在对话、录音、播放、响铃、联网配置、写笔记/读文件、U 盘占用时不强制休眠，明确提示先完成操作。后台 SD 待写内容先排空；20 秒仍未就绪则取消本次休眠并恢复页面。
- 实际采用 esp_light_sleep_start，保留 RAM 中的页面、草稿与会话状态，不是断电重启式 Deep-sleep。唤醒后按原机制自动重连 Wi-Fi/小智，不自动开启录音。
- USB Serial/JTAG 检测到主机、4G 模式或实验 BLE 构建时仅锁屏/暂停可暂停服务，不进入 CPU 浅睡。USB 保护用于避免调试/刷机链路消失；测睡眠必须脱离数据主机。

### 休眠顺序与低功耗范围

用 activity lease + Freeze 屏障协调输入、文件、网络和外设操作，复用主事件任务，不新增常驻 RTOS 工作栈。锁屏后停小智连接/心跳与天气/额度联网刷新，等待历史保存，暂停 Wi-Fi、I2S TX/RX DMA；能正确读写扩展器时关闭 PA 并记住原状态，暂停触摸和音量键轮询、系统监控。睡前暂停 button 定时器，醒后恢复，避免积累的 5ms 回调集中执行。

CPU 单次最多浅睡 30 秒，也会在下一提醒前约 1 秒唤醒让提醒任务运行；定时唤醒继续保持壁纸，不重连网络、不每秒刷新时钟。到期提醒或电源键唤醒回到 UI。外设暂停失败则回滚，恢复失败显示提示并间隔 5 秒重试，不在 DMA 未恢复时强开功放。

没有关闭 MAIN_PWR、SCREEN_SOCKET_PWR、SD 总电源、USB mux 或外置 UART/I2S 蓝牙模组，也未发送未知的 4G/蓝牙低功耗 AT 命令。实际整机电流还受外设与板级电路影响；未测电流，不提供续航倍数或微安指标。锁屏不是密码认证，不加密 SD 内容。

## 4. 休眠壁纸

无自定义文件时显示黑白默认页“休息一下”。锁屏时整页不透明覆盖，不透出聊天或 Wi-Fi 密码。自定义文件固定为 SD 的 /wallpaper/lock.pbm，即固件路径 /sdcard/wallpaper/lock.pbm；P4 PBM、480×800、1-bit，正文字节恰为 48000。无文件、尺寸/格式不合法时回退默认页。底部 y>=664 为锁屏说明保留区域。

可在电脑本地转换自己的图片：

```sh
python3 -m pip install Pillow
python3 tools/make_sleep_wallpaper.py /path/to/picture.jpg --out /path/to/lock.pbm
```

工具纠正 EXIF 方向、等比裁切、白底合成、阈值二值化，不用灰阶或抖动输出，不上传图片，不覆盖原图。复制到 SD 后下次锁屏加载。无每秒刷新、无闪动光标；后台通知、直接绘制路径、AI 页面更新都不能覆盖锁屏壁纸。解锁取消密码明文显示，但保留未提交编辑内容。

## 5. 诊断和部署

继续使用原 ESP-IDF 6.0.1 环境构建。确认实际端口、设备身份与分区匹配后仅 app-flash；本次不需要刷 NVS、字体、分区表或 OTA 选择。先备份 SD，关闭占用串口的网页/监视器。

```sh
python3 tools/miaoink4_serial.py --port /dev/cu.usbmodemXXXXX command 'XIAOZHI_NET?'
python3 tools/miaoink4_serial.py --port /dev/cu.usbmodemXXXXX command 'SLEEP?'
python3 tools/miaoink4_serial.py --port /dev/cu.usbmodemXXXXX command 'SLEEP'
python3 tools/miaoink4_serial.py --port /dev/cu.usbmodemXXXXX command 'WAKE'
```

SLEEP/WAKE 是排队请求，不把 ACK 当作已经进睡。SLEEP? 返回 locked、sleeps、slept_ms、failures、idle_sec；usb_guard=1 表示保护策略开启，不是主机连接状态。USB 连着电脑时 sleeps 不增长是预期行为。

XIAOZHI_NET? 返回 connected/ready/epoch/turn、ws_busy、ping/pong/ping_fail、frames、close、tls_error、任务字节/已读字节/次数/失败。ws_busy=1 时不把本次空白传输计数解读成确实未发 Ping。连接释放后的状态可能已清零，定位旧连接要结合连续元数据日志。

实机验收分开做：真人语音后分别点三个快捷操作，核对 task read complete=1 后是否有真实回答；长时间观察 Ping/Pong 与断线；电池供电短按锁屏/唤醒、五分钟自动锁屏、编辑保护、闹钟唤醒；再检查 TTS/铃声、最大连续内存、SD 待保存错误和自定义壁纸。USB 连接时只能验证锁屏/交互，不是电池睡眠或电流验收。验证证据见 SLEEP_CONNECTION_VALIDATION_20260922.md。
