# Metalio AI Ink Dashboard

## 项目定位

这是 Metalio E-Ink 4 的 ESP32-S3 固件。产品 UI 走 `RawDisplay` 的 1-bit framebuffer 路径；LVGL 适配器和旧硬件测试页只用于诊断，不属于当前产品启动路径。

## 常用命令

```bash
python3 tools/check_ui_contract.py
python3 tools/check_ai_contract.py
python3 tools/check_dashboard_contract.py
python3 tools/check_reminders_contract.py
python3 tools/check_system_tools.py
python3 tools/render_ui_preview.py

export IDF_PATH=/Users/henry/.espressif/v6.0.1/esp-idf
idf.py set-target esp32s3
idf.py build
```

## 代码边界

- 产品启动和服务编排：`main/application.*`
- 产品页面、输入路由和屏幕刷新：`main/display/raw_display.*`
- 板级硬件和外设：`main/hal/`
- 天气、额度和离线快照：`main/dashboard/`
- 闹钟与日程：`main/reminders/`
- 笔记和闪念胶囊：`main/notes/`、`main/xiaozhi/`
- Xiaozhi 网络、MCP 和音频：`main/xiaozhi/`
- 主机检查与串口工具：`tools/`

## 修改约定

- 先运行对应的 `tools/check_*.py`，再修改行为。
- 不在屏幕刷新锁内做网络、TLS、NVS 或长时间阻塞操作。
- 输入回调只提交有界事件；绘制和持久化在所属任务中执行。
- 不把 token、私人对话文本或设备凭据写入日志、测试输出或仓库。
- 不把 `build/`、`managed_components/`、预览产物和本地环境文件加入版本控制。
- 修改 UI 几何后，同时更新 `docs/UI_QA_CONTRACT.md` 和主机预览检查。

## 交付边界

应用镜像写入分区表定义的 `0x80000`。字体位于独立 `font_data` 分区，首次部署和应用更新必须按 `docs/BUILD_DELIVERY.md` 区分处理。
