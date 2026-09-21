# 构建与烧录交付

本工程目标为 ESP32-S3、16 MB Flash，使用 `partitions/v1/16m.csv`。
执行 `idf.py build` 会校验完整字库、生成 `build/font_data.bin`，并将镜像大小、
分区边界和 SHA-256 写入 `build/delivery_manifest.json`。构建检查失败时不应烧录。

## 首次部署

```sh
idf.py build
idf.py -p /dev/cu.usbmodem21101 flash
```

标准 `flash` 包含 bootloader、分区表、初始 OTA 选择、应用和完整字库。
它会重置 OTA 槽选择；交付镜像不包含 NVS。已有其他分区布局的设备需要先制定迁移方案，
不能仅凭芯片型号相同就直接更新应用。

## 已有设备更新

先读取设备 `0x8000` 处的分区表，确认与本次构建一致：

```sh
python -m esptool --chip esp32s3 --port /dev/cu.usbmodem21101 \
  read-flash 0x8000 0x1000 /tmp/miaoink4-partition.bin
python3 tools/check_build_delivery.py \
  --device-partition-table /tmp/miaoink4-partition.bin
```

确认设备使用本工程的 `ota_0` 应用槽后，选择所需更新目标：

```sh
idf.py -p /dev/cu.usbmodem21101 app-flash
idf.py -p /dev/cu.usbmodem21101 font-flash
```

`app-flash` 仅写应用（当前地址 `0x80000`）；`font-flash` 仅写字库
（当前地址 `0xae4000`）。两者都不修改分区表、OTA 选择或 NVS。
若设备曾 OTA 切换到其他槽，需先核对当前启动槽；写入 `ota_0` 不会自动切换启动槽。

## 烧录后验证

检查烧录日志中的 `Hash of data verified`，并查询固件运行状态：

```sh
python3 tools/miaoink4_serial.py command 'FONT?'
python3 tools/miaoink4_serial.py command 'XIAOZHI_STATS?'
```

完整字库预期返回 `@@FONT_ACK ready=1 glyphs=45248`。烧录哈希验证证明写入数据
与构建镜像一致；实体按键、屏幕和扬声器仍须单独回归。串口打开可能让设备复位，
连续动作应保持同一串口会话，避免把 USB 复位误判为按键崩溃。
