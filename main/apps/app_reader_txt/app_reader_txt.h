#pragma once

#include "lvgl.h"

// Eego TXT reader port. EPUB is intentionally not included.
class AppReaderTxt {
public:
    static lv_obj_t* Create();
    static lv_obj_t* CreateReader();
};
