/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ssd1677.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_ssd1677_commands.h"

static const char *TAG = "lcd_panel.ssd1677";

typedef struct {
    esp_lcd_epaper_panel_cb_t callback_ptr;
    void *args;
} epaper_panel_callback_t;

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    bool reset_level;
    int busy_gpio_num;
    int gap_x;
    int gap_y;
    epaper_panel_callback_t epaper_refresh_done_isr_callback;
    esp_lcd_ssd1677_bitmap_color_t bitmap_color;
    esp_lcd_ssd1677_refresh_mode_t refresh_mode;
    bool _non_copy_mode;
    bool _mirror_x;
    bool _mirror_y;
    bool _swap_xy;
    bool _invert_color;
    uint8_t *_framebuffer;
} epaper_panel_t;

static inline uint8_t byte_reverse(uint8_t data);
static esp_err_t process_bitmap(esp_lcd_panel_t *panel, int len_x, int len_y, int src_stride, int dst_stride,
                                const void *color_data);
static esp_err_t panel_epaper_wait_busy(esp_lcd_panel_t *panel);
static void epaper_driver_gpio_isr_handler(void *arg);
static esp_err_t epaper_set_ram_area(esp_lcd_panel_io_handle_t io, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
static esp_err_t panel_epaper_set_vram(esp_lcd_panel_io_handle_t io, uint8_t cmd, const uint8_t *bitmap, size_t size);
static esp_err_t epaper_panel_del(esp_lcd_panel_t *panel);
static esp_err_t epaper_panel_reset(esp_lcd_panel_t *panel);
static esp_err_t epaper_panel_init(esp_lcd_panel_t *panel);
static esp_err_t epaper_panel_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                          const void *color_data);
static esp_err_t epaper_panel_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t epaper_panel_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t epaper_panel_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t epaper_panel_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t epaper_panel_disp_on_off(esp_lcd_panel_t *panel, bool on_off);

static void epaper_driver_gpio_isr_handler(void *arg)
{
    epaper_panel_t *epaper_panel = arg;
    if (epaper_panel->busy_gpio_num >= 0) {
        gpio_intr_disable(epaper_panel->busy_gpio_num);
    }
    if (epaper_panel->epaper_refresh_done_isr_callback.callback_ptr) {
        (epaper_panel->epaper_refresh_done_isr_callback.callback_ptr)(&(epaper_panel->base), NULL,
                                                                    epaper_panel->epaper_refresh_done_isr_callback.args);
    }
}

esp_err_t epaper_panel_register_event_callbacks(esp_lcd_panel_t *panel, epaper_panel_callbacks_t *cbs, void *user_ctx)
{
    ESP_RETURN_ON_FALSE(panel && cbs, ESP_ERR_INVALID_ARG, TAG, "invalid arg");
    epaper_panel_t *epaper_panel = __containerof(panel, epaper_panel_t, base);
    epaper_panel->epaper_refresh_done_isr_callback.callback_ptr = cbs->on_epaper_refresh_done;
    epaper_panel->epaper_refresh_done_isr_callback.args = user_ctx;
    return ESP_OK;
}

esp_err_t epaper_panel_set_bitmap_color(esp_lcd_panel_t *panel, esp_lcd_ssd1677_bitmap_color_t color)
{
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "panel is NULL");
    epaper_panel_t *epaper_panel = __containerof(panel, epaper_panel_t, base);
    epaper_panel->bitmap_color = color;
    return ESP_OK;
}

esp_err_t epaper_panel_set_refresh_mode(esp_lcd_panel_t *panel, esp_lcd_ssd1677_refresh_mode_t mode)
{
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "panel is NULL");
    epaper_panel_t *epaper_panel = __containerof(panel, epaper_panel_t, base);
    epaper_panel->refresh_mode = mode;
    return ESP_OK;
}

