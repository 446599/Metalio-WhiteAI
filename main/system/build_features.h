#pragma once
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

// Host tests default to the same product configuration as firmware. The
// optional macro can only enable the experimental discovery variant.
namespace device {
#if defined(CONFIG_WHITEAI_EXPERIMENTAL_BLE_DISCOVERY) && CONFIG_WHITEAI_EXPERIMENTAL_BLE_DISCOVERY
inline constexpr bool kBleDiscoveryEnabled = true;
#else
inline constexpr bool kBleDiscoveryEnabled = false;
#endif
}
