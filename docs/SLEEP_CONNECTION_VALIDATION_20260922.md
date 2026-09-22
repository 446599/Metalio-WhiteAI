# 睡眠与连接修复验证 — 2026-09-22

## 来源和版本

用户附件 WhiteAI-Xiaozhi-Translate-Disconnect-20260922.zip 是 c95bfef 实机证据。它确认接收失败与重连，未确定翻译失败的云端根因。本轮不把先前音频/USB 真机结果当成新休眠功能验收。

隔离验证运行：https://github.com/446599/Metalio-WhiteAI/actions/runs/35722975127
输入提交 5e7b85fd10c4c0a180aa61d9268ea253ee109216，prepare/host/firmware/publish 均成功。
验证后的普通源码提交 a9c356e27ed03235bbcb88fb033d1fb2149d591a。
默认分支直接采用以下已验证 Git 子树，未带入临时 .ci 传输文件或自动发布工作流：

| 子树 | Git tree SHA |
|---|---|
| main | 36cf73fc3837bcc852d09c4a9d84d68966c19671 |
| components | 8e0a70c635ad00449b53911fb120fe0b14496569 |
| tools | ab264ed48be6d4f65533e9fe483069f9466add71 |

额外修改仅为本说明、使用说明、UI QA 契约以及常规只读 CI 中加入两项检查。默认分支的二次运行状态以该提交的 Actions 为准。

## 实际验证

20 组主机检查全部通过，涵盖新增 check_sleep_contract.py、check_transport_health.py 与原有聊天、图标、音频、控制栏、输入、存储、提醒、串口和刷新回归。睡眠检查执行生产 SleepService、Gate、策略和 PBM 解析代码；RTOS/无线电/DMA/GPIO/时钟由适配器模拟，不是物理电流测试。覆盖屏障竞争、USB/4G 禁睡、存储排空、按键/闹钟唤醒、暂停与恢复失败回滚、自动锁屏与编辑保护。

连接检查执行生产 Ping/Pong、发送、接收边界以及 TLS shutdown/join/destroy 路径，使用假 socket；小智契约验证空闲断线保留完成回答、活动断线保留原文，以及元数据诊断与任务完整读取。不连接真实小智账号，不声称云端已配合调用工具。

CI 实际字库/C++ 页面预览共 509 帧：normal/offline/long/stale 各102帧，empty 101帧，另有5张总览。通过缺字和文字越界检查、实际页面输入路由、密码隐藏、保留草稿、壁纸位极性和锁屏期间禁止被直接重绘等断言。已查看控制中心及默认壁纸图，不是实体屏幕照片。

收尾时从 artifact 下载并按 SHA-256 校验 overlay、主机证据与编译证据。局部 overlay 不含未修改的依赖头，第一次本地主机补测因此缺 reminder_store.h；补入与远端 blob 6a55d89a1f35cd914143f08821fd0c65483075ee 一致的真实头后，check_sleep_contract.py 在 GCC 和 Clang 下均通过 ASan/UBSan。完整仓库 CI 没有此缺文件问题，未修改测试来忽略依赖。

## 完整目标编译

官方 espressif/idf:v6.0.1 容器中的真实 idf.py build 完成1403个 Ninja 步骤，镜像生成、音频内部内存护栏及五镜像交付检查全部通过。

| 项目 | 本轮 CI 数值 |
|---|---:|
| 应用字节数 | 3197536（0x30ca60） |
| 5 MiB 应用槽剩余 | 2045344 字节，约39% |
| IRAM 总占用 | 91392 字节 |
| 共享内部静态占用 | 148624 字节 |
| 与 c95bfef 内部静态差额 | +1776 字节 |
| BLE 禁止符号 | 无 |
| 内存预算错误 | 无；未提高原门限 |

应用 SHA-256：46e89090448dc683b80bfdb9213db65bf456d3b671c21de7949f904110c91c4f。
字体和分区表 SHA-256 保持原值；sdkconfig、dependencies.lock、分区、字体、刷新 LUT 及音频栈大小未修改。重新构建的镜像哈希可能受版本/编译时间影响，应以当次 delivery_manifest.json 为准。

## 未执行和验收条件

未刷写实体设备，未验证真实语音翻译/MCP、现场63秒断连、射频/路由器行为、实际 POWER 唤醒/闹钟时序、I2S 恢复和 TTS、真实 SD 断电/拔卡、电池电流/续航、峰值栈或运行时最大连续堆。静态内存预算通过不能替代这些。

锁屏功能不是安全认证；外部蓝牙/4G/SD 电源未被未经验证地关闭。USB主机连接、4G模式和实验BLE构建仅锁屏，不进入CPU浅睡，不能按这些场景测得的现象判断电池浅睡失效。先测锁屏，再在电池供电测睡眠计数与实际电流。
