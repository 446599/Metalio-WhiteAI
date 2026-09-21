from pathlib import Path

def replace(path,old,new):
 p=Path(path);s=p.read_text();assert s.count(old)==1,(path,old[:80],s.count(old));p.write_text(s.replace(old,new,1))

replace('components/esp-wifi-connect/wifi_station.cc', '''    wifi_ap_record_t ap_info;
    ESP_ERROR_CHECK(esp_wifi_sta_get_ap_info(&ap_info));
    return ap_info.rssi;''','''    wifi_ap_record_t ap_info{};
    return esp_wifi_sta_get_ap_info(&ap_info)==ESP_OK ? ap_info.rssi : -127;''')
replace('components/esp-wifi-connect/wifi_station.cc', '''    wifi_ap_record_t ap_info;
    ESP_ERROR_CHECK(esp_wifi_sta_get_ap_info(&ap_info));
    return ap_info.primary;''','''    wifi_ap_record_t ap_info{};
    return esp_wifi_sta_get_ap_info(&ap_info)==ESP_OK ? ap_info.primary : 0;''')
replace('components/esp-wifi-connect/wifi_station.cc', '''        reconnect_count_=0;
        manual_setup_.store(false);''','''        // A late disconnect from a cancelled attempt must not retry its
        // uncommitted password. Only the saved-profile scan may reconnect.
        reconnect_count_=keep_connection ? 0 : MAX_RECONNECT_COUNT;
        manual_setup_.store(false);''')
replace('components/esp-wifi-connect/include/wifi_station.h', '    std::vector<WifiScanAp> list_scan_results_;', '    std::vector<WifiScanAp> list_scan_results_;\n    bool list_scan_ok_ = false;')
replace('components/esp-wifi-connect/wifi_station.cc', '''    list_scan_results_.clear();
    xEventGroupClearBits(event_group_, WIFI_EVENT_SCAN_LIST_DONE);''','''    list_scan_results_.clear();
    list_scan_ok_=false;
    xEventGroupClearBits(event_group_, WIFI_EVENT_SCAN_LIST_DONE);''')
replace('components/esp-wifi-connect/wifi_station.cc', '''    if ((bits & WIFI_EVENT_SCAN_LIST_DONE) == 0) {''','''    if ((bits & WIFI_EVENT_SCAN_LIST_DONE) == 0 || !list_scan_ok_) {''')
replace('components/esp-wifi-connect/wifi_station.cc', '''    esp_wifi_scan_get_ap_num(&ap_num);
    ap_num = std::min<uint16_t>(ap_num, 64);''','''    if(esp_wifi_scan_get_ap_num(&ap_num)!=ESP_OK) {
        esp_wifi_clear_ap_list();
        if(listing_scan_.load()) xEventGroupSetBits(event_group_,WIFI_EVENT_SCAN_LIST_DONE);
        return;
    }
    ap_num = std::min<uint16_t>(ap_num, 64);''')
replace('components/esp-wifi-connect/wifi_station.cc', '''        esp_wifi_scan_get_ap_records(&ap_num, ap_records);
        std::sort''','''        if(esp_wifi_scan_get_ap_records(&ap_num, ap_records)!=ESP_OK) {
            free(ap_records);esp_wifi_clear_ap_list();
            if(listing_scan_.load()) xEventGroupSetBits(event_group_,WIFI_EVENT_SCAN_LIST_DONE);
            return;
        }
        std::sort''')
replace('components/esp-wifi-connect/wifi_station.cc', '''        ESP_LOGI(TAG, "ScanForList done: %d APs", static_cast<int>(list_scan_results_.size()));''','''        list_scan_ok_=true;
        ESP_LOGI(TAG, "ScanForList done: %d APs", static_cast<int>(list_scan_results_.size()));''')
replace('components/esp-wifi-connect/wifi_station.cc', '''    } else if (event_id == WIFI_EVENT_SCAN_DONE) {
        this_->HandleScanResult();''','''    } else if (event_id == WIFI_EVENT_SCAN_DONE) {
        const auto* scan=static_cast<wifi_event_sta_scan_done_t*>(event_data);
        if(scan && scan->status!=0) {
            esp_wifi_clear_ap_list();
            if(this_->listing_scan_.load()) xEventGroupSetBits(this_->event_group_,WIFI_EVENT_SCAN_LIST_DONE);
            return;
        }
        this_->HandleScanResult();''')
replace('tools/preview_input_support.py','#include "notes/note_writer.h"','#include "notes/note_writer.h"\n#include <climits>')
