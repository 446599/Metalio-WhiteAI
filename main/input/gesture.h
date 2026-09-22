#pragma once
#include <cstdlib>
namespace input {
enum class Pull { None, Open, Close };
inline Pull ControlPull(int x0,int y0,int x1,int y1,int held_ms,bool open) {
    if (held_ms < 0 || held_ms > 1600 || x0 < 0 || x0 >= 480 ||
        x1 < 0 || x1 >= 480 || y0 < 0 || y0 >= 800 || y1 < 0 || y1 >= 800 ||
        std::abs(x1-x0)>96) return Pull::None;
    if (!open && y0<64 && y1-y0>=64) return Pull::Open;
    if (open && y0-y1>=64) return Pull::Close;
    return Pull::None;
}
}
