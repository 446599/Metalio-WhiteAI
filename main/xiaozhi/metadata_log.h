#pragma once
#ifdef ESP_PLATFORM
#include <esp_log.h>
#define XZ_META(...) ESP_LOGI("XiaozhiMeta", __VA_ARGS__)
#else
namespace xiaozhi { template<class... T> inline void QuietMetadata(const T&...) {} }
#define XZ_META(...) ::xiaozhi::QuietMetadata(__VA_ARGS__)
#endif
