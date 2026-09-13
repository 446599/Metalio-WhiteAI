# Metalio 硬件测试固件

面向 **Metalio E-Ink 4**（ESP32-S3）的上电调试与产测固件。

本仓库正在演进为面向 AI 的墨水屏阅读器固件：保留完整硬件诊断能力，并新增 AI 首页、日程、天气、自定义卡片与小智接入层。产品设计见 [docs/AI_PRODUCT_PLAN.md](docs/AI_PRODUCT_PLAN.md)。

| 项目 | 说明 |
|------|------|
| 工程名 | `metalio-hw-test` |
| 版本 | 2.0.51 |
| 芯片 | ESP32-S3 |
| 开发板 | Metalio E-Ink 4 |
| 屏幕 | GDEM0397T81 3.97″，800×480，SSD1677 |
| ESP-IDF | ≥ 5.5.2 |

English: [README.md](README.md).

## 可测项目

设备启动后进入测试主页，当前包含：

- 电子纸显示
- 震动马达
- 电容触摸（CST816S）及盖板虚拟键
- 实体按键
- 电池电量计（BQ27220）
- 音频（外置蓝牙音频模块 / I2S）
- 蓝牙扫描
- Wi-Fi
- 蜂窝网络（4G，NT26 / ML307 通路）
- microSD（SDMMC 1-bit）

板上相关外设还包括 TCA9555 IO 扩展、PCF8563 RTC，以及可选的 USB MSC（模拟 U 盘）。

## 目录结构

```
main/
  apps/           # 测试主页与各分项 App
  hal/
    common/       # 公共板级辅助
    metalio-e-ink-4/
  display/        # LVGL 适配、字库与界面辅助
  audio/
docs/             # 自定义开发板说明
partitions/       # Flash 分区表
components/       # 本地组件（Wi-Fi 配网、ML307 等）
```

板型在 `main/CMakeLists.txt`（`BOARD_TYPE=metalio-e-ink-4`）与 `main/Kconfig.projbuild` 中固定。

## 环境要求

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/get-started/) 5.5.2 及以上
- 目标芯片：ESP32-S3
- 按 ESP-IDF 要求配置工具链与 Python 环境

依赖见 `main/idf_component.yml`，首次编译时由 ESP Component Manager 拉取。

当前工作区已用 ESP-IDF 6.0.1（Python 3.14）完成构建验证。为兼容 IDF 6，工程使用 Component Registry 中的 `cjson`、`mqtt`，并在 `components/uart-uhci/` 保留了本地 UHCI 组件及 GDMA API 适配。

## 编译

```bash
# 先激活 ESP-IDF 环境
idf.py set-target esp32s3
idf.py build
```

烧录与串口监视：

```bash
idf.py -p PORT flash monitor
```

将 `PORT` 替换为实际串口（如 `/dev/ttyUSB0`、`COM3`）。

附近 BLE 观察相关的默认蓝牙配置见 `sdkconfig.defaults`。

## 使用说明

1. 上电并烧录本固件。
2. 启动后进入测试主页，按列表选择分项。
3. 按界面提示完成测试，再返回主页。

Wi-Fi 配网 SoftAP SSID 前缀：`MetalioEInk4`。

设备上 SD 数据目录约定为 `/sdcard/metalio/e-ink/`。

### TXT 阅读测试页

测试主页新增“TXT 阅读”。它会扫描上述目录和 SD 根目录中的 `.txt` 文件，支持
UTF-8、UTF-16、GBK/CP950 解码以及分页、上一页/下一页和中文行首标点约束。
阅读页右半屏翻到下一页，左半屏返回上一页；盖板 `vk_next`/`vk_prev` 也可翻页。
当前仅移植 TXT 阅读路径，未加入 EPUB 解析；单文件大小上限为 1 MiB。

## 配置说明

板型固定为 `metalio-e-ink-4`；引脚和外设参数集中在
`main/hal/metalio-e-ink-4/config.h`，启动与测试流程位于 `main/apps/`。

## 许可

除各文件或第三方组件另有说明外，以本仓库及所依赖组件的许可条款为准。
