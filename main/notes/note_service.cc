#include "note_service.h"
#include "snapshot_file.h"
#include "hal/hal.h"
#include <esp_log.h>
#include <sys/stat.h>

namespace notes {
namespace {
SnapshotFile& File() {static SnapshotFile file("/sdcard/notes/mcp");return file;}
}
Store& DeviceStore() {
    static Store store([](const std::string& json) {
        return GetHAL().IsSdMounted() && File().Save(json);
    });
    return store;
}
void Start() {
    if (!GetHAL().IsSdMounted()) {ESP_LOGW("Notes","SD unavailable; notes disabled");return;}
    if (mkdir("/sdcard/notes",0755)!=0 && errno!=EEXIST) return;
    std::string json;
    const bool loaded=File().Load(json,[](const auto& text) {
        Store candidate([](const auto&){return false;});return candidate.Restore(text);
    });
    if (!loaded || !DeviceStore().Restore(json)) ESP_LOGE("Notes","storage unavailable; existing snapshots preserved");
}
}
