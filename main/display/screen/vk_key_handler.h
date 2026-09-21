#pragma once

#include "lvgl.h"

// ---------------------------------------------------------------------------
// 盖板虚拟键统一分发（按键语义对齐 config.h 注释）
//
// 架构（解耦）：
//   1) 各页 VkKey_AttachScreen 注册可选 factory / on_key / boot / 盖板长按钩子
//   2) Dispatch 先问当前页 on_key；返回 true 则已消费
//   3) 否则走全局默认策略（页面无需关心）
//   4) 板载 BOOT 按下/松开/短按/长按由 BootKey_* 查本注册表分发（见 boot_key_handler）
//   5) 盖板键长按/松开由 VkKey_OnLongPress / VkKey_OnPressUp 分发（仅配置了
//      long_press_ms 的触摸虚拟键会到达这里；未注册钩子则返回 false）
//
// 默认策略：
//   vk_home — 回首页（清空返回栈）；已在首页 no-op
//   vk_prev — 返回上一页（ScreenNavigateBack）；已在首页 no-op
//   vk_next — no-op
//
// 覆盖示例：测试页在 Attach 时传入 on_key，自行处理虚拟键。
// ---------------------------------------------------------------------------

typedef enum {
    VK_SCREEN_LIFECYCLE_LOAD = 0,
    VK_SCREEN_LIFECYCLE_UNLOAD,
} vk_screen_lifecycle_t;

// 用于返回栈重建页面；须为无捕获静态函数，如 AppLauncher::Create。
using ScreenFactory = lv_obj_t* (*)();

// 返回 true 表示本页已处理该键，不再走默认策略。
using VkKeyHandler = bool (*)(const char* key_name);

// 板载 BOOT 动作；返回 true 表示本页已处理。须自行 lv_async_call 再碰 LVGL。
using BootKeyAction = bool (*)();

struct VkKeyScreenDesc {
    ScreenFactory factory = nullptr;             // 可被 ScreenNavigateTo 压入返回栈
    VkKeyHandler on_key = nullptr;               // 可选盖板虚拟键短按覆盖
    BootKeyAction on_boot_click = nullptr;       // 可选 BOOT 短按（单击）；语义由页面定义
    BootKeyAction on_boot_long_press = nullptr;  // 可选 BOOT 长按
    BootKeyAction on_boot_press_down = nullptr;  // 可选 BOOT 按下（勿默认当听）
    BootKeyAction on_boot_press_up = nullptr;    // 可选 BOOT 松开
    VkKeyHandler on_key_long_press = nullptr;    // 可选盖板键长按（如 vk_home PTT）
    VkKeyHandler on_key_press_up = nullptr;      // 可选盖板键松开（配对长按）
};

// 页面生命周期：LOAD 压栈，UNLOAD 移除最靠上的同名页。
void VkKey_OnScreenLifecycle(const char* name, vk_screen_lifecycle_t event);

// 当前前台页面名（栈空时为 "none"）。
const char* VkKey_ActiveScreen();

// 查注册表：当前/指定页的重建 factory（未注册则为 nullptr）。
ScreenFactory VkKey_GetScreenFactory(const char* name);

// 仅更新已注册页的 factory（不重复挂生命周期回调）。用于同页内切换返回目标。
void VkKey_SetScreenFactory(const char* name, ScreenFactory factory);

// 查注册表：BOOT 钩子（未注册则为 nullptr）。
BootKeyAction VkKey_GetBootClick(const char* name);
BootKeyAction VkKey_GetBootLongPress(const char* name);
BootKeyAction VkKey_GetBootPressDown(const char* name);
BootKeyAction VkKey_GetBootPressUp(const char* name);

// 给屏挂上 LOADED/UNLOADED，并注册策略（name 须为静态字符串）。
void VkKey_AttachScreen(lv_obj_t* scr, const char* name);
void VkKey_AttachScreen(lv_obj_t* scr, const char* name, const VkKeyScreenDesc& desc);

// 板级触摸虚拟键回调入口（短按 / Click）。
void VkKey_Dispatch(const char* key_name);

// 盖板键长按 / 松开。返回 true 表示当前页已消费（长按消费后触摸层不再发 Click）。
bool VkKey_OnLongPress(const char* key_name);
bool VkKey_OnPressUp(const char* key_name);
