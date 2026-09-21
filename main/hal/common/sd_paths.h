#pragma once

/**
 * @file sd_paths.h
 * @brief SD layout for hardware-test firmware under /sdcard/metalio/e-ink/
 *
 * Mount point stays `/sdcard` (SdCardManager). App data root is created on mount
 * so the card root stays clean; feature subdirs are not pre-created in this build.
 */

#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <esp_log.h>

#ifdef __cplusplus
extern "C" {
#endif

/** VFS mount point — do not change without updating SdCardManager. */
#ifndef SD_MOUNT_POINT
#define SD_MOUNT_POINT "/sdcard"
#endif

/** Product / test data root on SD. */
#ifndef SD_APP_ROOT
#define SD_APP_ROOT "/sdcard/metalio/e-ink"
#endif

/**
 * On-screen / user-facing path: strip VFS mount (`/sdcard`) so hints match USB MSC
 * card root (e.g. `metalio/e-ink`). Avoids users creating a literal `/sdcard` folder.
 */
static inline const char* SdUserPath(const char* posix_abs) {
    if (posix_abs == NULL || posix_abs[0] == '\0') {
        return "";
    }
    const size_t n = sizeof(SD_MOUNT_POINT) - 1;
    if (strncmp(posix_abs, SD_MOUNT_POINT, n) == 0) {
        if (posix_abs[n] == '/') {
            return posix_abs + n + 1;
        }
        if (posix_abs[n] == '\0') {
            return ".";
        }
    }
    return posix_abs;
}

static inline int SdEnsureDir(const char* path) {
    struct stat st;
    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 1 : 0;
    }
    if (mkdir(path, 0755) == 0) {
        return 1;
    }
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 1 : 0;
    }
    return 0;
}

/** Create /sdcard/metalio/e-ink (best-effort). Returns 1 on ok. */
static inline int SdEnsureAppLayout(void) {
    static const char* kTag = "sd_paths";
    if (!SdEnsureDir("/sdcard/metalio") || !SdEnsureDir(SD_APP_ROOT)) {
        ESP_LOGW(kTag, "ensure app root %s failed errno=%d", SD_APP_ROOT, errno);
        return 0;
    }
    return 1;
}

#ifdef __cplusplus
}
#endif
