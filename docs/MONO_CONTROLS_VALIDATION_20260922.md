# 黑白字体与控制栏：默认分支交付验证

日期：2026-09-22。基线为用户已验证的 `093e9df1811de0b0670879bffb61d9cd138df8c4`。
本次交付采用已经通过完整 CI 的 `codex/mono-controls` 实现，不是早先仅完成主机测试的离线补丁。
功能边界和操作方法见 [MONO_CONTROLS.md](MONO_CONTROLS.md)。

## 可追溯来源

- 验证运行：[Mono controls validation / 35671645875](https://github.com/446599/Metalio-WhiteAI/actions/runs/35671645875)。
- 运行输入提交：`83c890912d80c3d626ecbfa77f474ec6b5f5b948`。
- 工作流先应用校验过的源码 overlay，再分别执行 host 和 firmware；二者成功后，publish 将同一 overlay 提交到隔离开发分支。
- 已验证源码提交：`8579f0ca4e20b34583573d7fbc7b035cfc105b46`。
- 默认分支直接采用相同的 `main` 子树 `7f8674259861683a31b2453f5556f573b3b87e88` 和 `tools` 子树 `c0ec3555ffa65d2a9e780c037d447ea51f56a44e`，不重新转录产品代码。
- 不带入 `.ci/` 临时补丁、应用脚本或自动提交工作流。原常规验证工作流加入新检查及默认分支 push 触发，只保留 contents: read 权限。

## 已完成检查

`prepare`、`host`、`firmware`、`publish` 四个 job 均为 success。已下载并核对验证产物 SHA-256 和日志，不仅依赖提交说明。

| 检查 | 结果 |
| --- | --- |
| 原有 14 组主机回归 | 全部通过：输入联网、SSID 存储、UI、AI、Dashboard、Reminders、SystemTools、Memory、InputTransport、VolumeCallbacks、SerialWrite、Recorder、RefreshPolicy、ReminderChime |
| `check_mono_controls.py` | 4408 次运行时 CHECK，通过；不是 4408 个独立测试用例 |
| 拼音和联网 | 3176 次运行时 CHECK，通过 |
| 记忆 | 504 次 CHECK，通过；26 个 MCP 工具、9 页 |
| 实际 framebuffer 预览 | 正常、离线、空、长文本、过期五组，每组 91 帧，共 455 帧；另有每组总览，共 460 个 PNG |
| 页面输入路由 | 实际绘制/触摸/按键函数；控制栏、书库、键盘、AI 归档以及显示锁外副作用检查通过 |
| 完整 ESP32-S3 构建 | 官方 `espressif/idf:v6.0.1`，1397 个 Ninja 步骤完成，包含链接和镜像生成 |
| 交付检查 | `Build delivery OK: 5 images; font 45248 glyphs` |

预览采用仓库真实位图资源；字体最终输出仍为黑白。无线电、任务、NVS 和外设在主机测试中模拟；软件预览不代表屏幕光学验收。

## 构建产物标识

| 项目 | 此轮 CI 值 |
| --- | --- |
| 应用字节数 | 3275008（0x31f900） |
| 应用分区 | ota_0 / 0x80000 / 5 MiB |
| 剩余容量 | 1967872 字节，约 38% |
| 应用 SHA-256 | fedd762fde6d6629faa3ddfe9327fb9cdba70330a9882ad526aba8cc9e176741 |
| 分区表 SHA-256 | b2ab8fbfdadaf0bcd91955cae6e646dcc92e698df9a364c7f9481334e5fa3113 |
| 原字体 SHA-256 | 00fec4502eef536f2f7581b15269bf7b06f6440cca97791118bb70868e2ca3cb |
| `mono-build-validation` ZIP SHA-256 | 5bb73893faaaed4e2f28ba7685f6319c2a88f033d301c916ce19900c937053f2 |
| `mono-host-validation` ZIP SHA-256 | ba04d705c15787085a1ac9a7a5df97a44c3bf2a562ab3e9774393510680184c4 |

应用哈希只对应此轮 CI，编译时间或环境变化可能使重建结果不同，以自己的 delivery_manifest.json 为准。容器构建会生成环境相关配置；没有把 CI 中改写的 sdkconfig 或依赖锁覆盖到仓库。

## 发布与未验证范围

保留 USB mux、安全上电、boot_diag、DTR/RTS 释放和用户最新音频任务内部 RAM 修复。未更改分区、完整字库、sdkconfig、依赖锁或屏幕 LUT，不包含新的 TTF/fontpack 下载。

已有本项目布局的设备核对分区后只需 `idf.py build` 和 `idf.py -p <实际端口> app-flash`；不需要重刷字体或清空 NVS。关闭占用串口的浏览器/监视器后再烧录。

未执行实体设备刷写，也未验证真实 Wi-Fi/BLE 射频、外置蓝牙配对、音频并发峰值、真实 SD 掉电、触摸速度、残影和续航。建议真机回归：下拉开关与草稿恢复、音量后语音/铃声、四种提醒组合、BLE 停止后继续语音、密码遮罩、触摸归档去重、TXT 阅读及重启恢复。