static esp_err_t epaper_set_ram_area(esp_lcd_panel_io_handle_t io, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint16_t yrev = SSD1677_PANEL_HEIGHT - y - h;

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, SSD1677_CMD_DATA_ENTRY_MODE,
                                                  (uint8_t[]) { SSD1677_PARAM_DATA_ENTRY_MODE_XY_INC_Y_DEC }, 1),
                        TAG, "DATA_ENTRY_MODE err");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, SSD1677_CMD_SET_RAMX_START_END_POS, (uint8_t[]) {
        x & 0xFF, (uint8_t)(x >> 8),
        (uint8_t)((x + w - 1) & 0xFF), (uint8_t)((x + w - 1) >> 8)
    }, 4), TAG, "SET_RAMX err");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, SSD1677_CMD_SET_RAMY_START_END_POS, (uint8_t[]) {
        (uint8_t)((yrev + h - 1) & 0xFF), (uint8_t)((yrev + h - 1) >> 8),
        (uint8_t)(yrev & 0xFF), (uint8_t)(yrev >> 8)
    }, 4), TAG, "SET_RAMY err");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, SSD1677_CMD_SET_INIT_X_ADDR_COUNTER, (uint8_t[]) {
        x & 0xFF, (uint8_t)(x >> 8)
    }, 2), TAG, "SET_X_COUNTER err");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, SSD1677_CMD_SET_INIT_Y_ADDR_COUNTER, (uint8_t[]) {
        (uint8_t)((yrev + h - 1) & 0xFF), (uint8_t)((yrev + h - 1) >> 8)
    }, 2), TAG, "SET_Y_COUNTER err");

    return ESP_OK;
}

