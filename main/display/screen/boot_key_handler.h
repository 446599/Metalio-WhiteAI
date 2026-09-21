#pragma once

// ---------------------------------------------------------------------------
// 板载 BOOT 键（GPIO0 / IO0）统一分发
//
// 板级 InitializeButtons：
//   OnPressDown  → BootKey_OnPressDown
//   OnPressUp    → BootKey_OnPressUp
//   OnClick      → BootKey_OnClick
//   OnLongPress  → BootKey_OnLongPress
//
// 策略不写死在板级，而由各页 VkKey_AttachScreen 注册。语义由页面定义。
// ---------------------------------------------------------------------------

void BootKey_OnPressDown();
void BootKey_OnPressUp();
void BootKey_OnClick();
void BootKey_OnLongPress();

/** 自上次 PressDown 起尚未 PressUp（跨切页仍有效）。 */
bool BootKey_IsHeld();

/** 本轮按下是否已触发过长按（用于与短按互斥）。 */
bool BootKey_DidLongPress();
