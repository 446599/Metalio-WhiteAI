#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 按键震动偏好（NVS namespace "display" / key "haptic"，默认开启）。
// 读走 RAM 缓存；写立刻改内存，NVS 落盘在内部 RAM 栈任务中完成。
bool HapticIsEnabled(void);
void HapticSetEnabled(bool enabled);

// 开关开启时触发 Board::PulseVibration()；无马达板为空操作。
// 可在按键回调中调用。
void HapticPulseIfEnabled(void);


#ifdef __cplusplus
}
#endif
