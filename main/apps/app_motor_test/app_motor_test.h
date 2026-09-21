#pragma once

#include "lvgl.h"

// 马达测试：按钮在停止 / 持续震动间切换（不受设置页 haptic 开关影响）。
class AppMotorTest {
public:
    static lv_obj_t* Create();
};
