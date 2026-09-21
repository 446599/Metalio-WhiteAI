#include "app_bluetooth_test.h"

#include "hal/hal.h"
#include "haptic_feedback.h"
#include "screen_common.h"
#include "vk_key_handler.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "fontpack_lvgl.h"

#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const char* TAG = "AppBluetoothTest";
constexpr const char* kScreenId = "app_bluetooth_test";
constexpr lv_coord_t kBtnH = 56;
constexpr lv_coord_t kBorderW = 2;
constexpr int kPageSize = 5;

lv_obj_t* s_scr = nullptr;
lv_obj_t* s_status = nullptr;
lv_obj_t* s_list = nullptr;
lv_obj_t* s_page_lbl = nullptr;
std::atomic<bool> s_busy{false};
std::atomic<bool> s_alive{false};
std::vector<HalBtDevice> s_devs;
int s_page = 0;

void SetStatus(const char* text) {
    if (s_status != nullptr && lv_obj_is_valid(s_status) && text != nullptr) {
        lv_label_set_text(s_status, text);
    }
}

void StatusAsync(void* p) {
    auto* msg = static_cast<char*>(p);
    if (s_alive.load() && msg != nullptr) {
        SetStatus(msg);
    }
    free(msg);
}

void PostStatus(const char* text) {
    char* copy = text != nullptr ? strdup(text) : nullptr;
    if (copy != nullptr) {
        lv_async_call(StatusAsync, copy);
    }
}