static esp_err_t panel_epaper_wait_busy_timeout(esp_lcd_panel_t *panel, uint32_t timeout_ms)
{
    epaper_panel_t *epaper_panel = __containerof(panel, epaper_panel_t, base);
    const int64_t deadline_us = esp_timer_get_time() + (int64_t) timeout_ms * 1000;
    while (gpio_get_level(epaper_panel->busy_gpio_num)) {
        if (esp_timer_get_time() >= deadline_us) {
            ESP_LOGW(TAG, "BUSY timeout (%ums); aborting experimental refresh", (unsigned) timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    return ESP_OK;
}

static esp_err_t panel_epaper_wait_busy(esp_lcd_panel_t *panel)
{
    return panel_epaper_wait_busy_timeout(panel, 15000);
}

static esp_err_t panel_epaper_wait_refresh_timeout(esp_lcd_panel_t *panel, uint32_t timeout_ms)
{
    epaper_panel_t *epaper_panel = __containerof(panel, epaper_panel_t, base);
    const int64_t start_us = esp_timer_get_time();
    const int64_t deadline_us = start_us + (int64_t) timeout_ms * 1000;
    const int64_t arm_deadline_us = start_us + 20000; // activation assertion grace

    /*
     * Unlike a command wait, a refresh wait must not accept an idle BUSY
     * sample immediately after 0x20.  The SSD1677 may assert BUSY a few
     * milliseconds after MASTER_ACTIVATION; FreeInk calls this the
     * refresh-completion wait and gives the edge a bounded grace period.
     */
    while (!gpio_get_level(epaper_panel->busy_gpio_num)) {
        const int64_t now_us = esp_timer_get_time();
        if (now_us >= arm_deadline_us || now_us >= deadline_us) {
            // A controller that completed before the pin was sampled (or a
            // no-op activation) is already idle.  Do not turn this into a
            // false timeout; the important invariant is that the pin is low.
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    while (gpio_get_level(epaper_panel->busy_gpio_num)) {
        if (esp_timer_get_time() >= deadline_us) {
            ESP_LOGW(TAG, "refresh BUSY timeout (%ums)", (unsigned) timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_OK;
}

esp_err_t epaper_panel_wait_busy(esp_lcd_panel_t *panel)
{
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "panel is NULL");
    return panel_epaper_wait_busy(panel);
}

esp_err_t epaper_panel_wait_busy_timeout(esp_lcd_panel_t *panel, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(panel && timeout_ms > 0, ESP_ERR_INVALID_ARG, TAG, "invalid wait timeout");
    return panel_epaper_wait_busy_timeout(panel, timeout_ms);
}

esp_err_t epaper_panel_wait_refresh_timeout(esp_lcd_panel_t *panel, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(panel && timeout_ms > 0, ESP_ERR_INVALID_ARG, TAG, "invalid refresh wait timeout");
    return panel_epaper_wait_refresh_timeout(panel, timeout_ms);
}

esp_err_t epaper_panel_recover(esp_lcd_panel_t *panel)
{
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "panel is NULL");
    epaper_panel_t *epaper_panel = __containerof(panel, epaper_panel_t, base);

    /* A custom 0x0C/0xCC activation can leave the SSD1677 analogue engine in
     * its resident-LUT state.  A hardware reset is the deterministic boundary
     * used by Paper Mono when leaving that path; do not wait 15 s on a BUSY pin
     * that the experimental waveform may have left asserted. */
    gpio_intr_disable(epaper_panel->busy_gpio_num);
    if (epaper_panel->reset_gpio_num >= 0) {
        ESP_RETURN_ON_ERROR(gpio_set_level(epaper_panel->reset_gpio_num, epaper_panel->reset_level), TAG,
                            "recovery RST assert failed");
        vTaskDelay(pdMS_TO_TICKS(20));
        ESP_RETURN_ON_ERROR(gpio_set_level(epaper_panel->reset_gpio_num, !epaper_panel->reset_level), TAG,
                            "recovery RST release failed");
        vTaskDelay(pdMS_TO_TICKS(20));
    } else {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_SWRST, NULL, 0), TAG,
                            "recovery SWRST failed");
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    esp_err_t wait_err = panel_epaper_wait_busy_timeout(panel, 2000);
    if (wait_err != ESP_OK) {
        ESP_LOGW(TAG, "recovery BUSY did not settle: %s", esp_err_to_name(wait_err));
        return wait_err;
    }
    return epaper_panel_init(panel);
}

static esp_err_t panel_epaper_set_vram(esp_lcd_panel_io_handle_t io, uint8_t cmd, const uint8_t *bitmap, size_t size)
{
    if (bitmap && size > 0) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, cmd, bitmap, size), TAG, "tx_color err");
    }
    return ESP_OK;
}

esp_err_t epaper_panel_refresh_screen(esp_lcd_panel_t *panel)
{
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "panel is NULL");
    epaper_panel_t *epaper_panel = __containerof(panel, epaper_panel_t, base);

    uint8_t ctrl1[2] = { SSD1677_PARAM_DISP_UPDATE_CTRL1_NORMAL, 0x00 };
    uint8_t ctrl2 = SSD1677_PARAM_DISP_UPDATE_PARTIAL;

    switch (epaper_panel->refresh_mode) {
    case SSD1677_EPAPER_REFRESH_FULL:
        ctrl1[0] = SSD1677_PARAM_DISP_UPDATE_CTRL1_BYPASS_RED;
        ctrl2 = SSD1677_PARAM_DISP_UPDATE_FULL;
        break;
    case SSD1677_EPAPER_REFRESH_FULL_FAST:
        /* SSD1677's tested full-update path bypasses the RED plane.  The
         * UC8279C DTM1/DTM2 convention used by EegoRead is not equivalent to
         * this controller's RED VRAM command. */
        ctrl1[0] = SSD1677_PARAM_DISP_UPDATE_CTRL1_BYPASS_RED;
        ctrl2 = SSD1677_PARAM_DISP_UPDATE_FULL_FAST;
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_WRITE_TEMP_REG,
                                                      (uint8_t[]) { 0x6A }, 1), TAG, "WRITE_TEMP err");
        break;
    case SSD1677_EPAPER_REFRESH_SHUTDOWN:
        // 关机全刷：RED 参与差分（非 BYPASS），OTP 全刷 LUT，边框跟 VCOM
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_TEMP_SENSOR_CTRL,
                                                      (uint8_t[]) { SSD1677_PARAM_TEMP_SENSOR_INTERNAL }, 1),
                            TAG, "TEMP_SENSOR err");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_SET_BORDER_WAVEFORM,
                                                      (uint8_t[]) { SSD1677_PARAM_BORDER_WAVEFORM_VCOM }, 1),
                            TAG, "BORDER shutdown err");
        ctrl1[0] = SSD1677_PARAM_DISP_UPDATE_CTRL1_NORMAL;
        ctrl2 = SSD1677_PARAM_DISP_UPDATE_FULL;
        break;
    case SSD1677_EPAPER_REFRESH_PARTIAL:
        // FULL_FAST 会写死温度寄存器；局刷前恢复内部传感器，避免波形选错
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_TEMP_SENSOR_CTRL,
                                                      (uint8_t[]) { SSD1677_PARAM_TEMP_SENSOR_INTERNAL }, 1),
                            TAG, "TEMP_SENSOR err");
        // 局刷边框跟 VCOM，减轻窗口边缘发灰/残影观感（不增加刷新次数）
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_SET_BORDER_WAVEFORM,
                                                      (uint8_t[]) { SSD1677_PARAM_BORDER_WAVEFORM_VCOM }, 1),
                            TAG, "BORDER partial err");
        ctrl1[0] = SSD1677_PARAM_DISP_UPDATE_CTRL1_NORMAL;
        ctrl2 = SSD1677_PARAM_DISP_UPDATE_PARTIAL;
        break;
    case SSD1677_EPAPER_REFRESH_GRAY4:
        /* External LUT activation.  Unlike the OTP partial sequence this does
         * not reload the factory waveform, so both RAM planes select the
         * custom grayscale LUT written by epaper_panel_write_custom_lut(). */
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_TEMP_SENSOR_CTRL,
                                                      (uint8_t[]) { SSD1677_PARAM_TEMP_SENSOR_INTERNAL }, 1),
                            TAG, "TEMP_SENSOR gray err");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_SET_BORDER_WAVEFORM,
                                                      (uint8_t[]) { SSD1677_PARAM_BORDER_WAVEFORM_VCOM }, 1),
                            TAG, "BORDER gray err");
        ctrl1[0] = SSD1677_PARAM_DISP_UPDATE_CTRL1_NORMAL;
        ctrl2 = SSD1677_PARAM_DISP_UPDATE_EXTERNAL_LUT;
        break;
    case SSD1677_EPAPER_REFRESH_DU:
        /* Incremental differential update used by the Xteink community
         * driver.  Unlike the vendor 0xFC sequence this does not reload the
         * temperature register or recycle the analogue rails, which is what
         * makes it suitable for animation.  Keep it opt-in: some SSD1677
         * batches silently promote 0x1C to a long full update. */
        ctrl1[0] = SSD1677_PARAM_DISP_UPDATE_CTRL1_NORMAL;
        ctrl2 = SSD1677_PARAM_DISP_UPDATE_DU;
        break;
    case SSD1677_EPAPER_REFRESH_PARTIAL_WARM:
        /* Paper Mono's experimental resident-LUT path. Its caller owns the
         * waveform and rail state; normal UI updates must use PARTIAL so
         * temperature and the factory partial LUT are loaded each time. */
        ctrl1[0] = SSD1677_PARAM_DISP_UPDATE_CTRL1_NORMAL;
        ctrl2 = SSD1677_PARAM_DISP_UPDATE_PARTIAL_WARM;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_DISP_UPDATE_CTRL1, ctrl1, 2), TAG,
                        "DISP_UPDATE_CTRL1 err");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_DISP_UPDATE_CTRL2,
                                                  (uint8_t[]) { ctrl2 }, 1), TAG, "DISP_UPDATE_CTRL2 err");

    gpio_intr_enable(epaper_panel->busy_gpio_num);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_MASTER_ACTIVATION, NULL, 0), TAG,
                        "MASTER_ACTIVATION err");
    return ESP_OK;
}

