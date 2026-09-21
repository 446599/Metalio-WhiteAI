# 拼音、Wi-Fi 与页面交互验证

日期：2026-09-22（北京时间；GitHub Actions 日志为 2026-09-21 UTC）。
基线：用户已验证的 `dab59281aad363874467d12a28b59a10a5aa4cab`。

## 执行证据

GitHub Actions：[run 35621851677](https://github.com/446599/Metalio-WhiteAI/actions/runs/35621851677)。
最终轮次 prepare、host、firmware、publish 均为 success。测试的正常源码已提交为
`86b631601a20ea3ea206e78c99ebaf2f5a87210e`；默认分支交付从这些已验证子树取内容，
不带入临时传输数据或自动提交工作流。下列三个子树与 CI 测试版本完全一致：

| 子树 | Git tree SHA |
| --- | --- |
| main | 23790f8b4200a144d9e70490146c6e952a73c351 |
| components | 1275881279b7a524e3a227777bce4f80766bea13 |
| tools | 83cf26144494955fc2b09b244278be0bda7081c3 |

最终额外变更仅为本验证说明和只读权限的常规 CI 工作流，不改变已测试产品代码。
未改用户的 USB mux、上电电平、boot_diag、串口 DTR/RTS 修复、分区、字库、LUT 或 sdkconfig。

## 主机验证

Linux x86_64、C++17、真实 cJSON 1.7.19。新编辑器/状态机和凭据存储测试启用 ASan/UBSan；
驱动、无线电、NVS 提交失败和任务调度边界按测试用途模拟，不将模拟结果表述为硬件成功。

- `check_input_network.py`：3176 次运行时 CHECK 断言通过，不是 3176 个独立测试用例。
  覆盖真实词表、全拼/分隔符/v、候选分页、UTF-8 光标/退格、密码模式、输入字节上限、
  键盘命中、SSID/密码边界、扫描去重/排序/容量、取消与旧操作代次、提交阶段和并发快照。
- `check_ssid_store.py`：实际 SsidManager 与模拟 NVS；旧键迁移、32/64 字节边界、失败提交不改内存、
  重启恢复、替换/排序、十项上限和损坏 blob 拒绝覆盖均通过。
- 其余 12 组原有检查通过：UI、AI、Dashboard、Reminders、SystemTools、Memory、InputTransport、
  VolumeCallbacks、SerialWrite、Recorder、RefreshPolicy、ReminderChime。
- 原记忆检查仍为 504 次 CHECK、26 个 MCP 工具/9 页，没有将新键盘或 Wi-Fi 密码暴露为 AI 工具。

词表实际生成 413 个拼音条目、6763 个不同 GB2312 汉字，加上编辑器中 36 个常用词组。
来源固定为 pinyin-data `923b108dc5d45dee061324c011b478fb649f8b73`，源 SHA-256：
`621f8ca9eff8519f47e2b17b564fd318161e13bca07eea8c8e04993cd5d3b52e`。
许可随文字词表提交；未生成或发布新的字体文件。

## 实际绘图与页面路由

`render_ui_preview.py` 编译并执行实际 C++ 绘图和新增页面输入处理，生成正常、离线、空内容、
长文本、过期数据五组，每组 78 帧，共 390 帧；缺字和文字越界检查通过。
额外执行真实触摸/硬件键路径：选网络、键入密码、遮罩/显示、取消、手动中文 SSID、候选选字、
笔记新建/编辑/保存结果、未保存退出确认。模拟锁断言验证扫描/连接/保存不在显示锁内启动。
已查看导出的 Wi-Fi 列表、密码键盘、拼音候选和笔记编辑页面；它们是软件 framebuffer 预览，不是屏幕光学照片。
密码输入页面禁止 FRAME/FRAME_PANEL 导出，默认隐藏，退出或提交清理编辑缓冲；不作加密或安全认证声明。

## 完整目标构建

使用官方容器 `espressif/idf:v6.0.1`，执行真实 `idf.py build`，1393 个 Ninja 步骤完成。
包含完整程序链接、ESP32-S3 镜像生成、分区边界和交付清单校验。

| 项目 | 本轮 CI 结果 |
| --- | --- |
| 应用大小 | 3121136 字节（0x2f9ff0） |
| 应用槽 | ota_0，0x80000，5 MiB |
| 槽内剩余 | 2121744 字节，约 40% |
| 应用 SHA-256 | bbde1f7204ced67f610bd312f3bae7eb9ffdf4af9c57e5b339120a6f359627a4 |
| 分区表 SHA-256 | b2ab8fbfdadaf0bcd91955cae6e646dcc92e698df9a364c7f9481334e5fa3113 |
| 交付检查 | Build delivery OK: 5 images; font 45248 glyphs |

上面应用哈希只标识这一轮 CI 产物；重新构建可能因编译时间/环境产生不同哈希，应以本次构建的
`build/delivery_manifest.json` 为准。此提交不附送烧录包，也没有操作实体设备。

## 部署与尚待设备验证

沿用原 ESP-IDF 6.0.1 环境，先 `idf.py build`，确认实际 USB 端口及设备分区匹配后仅 `app-flash`。
本次无需更新分区、OTA 选择、NVS 或字体。升级前备份现有 NVS；v2 凭据首次成功保存后，新版优先读 v2，
降级旧固件只会看到旧配置键。SD 笔记仍最多八条，延续已有 schema 2 兼容边界。

未验证：实际射频扫描、具体 AP 的 WPA/WPA2/WPA3 认证、错误密码和 DHCP 行为、USB 烧录、硬件重启、
长时间并发/峰值堆栈、实际触摸速度和墨水屏残影、续航。主机模拟和完整编译不能替代这些。
建议上机顺序：确认 FONT?/STATE? 保持可用；扫描并连目标 AP；错密码/取消不覆盖旧配置；重启自动连接；
断网中文笔记保存恢复；闹钟打断输入及密码隐藏。使用说明见 [INPUT_WIFI.md](INPUT_WIFI.md)。
