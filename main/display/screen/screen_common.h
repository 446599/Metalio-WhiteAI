#pragma once

#include "lvgl.h"

struct EpdStatusBar {
    lv_obj_t* bar = nullptr;           // 图标行（网络 / 静音 / 电量）
    lv_obj_t* overlay = nullptr;       // 叠在上方的状态/通知文字
    lv_obj_t* network_label = nullptr;
    lv_obj_t* mute_label = nullptr;
    lv_obj_t* battery_percent_label = nullptr;
    lv_obj_t* battery_label = nullptr;
    lv_obj_t* status_label = nullptr;
    lv_obj_t* notification_label = nullptr;
    lv_obj_t* low_battery_popup = nullptr;
    lv_coord_t height = 0;
};

// 墨水屏通用顶栏（白底黑字），高度对齐 MiSans Regular 25。
EpdStatusBar ScreenCreateStatusBar(lv_obj_t* scr);

// 占位页：顶栏 + 居中标题（返回由虚拟键默认策略负责）。
// screen_id 用于虚拟键前台栈（须为静态字符串，如 "book"）。
lv_obj_t* ScreenCreatePlaceholder(const char* screen_id, const char* title);

// 切到新屏并异步删旧屏（不维护返回栈；应用内跳转等用此接口）。
void ScreenLoadReplace(lv_obj_t* new_scr);

// 前进导航：若当前页注册了 factory，则压入返回栈，再切到 create()。
// create 为 ScreenFactory（见 vk_key_handler.h）。
void ScreenNavigateTo(lv_obj_t* (*create)());

// 返回上一页（弹返回栈）；栈空则回首页。可从触摸回调直接调（内部 async）。
void ScreenNavigateBack();

// 标记当前是否为首页（Launcher / 应用 Create 时调用）。
void ScreenSetIsHome(bool is_home);
bool ScreenIsHome();

// 回首页并清空返回栈。
void ScreenGoHome();

// 请求回首页（异步）。vk_home 默认策略走此接口。
void ScreenRequestHome();

// 兼容旧名：等同 ScreenRequestHome。
void ScreenRequestBack();

// 弹窗外遮罩：I1 友好大颗粒网点底（16×16 平铺、3×3 黑块）。
void ScreenApplyDotBackdrop(lv_obj_t* obj);
