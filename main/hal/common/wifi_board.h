#ifndef WIFI_BOARD_H
#define WIFI_BOARD_H

#include "board.h"

class WifiBoard : public Board {
protected:
    bool wifi_config_mode_ = false;
    void EnterWifiConfigMode();
    virtual std::string GetBoardJson() override;

public:
    WifiBoard();
    // True when the force_ap flag was set before this boot; the product boot
    // path uses it to enter the configuration AP before providers start.
    bool wifi_config_requested() const { return wifi_config_mode_; }
    // Starts the config AP and never returns: the configuration web page
    // reboots the board once credentials are stored.
    void EnterWifiConfigModeForProvisioning() { EnterWifiConfigMode(); }
    virtual std::string GetBoardType() override;
    virtual void StartNetwork() override;
    virtual NetworkInterface* GetNetwork() override;
    virtual const char* GetNetworkStateIcon() override;
    virtual void SetPowerSaveMode(bool enabled) override;
    virtual void ResetWifiConfiguration();
    virtual AudioCodec* GetAudioCodec() override { return nullptr; }
    virtual std::string GetDeviceStatusJson() override;
};

#endif // WIFI_BOARD_H
