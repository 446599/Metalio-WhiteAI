#include "dual_network_board.h"
#include "application.h"
#include "display.h"
#include "assets/lang_config.h"
#include "settings.h"
#include <esp_log.h>

static const char *TAG = "DualNetworkBoard";

DualNetworkBoard::DualNetworkBoard(gpio_num_t ml307_tx_pin, gpio_num_t ml307_rx_pin, gpio_num_t ml307_dtr_pin, int32_t default_net_type) 
    : Board(), 
      ml307_tx_pin_(ml307_tx_pin), 
      ml307_rx_pin_(ml307_rx_pin), 
      ml307_dtr_pin_(ml307_dtr_pin) {
    
    // 从Settings加载网络类型
    network_type_ = LoadNetworkTypeFromSettings(default_net_type);
    
    // 只初始化当前网络类型对应的板卡
    InitializeCurrentBoard();
}

DualNetworkBoard::DualNetworkBoard(gpio_num_t cellular_tx_pin,
    gpio_num_t cellular_rx_pin,
    gpio_num_t cellular_dtr_pin,
    gpio_num_t cellular_ri_pin,
    int32_t default_net_type)
: Board(), 
cellular_tx_pin_(cellular_tx_pin), 
cellular_rx_pin_(cellular_rx_pin), 
cellular_dtr_pin_(cellular_dtr_pin),
cellular_ri_pin_(cellular_ri_pin) {

// 从Settings加载网络类型
network_type_ = LoadNetworkTypeFromSettings(default_net_type);

// 只初始化当前网络类型对应的板卡
InitializeCurrentBoard();
}

NetworkType DualNetworkBoard::LoadNetworkTypeFromSettings(int32_t default_net_type) {
    Settings settings("network", true);
    // default_net_type: 1=4G(ML307/NT26), 0=WiFi；NVS 无记录时用该默认值
    const int fallback = (default_net_type == 0) ? 0 : 1;
    int network_type = settings.GetInt("type", fallback);
    return network_type == 1 ? NetworkType::ML307 : NetworkType::WIFI;
}

void DualNetworkBoard::SaveNetworkTypeToSettings(NetworkType type) {
    Settings settings("network", true);
    int network_type = (type == NetworkType::ML307) ? 1 : 0;
    settings.SetInt("type", network_type);
}

void DualNetworkBoard::InitializeCurrentBoard() {
    if (network_type_ == NetworkType::ML307) {
        ESP_LOGI(TAG, "Initialize ML307 board");

        current_board_ = std::make_unique<Nt26Board>(
            cellular_tx_pin_, cellular_rx_pin_, cellular_dtr_pin_, cellular_ri_pin_);
        // current_board_ = std::make_unique<Ml307Board>(ml307_tx_pin_, ml307_rx_pin_, ml307_dtr_pin_);
    } else {
        ESP_LOGI(TAG, "Initialize WiFi board");
        current_board_ = std::make_unique<WifiBoard>();
    }
}

void DualNetworkBoard::SwitchToNetworkType(NetworkType type) {
    if (type == network_type_) {
        return;
    }
    SaveNetworkTypeToSettings(type);
    auto* display = GetDisplay();
    if (display != nullptr) {
        if (type == NetworkType::ML307) {
            display->ShowNotification(Lang::Strings::SWITCH_TO_4G_NETWORK);
        } else {
            display->ShowNotification(Lang::Strings::SWITCH_TO_WIFI_NETWORK);
        }
    } else {
        ESP_LOGI(TAG, "switch network to %s (no display)",
                 type == NetworkType::WIFI ? "WiFi" : "4G");
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    Application::GetInstance().Reboot();
}

void DualNetworkBoard::SwitchNetworkType() {
    SwitchToNetworkType(network_type_ == NetworkType::WIFI ? NetworkType::ML307
                                                           : NetworkType::WIFI);
}

 
std::string DualNetworkBoard::GetBoardType() {
    return current_board_->GetBoardType();
}

void DualNetworkBoard::StartNetwork() {
    auto display = Board::GetInstance().GetDisplay();
    
    if (network_type_ == NetworkType::WIFI) {
        display->SetStatus(Lang::Strings::CONNECTING);
    } else {
        display->SetStatus(Lang::Strings::DETECTING_MODULE);
    }
    current_board_->StartNetwork();
}

void DualNetworkBoard::StartProvisioningIfRequested() {
    if (network_type_ != NetworkType::WIFI) {
        return;
    }
    auto* wifi = dynamic_cast<WifiBoard*>(current_board_.get());
    if (wifi == nullptr || !wifi->wifi_config_requested()) {
        return;
    }
    ESP_LOGI(TAG, "provisioning requested: starting Wi-Fi configuration AP");
    // Deliberately blocking: the configuration page stores credentials and
    // reboots the board, so no dashboard provider should start meanwhile.
    wifi->EnterWifiConfigModeForProvisioning();
}

NetworkInterface* DualNetworkBoard::GetNetwork() {
    return current_board_->GetNetwork();
}

const char* DualNetworkBoard::GetNetworkStateIcon() {
    return current_board_->GetNetworkStateIcon();
}

void DualNetworkBoard::SetPowerSaveMode(bool enabled) {
    current_board_->SetPowerSaveMode(enabled);
}

std::string DualNetworkBoard::GetBoardJson() {   
    return current_board_->GetBoardJson();
}

std::string DualNetworkBoard::GetDeviceStatusJson() {
    return current_board_->GetDeviceStatusJson();
}