esp_err_t epaper_panel_write_custom_lut(esp_lcd_panel_t *panel, const uint8_t *lut, size_t size)
{
    ESP_RETURN_ON_FALSE(panel && lut && size >= 110, ESP_ERR_INVALID_ARG, TAG, "invalid LUT");
    epaper_panel_t *epaper_panel = __containerof(panel, epaper_panel_t, base);
    ESP_RETURN_ON_ERROR(panel_epaper_wait_busy(panel), TAG, "BUSY before LUT");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_WRITE_LUT, lut, 105), TAG,
                        "WRITE_LUT err");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_GATE_VOLTAGE, lut + 105, 1), TAG,
                        "GATE_VOLTAGE err");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_SOURCE_VOLTAGE, lut + 106, 3), TAG,
                        "SOURCE_VOLTAGE err");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_WRITE_VCOM, lut + 109, 1), TAG,
                        "WRITE_VCOM err");
    return ESP_OK;
}

esp_err_t esp_lcd_new_panel_ssd1677(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *const panel_dev_config,
                                    esp_lcd_panel_handle_t *const ret_panel)
{
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid arg");
    esp_lcd_ssd1677_config_t *ssd1677_conf = panel_dev_config->vendor_config;
    ESP_RETURN_ON_FALSE(ssd1677_conf, ESP_ERR_INVALID_ARG, TAG, "vendor_config is NULL");

    epaper_panel_t *epaper_panel = calloc(1, sizeof(epaper_panel_t));
    ESP_RETURN_ON_FALSE(epaper_panel, ESP_ERR_NO_MEM, TAG, "no mem");

    epaper_panel->io = io;
    epaper_panel->reset_gpio_num = panel_dev_config->reset_gpio_num;
    epaper_panel->busy_gpio_num = ssd1677_conf->busy_gpio_num;
    epaper_panel->reset_level = panel_dev_config->flags.reset_active_high;
    epaper_panel->_non_copy_mode = ssd1677_conf->non_copy_mode;
    epaper_panel->bitmap_color = SSD1677_EPAPER_BITMAP_CURRENT;
    epaper_panel->refresh_mode = SSD1677_EPAPER_REFRESH_FULL_FAST;
    epaper_panel->base.del = epaper_panel_del;
    epaper_panel->base.reset = epaper_panel_reset;
    epaper_panel->base.init = epaper_panel_init;
    epaper_panel->base.draw_bitmap = epaper_panel_draw_bitmap;
    epaper_panel->base.invert_color = epaper_panel_invert_color;
    epaper_panel->base.set_gap = epaper_panel_set_gap;
    epaper_panel->base.mirror = epaper_panel_mirror;
    epaper_panel->base.swap_xy = epaper_panel_swap_xy;
    epaper_panel->base.disp_on_off = epaper_panel_disp_on_off;
    *ret_panel = &(epaper_panel->base);

    if (!epaper_panel->_non_copy_mode) {
        epaper_panel->_framebuffer = heap_caps_malloc(SSD1677_PANEL_BUFFER_SIZE,
                                                       MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        ESP_RETURN_ON_FALSE(epaper_panel->_framebuffer, ESP_ERR_NO_MEM, TAG, "framebuffer alloc failed");
    }

    if (epaper_panel->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << epaper_panel->reset_gpio_num,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "RST gpio err");
    }

    if (epaper_panel->busy_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_INPUT,
            .pull_down_en = 0x01,
            .pin_bit_mask = 1ULL << epaper_panel->busy_gpio_num,
            .intr_type = GPIO_INTR_NEGEDGE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "BUSY gpio err");
        ESP_RETURN_ON_ERROR(gpio_isr_handler_add(epaper_panel->busy_gpio_num, epaper_driver_gpio_isr_handler, epaper_panel),
                            TAG, "BUSY isr err");
        gpio_intr_disable(epaper_panel->busy_gpio_num);
    }

    ESP_LOGI(TAG, "new ssd1677 panel @%p", epaper_panel);
    return ESP_OK;
}

