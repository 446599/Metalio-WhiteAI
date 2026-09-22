#include "hal.h"

// A real unavailable implementation, not a fake scan result. Keeping this
// translation unit independent of NimBLE prevents controller IRAM from being
// linked even if an old diagnostic caller still invokes the HAL entry point.
bool Hal::BleScan(std::vector<HalBtDevice>& out, std::string& detail,
                  uint32_t timeout_ms, const std::function<bool()>& cancelled) {
    (void)timeout_ms;
    (void)cancelled;
    out.clear();
    detail = "BLE 发现已停用，优先保障语音与铃声";
    return false;
}
