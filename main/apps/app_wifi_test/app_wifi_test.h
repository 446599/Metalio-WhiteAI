#pragma once

#include "lvgl.h"

// WiFi 测试：扫描 → 选网 → 密码页连接（成功拿到 IP 才算通过）。
class AppWifiTest {
public:
    static lv_obj_t* Create();
    static lv_obj_t* CreatePassword();
};