static esp_err_t epaper_panel_del(esp_lcd_panel_t *panel)
{
    epaper_panel_t *epaper_panel = __containerof(panel, epaper_panel_t, base);
    if (epaper_panel->reset_gpio_num >= 0) {
        gpio_reset_pin(epaper_panel->reset_gpio_num);
    }
    if (epaper_panel->busy_gpio_num >= 0) {
        gpio_isr_handler_remove(epaper_panel->busy_gpio_num);
        gpio_reset_pin(epaper_panel->busy_gpio_num);
    }
    if (epaper_panel->_framebuffer) {
        heap_caps_free(epaper_panel->_framebuffer);
        epaper_panel->_framebuffer = NULL;
    }
    free(epaper_panel);
    return ESP_OK;
}

static esp_err_t epaper_panel_reset(esp_lcd_panel_t *panel)
{
    epaper_panel_t *epaper_panel = __containerof(panel, epaper_panel_t, base);
    if (epaper_panel->reset_gpio_num >= 0) {
        ESP_RETURN_ON_ERROR(gpio_set_level(epaper_panel->reset_gpio_num, epaper_panel->reset_level), TAG, "RST low err");
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_RETURN_ON_ERROR(gpio_set_level(epaper_panel->reset_gpio_num, !epaper_panel->reset_level), TAG, "RST high err");
        vTaskDelay(pdMS_TO_TICKS(10));
    } else {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_SWRST, NULL, 0), TAG, "SWRST err");
    }
    return panel_epaper_wait_busy(panel);
}