lv_obj_t* MakeBtn(lv_obj_t* parent, const char* title, lv_event_cb_t cb, lv_coord_t width = 0) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    if (width > 0) {
        lv_obj_set_width(btn, width);
    } else {
        lv_obj_set_width(btn, lv_pct(100));
    }
    lv_obj_set_height(btn, kBtnH);
    lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, kBorderW, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_margin_bottom(btn, 10, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

void RefreshReadyHint() {
    auto& hal = GetHAL();
    char buf[128];
    std::snprintf(buf, sizeof(buf), "UART:%s  模式1:%s",
                  hal.IsBtUartReady() ? "就绪" : "未就绪",
                  hal.IsBtModeReady() ? "就绪" : "未就绪");
    SetStatus(buf);
}

int PageCount() {
    if (s_devs.empty()) {
        return 1;
    }
    return static_cast<int>((s_devs.size() + kPageSize - 1) / kPageSize);
}

void RefreshListUi() {
    if (!s_alive.load() || s_list == nullptr || !lv_obj_is_valid(s_list)) {
        return;
    }
    lv_obj_clean(s_list);
    const int pages = PageCount();
    if (s_page < 0) {
        s_page = 0;
    }
    if (s_page >= pages) {
        s_page = pages - 1;
    }

    if (s_devs.empty()) {
        lv_obj_t* hint = lv_label_create(s_list);
        lv_label_set_text(hint, "未发现设备");
        lv_obj_set_width(hint, lv_pct(100));
    } else {
        const int start = s_page * kPageSize;
        const int end = std::min(start + kPageSize, static_cast<int>(s_devs.size()));
        for (int i = start; i < end; ++i) {
            const auto& d = s_devs[static_cast<size_t>(i)];
            lv_obj_t* row = lv_label_create(s_list);
            char line[160];
            if (!d.addr.empty()) {
                std::snprintf(line, sizeof(line), "%s\n%s  %ddBm", d.name.c_str(), d.addr.c_str(),
                              d.rssi);
            } else {
                std::snprintf(line, sizeof(line), "%s", d.name.c_str());
            }
            lv_label_set_text(row, line);
            lv_obj_set_width(row, lv_pct(100));
            lv_label_set_long_mode(row, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_margin_bottom(row, 8, 0);
        }
    }

    if (s_page_lbl != nullptr && lv_obj_is_valid(s_page_lbl)) {
        char foot[32];
        std::snprintf(foot, sizeof(foot), "%d / %d", s_page + 1, pages);
        lv_label_set_text(s_page_lbl, foot);
    }
}

void ListUiAsync(void* /*p*/) {
    if (s_alive.load()) {
        RefreshListUi();
    }
}

void ProbeTask(void* /*arg*/) {
    PostStatus("探测模组中…");
    // 该模组不认裸 AT；用产品协议 AT+MODE=1，应答含 SET MODE 1
    std::string resp;
    const bool ok = GetHAL().BtSendCollect("AT+MODE=1\r\n", resp, 3000);
    const bool settled = ok && (resp.find("SET MODE") != std::string::npos ||
                                resp.find("OK") != std::string::npos);
    if (!s_alive.load()) {
        s_busy.store(false);
        vTaskDelete(nullptr);
        return;
    }
    if (resp.size() > 240) {
        resp.resize(240);
        resp += "…";
    }
    char buf[300];
    std::snprintf(buf, sizeof(buf), "%s\n%s", settled ? "探测成功" : "探测失败", resp.c_str());
    PostStatus(buf);
    s_busy.store(false);
    vTaskDelete(nullptr);
}

void ScanBleTask(void* /*arg*/) {
    PostStatus("BLE 扫描中（约 8 秒）…");
    std::vector<HalBtDevice> found;
    std::string detail;
    const bool ok = GetHAL().BleScan(found, detail, 8000);
    if (!s_alive.load()) {
        s_busy.store(false);
        vTaskDelete(nullptr);
        return;
    }
    s_devs = std::move(found);
    s_page = 0;
    if (detail.empty()) {
        detail = ok ? "BLE 扫描完成" : "BLE 扫描失败";
    } else if (!ok) {
        detail = "BLE 扫描失败\n" + detail;
    }
    if (detail.size() > 240) {
        detail.resize(240);
        detail += "…";
    }
    PostStatus(detail.c_str());
    lv_async_call(ListUiAsync, nullptr);
    s_busy.store(false);
    vTaskDelete(nullptr);
}

void OnProbe(lv_event_t* /*e*/) {
    if (s_busy.exchange(true)) {
        return;
    }
    if (xTaskCreatePinnedToCore(ProbeTask, "bt_probe", 6144, nullptr, 5, nullptr, 0) != pdPASS) {
        PostStatus("创建任务失败");
        s_busy.store(false);
    }
}

void OnScanBle(lv_event_t* /*e*/) {
    if (s_busy.exchange(true)) {
        return;
    }
    if (xTaskCreatePinnedToCore(ScanBleTask, "ble_scan", 10240, nullptr, 5, nullptr, 0) != pdPASS) {
        PostStatus("创建任务失败");
        s_busy.store(false);
    }
}

void OnPrev(lv_event_t* /*e*/) {
    if (s_page > 0) {
        --s_page;
        RefreshListUi();
    }
}

void OnNext(lv_event_t* /*e*/) {
    if (s_page + 1 < PageCount()) {
        ++s_page;
        RefreshListUi();
    }
}

void OnDeleted(lv_event_t* e) {
    if (lv_event_get_target(e) != s_scr) {
        return;
    }
    s_alive.store(false);
    s_scr = nullptr;
    s_status = nullptr;
    s_list = nullptr;
    s_page_lbl = nullptr;
    s_devs.clear();
}

}  // namespace

lv_obj_t* AppBluetoothTest::Create() {
    ESP_LOGI(TAG, "create bluetooth test");
    ScreenSetIsHome(false);
    s_alive.store(true);
    s_busy.store(false);
    s_devs.clear();
    s_page = 0;

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_scr = scr;
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, OnDeleted, LV_EVENT_DELETE, nullptr);

    EpdStatusBar bar = ScreenCreateStatusBar(scr);
    if (bar.status_label != nullptr) {
    }

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, LV_VER_RES - bar.height);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, bar.height);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(body, 16, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    s_status = lv_label_create(body);
    lv_obj_set_width(s_status, lv_pct(100));
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_margin_bottom(s_status, 12, 0);
    RefreshReadyHint();

    lv_obj_t* tip = lv_label_create(body);
    lv_obj_set_width(tip, lv_pct(100));
    lv_label_set_long_mode(tip, LV_LABEL_LONG_WRAP);
    lv_label_set_text(tip, "扫描周边使用 ESP BLE");
    lv_obj_set_style_margin_bottom(tip, 12, 0);

    MakeBtn(body, "探测模组", OnProbe);
    MakeBtn(body, "扫描周边", OnScanBle);

    lv_obj_t* nav = lv_obj_create(body);
    lv_obj_remove_style_all(nav);
    lv_obj_set_width(nav, lv_pct(100));
    lv_obj_set_height(nav, kBtnH);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_margin_bottom(nav, 8, 0);
    MakeBtn(nav, "上一页", OnPrev, 160);
    MakeBtn(nav, "下一页", OnNext, 160);

    s_page_lbl = lv_label_create(body);
    lv_label_set_text(s_page_lbl, "1 / 1");
    lv_obj_set_width(s_page_lbl, lv_pct(100));
    lv_obj_set_style_text_align(s_page_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_bottom(s_page_lbl, 8, 0);

    s_list = lv_obj_create(body);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    RefreshListUi();

    VkKey_AttachScreen(scr, kScreenId, VkKeyScreenDesc{AppBluetoothTest::Create});
    return scr;
}
