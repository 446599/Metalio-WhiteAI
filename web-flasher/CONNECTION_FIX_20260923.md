# Web flasher 1.1.0 — 串口连接卡住修复

基线：主分支 599f5bc69fa0aea4b4b752eff6a9eb10fd3c4a7f；已部署页面 gh-pages 6e274d5db85a3e5559f2ce9eb5303d02f374232b。保留用户新增的六个固件下载入口、全部分区选择/校验/敏感确认。本次不修改设备固件、USB mux、NVS、字体、分区或波形，也没有连接真实设备。

## 已复现的问题与现场边界

使用站点现有的 esptool-js 0.7.0 bundle，而不是重写一个假的 Transport，复现如下路径：串口 writable.write 抛出错误后，SDK 的 write 未在 finally 中 releaseLock；SDK disconnect 的 waitForUnlock(400) 会不断轮询，400 是间隔而不是总超时。原 FlashSession 在错误清理中一直等待 disconnect，页面 busy 不会释放，用户无法重试或取消。

该写锁问题由真实 SDK 加模拟 Web Serial 流可确定复现；但没有本次用户的浏览器串口日志或硬件接入，不能据此断定现场每一种无响应都由它引起。站点原本已有自托管 SDK，不把 CDN 缺失说成已确定根因。固件下载区的新增不是此次写锁问题的来源。

## 修复

- SerialPortLease 使用 Web Serial 公共流接口记录读写器，在失败/取消时取消 reader、abort writer、释放锁；不修改 SDK 二进制或访问 SDK 私有成员。
- 端口打开、读写控制、清理有界；SDK 加载、ROM/stub 握手、安全检查、容量与分区表读取分别显示阶段并限制等待。无响应会显示失败，不跳过验证放行刷写。
- 选中串口后可取消连接，忙碌期间仍可导出日志。取消只用于建立连接，不作为中断 Flash 写入的按钮。
- 取消 Promise 不等于原生 USB 操作已结束。原生 open/write/setSignals 晚到时，同一个端口在旧操作及清理完成前仍保持占用，不能被下一次连接复用；无法释放时要求重新插拔。旧 Transport 的掉线回调不能关闭新会话。
- 原生 303A:1001 USB Serial/JTAG 固定 115200，避免为了修改 line coding 重开端口。外接 UART 桥继续使用所选速率。保留原自动下载复位与 no_reset 手动下载模式，不改 eFuse/设备引脚。
- 本地 SDK HEAD 和动态模块加载增加等待上限，只有确实不存在（404）才使用固定版本 CDN；本地响应错误/MIME 不正确不会静默改用其他文件。

## 验证证据

隔离验证：https://github.com/446599/Metalio-WhiteAI/actions/runs/35812930710
先前完整验证：https://github.com/446599/Metalio-WhiteAI/actions/runs/35812664718
最终经过验证的 web-flasher 源码树：c537de461fa2d3bcffac5da2aa495985f1fc7592。交付另加此说明及常规只读 CI，不改经过验证的生产代码。

- npm test：103 项通过，零跳过。包含原 84 项 Flash 安全和业务测试，以及 19 项超时、取消、真实 SDK 流清理、旧回调隔离和 SDK 加载测试。
- SDK 安装器：5 项通过。
- Chromium + Playwright 1.58.0：真实 HTTP 页面与原 CSP 下 21 条交互流程通过，USB/ESPLoader 业务使用模拟适配器，未请求硬件权限。
- 另在实际 Chromium 页面 CSP 下导入真正的本地 SDK，Transport/ESPLoader 导出校验通过，没有使用 CDN；这不是实际 USB 下载握手。
- CI 与本地源码 Git tree 完全一致；所有测试失败会阻止隔离发布，shell 启用 pipefail，没有 continue-on-error。
- SDK Git blob：1ba8cad43b3ffb8c257de73a1a06e6702aa900ca；SHA-256：00ebe6fb0b202976d3e2a3c6b9a6bd438c26a81d2af6739c95ac276a64c1a84e。

主分支加入独立的 Web flasher validation，覆盖网页变更；不依赖仅监控固件目录的原 CI。站点部署需要把相同文件同步到 gh-pages 根目录，保留原 vendor 资源；只更新主分支不会更新现有网页。

## 使用

重新打开原刷机站并强制刷新，页脚应显示 FLASH 1.1.0。关闭其他占用串口的窗口，重新选择设备。若提示下载模式握手失败，手动按住 BOOT（AI）并复位/重新上电，再选择“已手动进入下载模式”重连；USB 重新枚举后需重选当前端口。未完成连接前没有 Flash 写入。

本次未执行：真实浏览器权限选择、USB 驱动、DTR/RTS、电源、ROM/stub 握手、物理备份或刷写。不要把模拟流或网页测试解释为已验证用户设备刷机成功。出现问题可导出阶段日志，继续区分库加载、下载模式、安全检查及分区表阶段。