static esp_err_t epaper_panel_init(esp_lcd_panel_t *panel)
{
    epaper_panel_t *epaper_panel = __containerof(panel, epaper_panel_t, base);
    esp_lcd_panel_io_handle_t io = epaper_panel->io;

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, SSD1677_CMD_SWRST, NULL, 0), TAG, "SWRST err");
    vTaskDelay(pdMS_TO_TICKS(10));
    /*
     * The SSD1677 reset command is asynchronous.  FreeInk's reference
     * sequence drains BUSY before sending the setup registers; without this
     * wait, the first booster/driver commands can be accepted while the
     * controller is still in reset and a later 0x1C/0x0C activation may leave
     * BUSY asserted indefinitely.
     */
    ESP_RETURN_ON_ERROR(panel_epaper_wait_busy_timeout(panel, 5000), TAG, "SWRST busy err");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, SSD1677_CMD_TEMP_SENSOR_CTRL,
                                                  (uint8_t[]) { SSD1677_PARAM_TEMP_SENSOR_INTERNAL }, 1), TAG,
                        "TEMP_SENSOR err");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, SSD1677_CMD_BOOSTER_SOFT_START,
                                                  (uint8_t[]) { 0xAE, 0xC7, 0xC3, 0xC0, 0x80 }, 5), TAG,
                        "BOOSTER err");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, SSD1677_CMD_DRIVER_OUTPUT_CTRL, (uint8_t[]) {
        (uint8_t)((SSD1677_PANEL_HEIGHT - 1) & 0xFF),
        (uint8_t)((SSD1677_PANEL_HEIGHT - 1) >> 8),
        0x02
    }, 3), TAG, "DRIVER_OUTPUT err");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, SSD1677_CMD_SET_BORDER_WAVEFORM,
                                                  (uint8_t[]) { SSD1677_PARAM_BORDER_WAVEFORM }, 1), TAG,
                        "BORDER err");

    ESP_RETURN_ON_ERROR(epaper_set_ram_area(io, 0, 0, SSD1677_PANEL_WIDTH, SSD1677_PANEL_HEIGHT), TAG, "RAM area err");

    /*
     * Clear both controller RAM roles using the SSD1677 auto-write commands.
     * This is part of the panel's normal initialization flow (0x46/0x47 with
     * 0xF7), and is also needed after a hardware recovery reset so that the
     * next differential update has a valid controller baseline.
     */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, SSD1677_CMD_AUTO_WRITE_BW_RAM,
                                                  (uint8_t[]) { 0xF7 }, 1), TAG,
                        "AUTO_WRITE_BW err");
    ESP_RETURN_ON_ERROR(panel_epaper_wait_busy_timeout(panel, 5000), TAG, "AUTO_WRITE_BW busy err");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, SSD1677_CMD_AUTO_WRITE_RED_RAM,
                                                  (uint8_t[]) { 0xF7 }, 1), TAG,
                        "AUTO_WRITE_RED err");
    ESP_RETURN_ON_ERROR(panel_epaper_wait_busy_timeout(panel, 5000), TAG, "AUTO_WRITE_RED busy err");
    return ESP_OK;
}

