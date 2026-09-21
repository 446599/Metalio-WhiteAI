# 项目地图

## 运行时主链路

```text
app_main
  -> Application::Start
     -> HAL 初始化
     -> RawDisplay 首页
     -> 主事件任务
     -> Dashboard / Notes / Reminders / Xiaozhi 服务
```

## 目录职责

| 目录 | 职责 |
| --- | --- |
| `main/application.*` | 启动、事件组、UI 工作队列和状态更新 |
| `main/display/` | raw framebuffer 绘制、字体、图标、刷新波形、触摸与页面路由 |
| `main/hal/` | Metalio 板级驱动和通用外设抽象 |
| `main/dashboard/` | 天气、额度、卡片和离线快照 |
| `main/reminders/` | 闹钟/日程存储、到期处理和提醒状态 |
| `main/notes/` | 笔记存储、SD 快照和恢复 |
| `main/xiaozhi/` | WebSocket、激活、MCP 工具、会话和 Opus 音频 |
| `main/system/` | 设备控制和异步系统动作 |
| `main/apps/` | 原有硬件诊断应用，产品首页不直接依赖 |
| `components/` | 项目本地 ESP-IDF 组件 |
| `assets/` | 完整字体包和 Lucide SVG 资源 |
| `partitions/` | Flash 分区表 |
| `tools/` | 构建检查、UI 预览、串口诊断和资源生成 |
| `docs/` | 产品计划、验证记录、交付说明和 QA 契约 |

## 当前产品页面

首页、AI 对话、闪念胶囊、阅读演示、今日列表、卡片、应用目录、设备设置、闹钟、录音、笔记和笔记详情由 `RawDisplay::ProductPage` 统一路由。

## 可信验证入口

- 几何与命中区：`tools/check_ui_contract.py`
- AI 会话/保存：`tools/check_ai_contract.py`
- Dashboard：`tools/check_dashboard_contract.py`
- 闹钟和 MCP：`tools/check_reminders_contract.py`
- 系统工具：`tools/check_system_tools.py`
- 刷新、输入、串口：其余 `tools/check_*.py`
- 页面预览：`tools/render_ui_preview.py`

## 需要继续收口的边界

详细记录见 [`OPTIMIZATION_PLAN.md`](OPTIMIZATION_PLAN.md)。当前优先级是：可复现构建和字体部署、跨轮会话隔离、胶囊保存语义、额度单位、离线数据新鲜度，以及输入和屏幕刷新解耦。
