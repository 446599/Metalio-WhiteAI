#pragma once
namespace reminder_ui {
enum class Action { None, Stop, Snooze };
constexpr int kLeft = 32, kWidth = 416, kHeight = 72;
constexpr int kStopY = 568, kSnoozeY = 664;
constexpr Action Hit(int x, int y) {
    if (x < kLeft || x >= kLeft+kWidth) return Action::None;
    if (y >= kStopY && y < kStopY+kHeight) return Action::Stop;
    if (y >= kSnoozeY && y < kSnoozeY+kHeight) return Action::Snooze;
    return Action::None;
}
}  // namespace reminder_ui