static esp_err_t epaper_panel_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                          const void *color_data)
{
    epaper_panel_t *epaper_panel = __containerof(panel, epaper_panel_t, base);
    if (gpio_get_level(epaper_panel->busy_gpio_num)) {
        return ESP_ERR_NOT_FINISHED;
    }

    x_start += epaper_panel->gap_x;
    x_end += epaper_panel->gap_x;
    y_start += epaper_panel->gap_y;
    y_end += epaper_panel->gap_y;

    if (epaper_panel->_non_copy_mode) {
        ESP_RETURN_ON_FALSE(!epaper_panel->_swap_xy, ESP_ERR_INVALID_ARG, TAG, "swap_xy unavailable in non_copy_mode");
        ESP_RETURN_ON_FALSE(!epaper_panel->_mirror_y, ESP_ERR_INVALID_ARG, TAG, "mirror_y unavailable in non_copy_mode");
    }

    ESP_RETURN_ON_FALSE(color_data, ESP_ERR_INVALID_ARG, TAG, "bitmap is null");
    ESP_RETURN_ON_FALSE(x_start < x_end && y_start < y_end, ESP_ERR_INVALID_ARG, TAG, "invalid area");

    int len_x = x_end - x_start;
    int len_y = y_end - y_start;
    int x_draw = x_start;
    int y_draw = y_start;
    int w_draw = len_x;
    int h_draw = len_y;

    x_draw -= x_draw % 8;
    w_draw = ((w_draw + 7) / 8) * 8;

    int src_stride = (len_x + 7) / 8;
    int dst_stride = w_draw / 8;
    int buffer_size = dst_stride * h_draw;

    // 先设 RAM 窗（tx_param 会排空上一次异步 SPI color DMA），再准备发送缓冲。
    // 否则复用 bounce 时可能覆盖仍在 DMA 中的数据。
    ESP_RETURN_ON_ERROR(epaper_set_ram_area(epaper_panel->io, x_draw, y_draw, w_draw, h_draw), TAG, "RAM area err");

    const uint8_t *tx_buf = NULL;
    if (epaper_panel->_non_copy_mode) {
        // 仅内部 DMA 内存可直发。PSRAM（即使带 DMA cap）经 cache 写入后直 DMA
        // 易丢数据/鬼影；统一走 CPU memcpy → 内部 bounce，再 SPI DMA。
        const bool direct_ok = esp_ptr_dma_capable(color_data) && !esp_ptr_external_ram(color_data);
        if (direct_ok) {
            tx_buf = color_data;
        } else {
            ESP_RETURN_ON_FALSE(buffer_size <= SSD1677_PANEL_BUFFER_SIZE, ESP_ERR_INVALID_SIZE, TAG,
                                "bitmap too large");
            if (!epaper_panel->_framebuffer) {
                epaper_panel->_framebuffer =
                    heap_caps_malloc(SSD1677_PANEL_BUFFER_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
                ESP_RETURN_ON_FALSE(epaper_panel->_framebuffer, ESP_ERR_NO_MEM, TAG, "dma bounce alloc failed");
            }
            memcpy(epaper_panel->_framebuffer, color_data, buffer_size);
            tx_buf = epaper_panel->_framebuffer;
        }
    } else {
        process_bitmap(panel, w_draw, h_draw, src_stride, dst_stride, color_data);
        tx_buf = epaper_panel->_framebuffer;
    }

    uint8_t vram_cmd = (epaper_panel->bitmap_color == SSD1677_EPAPER_BITMAP_PREVIOUS)
                           ? SSD1677_CMD_WRITE_RED_VRAM
                           : SSD1677_CMD_WRITE_BW_VRAM;
    ESP_RETURN_ON_ERROR(panel_epaper_set_vram(epaper_panel->io, vram_cmd, tx_buf, buffer_size), TAG, "set_vram err");
    return ESP_OK;
}

static esp_err_t epaper_panel_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    epaper_panel_t *epaper_panel = __containerof(panel, epaper_panel_t, base);
    epaper_panel->_invert_color = invert_color_data;
    return ESP_OK;
}

static esp_err_t epaper_panel_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    epaper_panel_t *epaper_panel = __containerof(panel, epaper_panel_t, base);
    if (mirror_y && epaper_panel->_non_copy_mode) {
        return ESP_ERR_INVALID_ARG;
    }
    epaper_panel->_mirror_x = mirror_x;
    epaper_panel->_mirror_y = mirror_y;
    return ESP_OK;
}

static esp_err_t epaper_panel_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    epaper_panel_t *epaper_panel = __containerof(panel, epaper_panel_t, base);
    if (swap_axes && epaper_panel->_non_copy_mode) {
        return ESP_ERR_INVALID_ARG;
    }
    epaper_panel->_swap_xy = swap_axes;
    return ESP_OK;
}

static esp_err_t epaper_panel_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    epaper_panel_t *epaper_panel = __containerof(panel, epaper_panel_t, base);
    epaper_panel->gap_x = x_gap;
    epaper_panel->gap_y = y_gap;
    return ESP_OK;
}

