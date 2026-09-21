// Minimal static language strings for Metalio hardware-test firmware.
#pragma once

#include <string_view>

#ifndef zh_cn
    #define zh_cn
#endif

namespace Lang {
    constexpr const char* CODE = "zh-CN";

    namespace Strings {
        constexpr const char* ACCESS_VIA_BROWSER = "，浏览器访问 ";
        constexpr const char* BATTERY_NEED_CHARGE = "电量低，请充电";
        constexpr const char* CONNECTED_TO = "已连接 ";
        constexpr const char* CONNECTING = "连接中...";
        constexpr const char* CONNECT_TO = "连接 ";
        constexpr const char* CONNECT_TO_HOTSPOT = "手机连接热点 ";
        constexpr const char* DETECTING_MODULE = "检测模组...";
        constexpr const char* ENTERING_WIFI_CONFIG_MODE = "进入配网模式...";
        constexpr const char* ERROR = "错误";
        constexpr const char* MAX_VOLUME = "最大音量";
        constexpr const char* MUTED = "已静音";
        constexpr const char* PIN_ERROR = "请插入 SIM 卡";
        constexpr const char* REGISTERING_NETWORK = "等待网络...";
        constexpr const char* REG_ERROR = "无法接入网络，请检查流量卡状态";
        constexpr const char* SCANNING_WIFI = "扫描 Wi-Fi...";
        constexpr const char* SWITCH_TO_4G_NETWORK = "切换到 4G...";
        constexpr const char* SWITCH_TO_WIFI_NETWORK = "切换到 Wi-Fi...";
        constexpr const char* VOLUME = "音量 ";
        constexpr const char* WIFI_CONFIG_MODE = "配网模式";
    }

    // 硬件测试固件不嵌入提示音；保留符号避免板级 Alert 调用编译失败。
    namespace Sounds {
        static constexpr std::string_view OGG_WIFICONFIG{};
        static constexpr std::string_view OGG_ERR_PIN{};
        static constexpr std::string_view OGG_ERR_REG{};
    }
}
