/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*esp_lcd_epaper_panel_cb_t)(const esp_lcd_panel_handle_t handle, const void *edata, void *user_data);

typedef struct {
    esp_lcd_epaper_panel_cb_t on_epaper_refresh_done;
} epaper_panel_callbacks_t;

typedef struct {
    int busy_gpio_num;
    bool non_copy_mode;
    bool use_fast_full_update;
} esp_lcd_ssd1677_config_t;

typedef enum {
    SSD1677_EPAPER_BITMAP_CURRENT = 0,
    SSD1677_EPAPER_BITMAP_PREVIOUS,
} esp_lcd_ssd1677_bitmap_color_t;

typedef enum {
    SSD1677_EPAPER_REFRESH_FULL = 0,
    SSD1677_EPAPER_REFRESH_FULL_FAST,
    SSD1677_EPAPER_REFRESH_PARTIAL,
    /** 关机末帧：RED+BW 同步全刷（非 BYPASS_RED），边框跟 VCOM。 */
    SSD1677_EPAPER_REFRESH_SHUTDOWN,
    /** 自定义 LUT 四灰度刷新；调用者须先写入两个灰度平面。 */
    SSD1677_EPAPER_REFRESH_GRAY4,
    /** 增量 DU 刷新（CTRL2=0x1C），用于动画诊断；可能因面板批次退化。 */
    SSD1677_EPAPER_REFRESH_DU,
    /** Experimental resident-LUT window refresh (0x0C); not for normal UI. */
    SSD1677_EPAPER_REFRESH_PARTIAL_WARM,
} esp_lcd_ssd1677_refresh_mode_t;

esp_err_t esp_lcd_new_panel_ssd1677(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel);

esp_err_t epaper_panel_refresh_screen(esp_lcd_panel_t *panel);

/** Block until the controller BUSY line is idle.  E-paper updates are
 * asynchronous at the controller level; callers that keep their own frame
 * history must drain BUSY before writing the next plane. */
esp_err_t epaper_panel_wait_busy(esp_lcd_panel_t *panel);

/** Bounded BUSY wait for experimental animation/window refreshes. */
esp_err_t epaper_panel_wait_busy_timeout(esp_lcd_panel_t *panel, uint32_t timeout_ms);

/**
 * Wait for one complete refresh waveform.  SSD1677 BUSY is active high, and
 * MASTER_ACTIVATION can return a few milliseconds before BUSY rises.  This
 * edge-qualified wait first observes the active level (with a short grace
 * period) and only then waits for the idle level, so callers do not rewrite
 * either RAM plane while the waveform is still running.
 */
esp_err_t epaper_panel_wait_refresh_timeout(esp_lcd_panel_t *panel, uint32_t timeout_ms);

/** Reset and reinitialize the controller after an experimental/custom LUT
 * waveform.  The glass image is preserved physically; the caller must redraw
 * a known binary frame before using a differential refresh again. */
esp_err_t epaper_panel_recover(esp_lcd_panel_t *panel);

esp_err_t epaper_panel_set_refresh_mode(esp_lcd_panel_t *panel, esp_lcd_ssd1677_refresh_mode_t mode);

esp_err_t epaper_panel_set_bitmap_color(esp_lcd_panel_t *panel, esp_lcd_ssd1677_bitmap_color_t color);

/** Write the SSD1677 external waveform LUT (112 bytes) while BUSY is idle. */
esp_err_t epaper_panel_write_custom_lut(esp_lcd_panel_t *panel, const uint8_t *lut, size_t size);

esp_err_t epaper_panel_register_event_callbacks(esp_lcd_panel_t *panel, epaper_panel_callbacks_t *cbs, void *user_ctx);

/** 关机专用：0x83 Power Off（等 BUSY）→ 0x10/0x03 Deep Sleep；唤醒需 HW RST。 */
esp_err_t epaper_panel_park_for_shutdown(esp_lcd_panel_t *panel);

#ifdef __cplusplus
}
#endif