static esp_err_t epaper_panel_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    epaper_panel_t *epaper_panel = __containerof(panel, epaper_panel_t, base);
    if (on_off) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_DISP_UPDATE_CTRL2,
                                                      (uint8_t[]) { SSD1677_PARAM_DISP_UPDATE_POWER_ON }, 1), TAG,
                            "power on err");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_MASTER_ACTIVATION, NULL, 0), TAG,
                            "power on activation err");
        return panel_epaper_wait_busy(panel);
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_DISP_UPDATE_CTRL2,
                                                  (uint8_t[]) { SSD1677_PARAM_DISP_UPDATE_POWER_OFF }, 1), TAG,
                        "power off err");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_MASTER_ACTIVATION, NULL, 0), TAG,
                        "power off activation err");
    return panel_epaper_wait_busy(panel);
}

static esp_err_t epaper_panel_deep_sleep_locked(esp_lcd_panel_t *panel)
{
    epaper_panel_t *epaper_panel = __containerof(panel, epaper_panel_t, base);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_DEEP_SLEEP,
                                                  (uint8_t[]) { SSD1677_PARAM_DEEP_SLEEP }, 1),
                        TAG, "deep sleep err");
    /* GxEPD2/GoodDisplay：Deep Sleep 后 BUSY 常保持高，留时间给模拟前端收尾。 */
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

esp_err_t epaper_panel_park_for_shutdown(esp_lcd_panel_t *panel)
{
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "panel is NULL");
    epaper_panel_t *epaper_panel = __containerof(panel, epaper_panel_t, base);

    if (epaper_panel->busy_gpio_num >= 0 && gpio_get_level(epaper_panel->busy_gpio_num)) {
        ESP_LOGW(TAG, "park: BUSY still high before power off, waiting");
        ESP_RETURN_ON_ERROR(panel_epaper_wait_busy(panel), TAG, "busy pre-park");
    }

    esp_err_t err = epaper_panel_disp_on_off(panel, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "park power off failed: %s", esp_err_to_name(err));
        return err;
    }

    err = epaper_panel_deep_sleep_locked(panel);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "park deep sleep failed: %s", esp_err_to_name(err));
        return err;
    }

    const int busy_after = (epaper_panel->busy_gpio_num >= 0) ? gpio_get_level(epaper_panel->busy_gpio_num) : -1;
    ESP_LOGI(TAG, "park ok busy_after=%d (expect 1 in deep sleep)", busy_after);
    return ESP_OK;
}

static esp_err_t process_bitmap(esp_lcd_panel_t *panel, int len_x, int len_y, int src_stride, int dst_stride,
                                const void *color_data)
{
    epaper_panel_t *epaper_panel = __containerof(panel, epaper_panel_t, base);
    const uint8_t *src = color_data;
    uint8_t *dst = epaper_panel->_framebuffer;

    memset(dst, epaper_panel->_invert_color ? 0x00 : 0xFF, dst_stride * len_y);

    for (int row = 0; row < len_y; row++) {
        int src_row = epaper_panel->_mirror_y ? (len_y - 1 - row) : row;
        int dst_row = row;
        const uint8_t *src_line = &src[src_row * src_stride];
        uint8_t *dst_line = &dst[dst_row * dst_stride];

        for (int col = 0; col < dst_stride; col++) {
            uint8_t data = (col < src_stride) ? src_line[col] : 0xFF;
            if (epaper_panel->_mirror_x) {
                data = byte_reverse(data);
            }
            if (epaper_panel->_invert_color) {
                data = ~data;
            }
            dst_line[col] = data;
        }
    }

    (void)len_x;
    (void)epaper_panel->_swap_xy;
    return ESP_OK;
}

static inline uint8_t byte_reverse(uint8_t data)
{
    static const uint8_t lut[] = {
        0x00, 0x08, 0x04, 0x0C, 0x02, 0x0A, 0x06, 0x0E,
        0x01, 0x09, 0x05, 0x0D, 0x03, 0x0B, 0x07, 0x0F
    };
    return (uint8_t)((lut[data & 0x0F] << 4) | lut[data >> 4]);
}
