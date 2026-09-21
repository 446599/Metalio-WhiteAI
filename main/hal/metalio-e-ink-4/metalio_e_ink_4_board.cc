#include "dual_network_board.h"
#include "application.h"
#include "bt_audio_codec.h"
#include "bq27220_gauge.h"
#include "display/raw_display.h"
#include "haptic_feedback.h"
#include "system_reset.h"
#include "button.h"
#include "config.h"
#include "assets/lang_config.h"
#include "IOExpander.hpp"
#include "SdCardManager.hpp"
#include "usb_virtual_disk.h"
#include "SimpleUart.hpp"
#include "cx25601n.h"
#include "pcf8563.h"
#include "settings.h"
#include "esp_lcd_panel_ssd1677.h"
#include "esp_lcd_ssd1677_commands.h"

#include <wifi_station.h>
#include <esp_log.h>
#include <esp_check.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_touch.h>
#include <esp_lcd_touch_cst816s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <button_types.h>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#define TAG "MetalioEInk4Board"

// 外置 BT 模块模式 1（I2S 时钟）是否已下发完成
static std::atomic<bool> s_bt_audio_mode_ready{false};

static void BtDefaultModeTask(void* /*arg*/) {
    SimpleUart& uart = SimpleUart::getInstance();
    ESP_LOGI(TAG, "TX: AT+RX=2");
    uart.sendString("AT+RX=2\r\n");
    vTaskDelay(pdMS_TO_TICKS(700));
    ESP_LOGI(TAG, "TX: AT+MODE=1");
    uart.sendString("AT+MODE=1\r\n");
    s_bt_audio_mode_ready.store(true);
    ESP_LOGI(TAG, "BT audio default mode applied (mode1)");
    vTaskDelete(nullptr);
}

static esp_timer_handle_t s_vibe_timer = nullptr;

static void VibeMotorOffTimerCb(void* /*arg*/) {
    gpio_set_level(VIBRATION_MOTOR_GPIO, 0);
}

static void PulseVibrationMotor() {
    if (s_vibe_timer == nullptr) {
        return;
    }
    gpio_set_level(VIBRATION_MOTOR_GPIO, 1);
    esp_timer_stop(s_vibe_timer);
    esp_err_t err = esp_timer_start_once(
        s_vibe_timer, static_cast<uint64_t>(VIBRATION_MOTOR_PULSE_MS) * 1000ULL);
    if (err != ESP_OK) {
        gpio_set_level(VIBRATION_MOTOR_GPIO, 0);
        ESP_LOGW(TAG, "vibe timer start failed: %s", esp_err_to_name(err));
    }
}

static void SetVibrationMotor(bool on) {
    if (s_vibe_timer != nullptr) {
        esp_timer_stop(s_vibe_timer);
    }
    gpio_set_level(VIBRATION_MOTOR_GPIO, on ? 1 : 0);
}

// TCA9555 按键：用 iot_button 自定义驱动轮询 IOExpander 电平（默认低电平有效）。
struct IoExpanderButtonDriver {
    button_driver_t base{};
    IOExpander::Pin pin = IOExpander::Pin::kPinCount;
    uint8_t active_level = 0;
};

static uint8_t IoExpanderButtonGetKeyLevel(button_driver_t* button_driver) {
    auto* self = reinterpret_cast<IoExpanderButtonDriver*>(button_driver);
    uint8_t level = 1;
    if (IOExpander::getInstance().getLevel(self->pin, &level) != ESP_OK) {
        return 0;
    }
    return level == self->active_level ? 1 : 0;
}

static esp_err_t IoExpanderButtonDelete(button_driver_t* button_driver) {
    delete reinterpret_cast<IoExpanderButtonDriver*>(button_driver);
    return ESP_OK;
}

static button_handle_t CreateIoExpanderButton(IOExpander::Pin pin, bool active_high = false) {
    auto* drv = new IoExpanderButtonDriver();
    drv->pin = pin;
    drv->active_level = active_high ? 1 : 0;
    drv->base.enable_power_save = false;
    drv->base.get_key_level = IoExpanderButtonGetKeyLevel;
    drv->base.enter_power_save = nullptr;
    drv->base.del = IoExpanderButtonDelete;

    button_config_t cfg = {};
    button_handle_t handle = nullptr;
    ESP_ERROR_CHECK(iot_button_create(&cfg, &drv->base, &handle));
    return handle;
}

// 关机：长按满 3s → 关功放/保屏电 → SHUTDOWN 全刷 + EPD park → settle → 断电脉冲。
constexpr uint16_t kBootLongPressMs = 500;    // BOOT / vk_home 长按阈值；与短按互斥
constexpr uint16_t kPowerLongPressMs = 3000;
/** Power Off BUSY（~200ms）+ Deep Sleep 后再等 VCOM/高压轨放完（GoodDisplay/GxEPD2 建议）。 */
constexpr uint32_t kPostEpdParkSettleMs = 280;

static volatile bool s_power_held = false;
static int64_t s_power_down_us = 0;

/** 关机前：保持屏座/总电源，关功放减纹波；勿断 SCREEN_SOCKET_PWR。 */
static void PrepareHardwareForShutdown() {
    auto& io = IOExpander::getInstance();
    (void)io.setLevel(IOExpander::Pin::PA, false);
    (void)io.setLevel(IOExpander::Pin::SCREEN_SOCKET_PWR, true);
    (void)io.setLevel(IOExpander::Pin::MAIN_PWR, true);
}

static void PwrShutdownTask(void* /*arg*/) {
    PrepareHardwareForShutdown();
    if (auto* disp = RawDisplay::Instance()) disp->ShowPoweredOffScreen();
    vTaskDelay(pdMS_TO_TICKS(kPostEpdParkSettleMs));

    ESP_LOGW(TAG, "POWER: PWR_KEY_PULSE after EPD park + %ums settle",
             static_cast<unsigned>(kPostEpdParkSettleMs));
    auto& io = IOExpander::getInstance();
    constexpr int kPulseHalfMs = 100;
    for (;;) {
        io.setLevel(IOExpander::Pin::PWR_KEY_PULSE, true);
        vTaskDelay(pdMS_TO_TICKS(kPulseHalfMs));
        io.setLevel(IOExpander::Pin::PWR_KEY_PULSE, false);
        vTaskDelay(pdMS_TO_TICKS(kPulseHalfMs));
    }
}

static void BeginPowerShutdownPulse() {
    static bool shutting_down = false;
    if (shutting_down) {
        return;
    }
    shutting_down = true;
    ESP_LOGW(TAG, "POWER held %ums: show powered-off UI then pulse",
             static_cast<unsigned>(kPowerLongPressMs));
    if (xTaskCreatePinnedToCore(PwrShutdownTask, "pwr_off", 16 * 1024, nullptr,
                                tskIDLE_PRIORITY + 5, nullptr, 0) != pdPASS) {
        ESP_LOGE(TAG, "pwr_off task create failed");
        shutting_down = false;
    }
}

class MetalioEInk4Board : public DualNetworkBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    esp_lcd_touch_handle_t touch_ = nullptr;
    Display* display_ = nullptr;

    Button boot_button_;
    Button power_button_;
    std::unique_ptr<Button> volume_up_button_;
    std::unique_ptr<Button> volume_down_button_;

    void InitializeI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = I2C_SDA_PIN,
            .scl_io_num = I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &i2c_bus_));
    }

    void InitializeIOExpander() {
        auto& io = IOExpander::getInstance();
        ESP_ERROR_CHECK(io.begin(i2c_bus_));
        // 先开总电源，再开屏幕卡座供电
        ESP_ERROR_CHECK(io.setLevel(IOExpander::Pin::MAIN_PWR, true));
        ESP_ERROR_CHECK(io.setLevel(IOExpander::Pin::SCREEN_SOCKET_PWR, true));
        // 功放：PA=高；PA_SWITCH=0 给 ESP32
        ESP_ERROR_CHECK(io.setLevel(IOExpander::Pin::PA_SWITCH, false));
        ESP_ERROR_CHECK(io.setLevel(IOExpander::Pin::PA, true));
        // 关机脉冲 = P1.3(P13) 空闲拉高；begin 时输出脚默认 0
        ESP_ERROR_CHECK(io.setLevel(IOExpander::Pin::PWR_KEY_PULSE, true));
        // 触摸 RST = P1.1(原理图 P11)：上电复位 L → Tpr → H → Tron（begin 已置 L）
        vTaskDelay(pdMS_TO_TICKS(10));   // Tpr ≥5ms
        ESP_ERROR_CHECK(io.setLevel(IOExpander::Pin::TOUCH_RST, true));
        vTaskDelay(pdMS_TO_TICKS(120));  // Tron ≥100ms
        ESP_LOGI(TAG, "MAIN_PWR + SCREEN_SOCKET_PWR on; PA=1; TP_RST(P1.1/P11) L->H; PWR_KEY(P1.3/P13)=H");

        // TCA9555 INT → 主控 GPIO2（开漏低有效，内部上拉）
        gpio_config_t int_conf = {};
        int_conf.pin_bit_mask = 1ULL << IO_EXPANDER_INT_GPIO;
        int_conf.mode = GPIO_MODE_INPUT;
        int_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        int_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        int_conf.intr_type = GPIO_INTR_DISABLE;
        esp_err_t int_err = gpio_config(&int_conf);
        if (int_err != ESP_OK) {
            ESP_LOGW(TAG, "IO expander INT GPIO%d config failed: %s",
                     static_cast<int>(IO_EXPANDER_INT_GPIO), esp_err_to_name(int_err));
        } else {
            ESP_LOGI(TAG, "IO expander INT on GPIO%d; ACCEL_INT on TCA9555 P1.4",
                     static_cast<int>(IO_EXPANDER_INT_GPIO));
        }
    }

    void InitializeVibrationMotor() {
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = 1ULL << VIBRATION_MOTOR_GPIO;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        esp_err_t err = gpio_config(&io_conf);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "vibe motor GPIO%d config failed: %s",
                     static_cast<int>(VIBRATION_MOTOR_GPIO), esp_err_to_name(err));
            return;
        }
        gpio_set_level(VIBRATION_MOTOR_GPIO, 0);

        const esp_timer_create_args_t timer_args = {
            .callback = &VibeMotorOffTimerCb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "vibe_off",
            .skip_unhandled_events = true,
        };
        err = esp_timer_create(&timer_args, &s_vibe_timer);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "vibe timer create failed: %s", esp_err_to_name(err));
            s_vibe_timer = nullptr;
            return;
        }
        ESP_LOGI(TAG, "vibe motor ready GPIO%d pulse=%dms",
                 static_cast<int>(VIBRATION_MOTOR_GPIO), VIBRATION_MOTOR_PULSE_MS);
    }

    void InitializeBTAudio() {
        SimpleUart& uart = SimpleUart::getInstance();
        if (!uart.begin(BT_AUDIO_TX_PIN, BT_AUDIO_RX_PIN, 115200, UART_NUM_2)) {
            ESP_LOGE(TAG, "BT audio UART init failed (TX=%d RX=%d)", BT_AUDIO_TX_PIN,
                     BT_AUDIO_RX_PIN);
            return;
        }
        ESP_LOGI(TAG, "BT audio UART ready (TX=%d RX=%d)", BT_AUDIO_TX_PIN, BT_AUDIO_RX_PIN);

        uart.registerCallback([](const std::vector<uint8_t>& data) {
            // 按可打印字符输出，CR/LF 换成空格，便于串口日志阅读
            std::string line;
            line.reserve(data.size());
            for (uint8_t b : data) {
                if (b == '\r' || b == '\n') {
                    line.push_back(' ');
                } else if (b >= 0x20 && b < 0x7F) {
                    line.push_back(static_cast<char>(b));
                } else {
                    line.push_back('.');
                }
            }
            ESP_LOGI(TAG, "BT RX (%u): %s", static_cast<unsigned>(data.size()), line.c_str());
        });

        // 硬件测试无 BluetoothScreen：后台下发模式 1，外置模块才输出 I2S 时钟
        s_bt_audio_mode_ready.store(false);
        if (xTaskCreate(BtDefaultModeTask, "bt_mode", 3072, nullptr, 5, nullptr) != pdPASS) {
            ESP_LOGE(TAG, "bt_mode task create failed");
        }
    }

    void InitializeSsd1677() {
        spi_bus_config_t buscfg = {
            .mosi_io_num = EPD_PIN_MOSI,
            .miso_io_num = -1,
            .sclk_io_num = EPD_PIN_SCLK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = SSD1677_PANEL_BUFFER_SIZE + 8,
        };
        ESP_ERROR_CHECK(spi_bus_initialize(EPD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

        esp_lcd_panel_io_spi_config_t io_config = {
            .cs_gpio_num = EPD_PIN_CS,
            .dc_gpio_num = EPD_PIN_DC,
            .spi_mode = 0,
            .pclk_hz = EPD_SPI_CLK_HZ,
            .trans_queue_depth = 10,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)EPD_SPI_HOST, &io_config,
                                                 &panel_io_));

        esp_lcd_ssd1677_config_t ssd1677_cfg = {
            .busy_gpio_num = EPD_PIN_BUSY,
            .non_copy_mode = true,
            .use_fast_full_update = true,
        };
        esp_lcd_panel_dev_config_t panel_cfg = {
            .reset_gpio_num = EPD_PIN_RST,
            .vendor_config = &ssd1677_cfg,
            .flags =
                {
                    .reset_active_high = false,
                },
        };

        // BUSY 脚 ISR 需要 GPIO ISR service
        esp_err_t isr_ret = gpio_install_isr_service(0);
        if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(isr_ret);
        }

        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1677(panel_io_, &panel_cfg, &panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        vTaskDelay(pdMS_TO_TICKS(100));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, true));
        vTaskDelay(pdMS_TO_TICKS(100));
        ESP_LOGI(TAG, "SSD1677 ready %dx%d", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    }

    // 返回 true 表示触摸可用；检测不到时不崩溃，touch_ 保持 nullptr。
    bool InitializeTouch() {
        touch_ = nullptr;

        esp_err_t probe =
            i2c_master_probe(i2c_bus_, ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS, 200);
        if (probe != ESP_OK) {
            ESP_LOGW(TAG, "CST816S not found on I2C (0x%02X): %s",
                     ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS, esp_err_to_name(probe));
            return false;
        }

        esp_lcd_panel_io_handle_t tp_io = nullptr;
        // 不用 ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG()：宏内 designator 顺序与 C++ 不兼容
        esp_lcd_panel_io_i2c_config_t tp_io_cfg = {};
        tp_io_cfg.dev_addr = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS;
        tp_io_cfg.control_phase_bytes = 1;
        tp_io_cfg.dc_bit_offset = 0;
        tp_io_cfg.lcd_cmd_bits = 8;
        tp_io_cfg.lcd_param_bits = 0;
        tp_io_cfg.flags.disable_control_phase = 1;
        tp_io_cfg.scl_speed_hz = TOUCH_I2C_HZ;
        esp_err_t ret = esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_cfg, &tp_io);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "touch panel_io create failed: %s", esp_err_to_name(ret));
            return false;
        }

        // CST816S 原生竖屏 480x800，与 framebuffer UI 坐标一致。
        // 不要 swap_xy，保持触摸与屏幕方向一致。
        // INT=TOUCH_INT_GPIO：adapter 走 IRQ 模式；levels.interrupt=0 → 低有效 / NEGEDGE。
        const esp_lcd_touch_config_t tp_cfg = {
            .x_max = SSD1677_PANEL_HEIGHT,
            .y_max = SSD1677_PANEL_WIDTH,
            .rst_gpio_num = GPIO_NUM_NC,
            .int_gpio_num = TOUCH_INT_GPIO,
            .levels =
                {
                    .reset = 0,
                    .interrupt = 0,
                },
            .flags =
                {
                    .swap_xy = false,
                    .mirror_x = false,
                    .mirror_y = false,
                },
        };

        ret = esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &touch_);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "CST816S init failed: %s", esp_err_to_name(ret));
            esp_lcd_panel_io_del(tp_io);
            touch_ = nullptr;
            return false;
        }

        // 驱动 gpio_config 未开上拉；INT 多为开漏，补内部上拉。
        esp_err_t pull = gpio_set_pull_mode(TOUCH_INT_GPIO, GPIO_PULLUP_ONLY);
        if (pull != ESP_OK) {
            ESP_LOGW(TAG, "touch INT pull-up failed: %s", esp_err_to_name(pull));
        }

        ESP_LOGI(TAG, "CST816S ready (SDA=%d SCL=%d INT=%d) native 480x800 IRQ", I2C_SDA_PIN,
                 I2C_SCL_PIN, static_cast<int>(TOUCH_INT_GPIO));
        return true;
    }

    void InitializeDisplay() {
        display_ = new RawDisplay(panel_, panel_io_, touch_, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    }

    void InitializeButtons() {
        // Dedicated AI push-to-talk key. Button callbacks only change audio
        // intent; slow panel rendering is queued on the application task.
        boot_button_.OnPressDown([]() {
            if (auto* raw = RawDisplay::Instance()) (void)raw->HandleAiKey(true);
        });
        boot_button_.OnPressUp([]() {
            if (auto* raw = RawDisplay::Instance()) (void)raw->HandleAiKey(false);
        });
        power_button_.OnPressDown([]() {
            s_power_held = true;
            s_power_down_us = esp_timer_get_time();
            ESP_LOGI(TAG, "按键按下: POWER (GPIO%d)", static_cast<int>(POWER_BUTTON_GPIO));
        });
        power_button_.OnPressUp([]() { s_power_held = false; });
        power_button_.OnLongPress([]() {
            ESP_LOGI(TAG, "按键长按 %ums: POWER 刷关机画并断电 (GPIO%d)",
                     static_cast<unsigned>(kPowerLongPressMs),
                     static_cast<int>(POWER_BUTTON_GPIO));
            BeginPowerShutdownPulse();
        });

        // 音量键挂在 TCA9555，需在 IOExpander begin 之后创建
        volume_down_button_ =
            std::make_unique<Button>(CreateIoExpanderButton(IOExpander::Pin::VOLUME_DOWN));
        volume_up_button_ =
            std::make_unique<Button>(CreateIoExpanderButton(IOExpander::Pin::VOLUME_UP));

        volume_up_button_->OnClick([this]() {
            // iot_button callbacks run on the esp_timer task (3.5 KiB stack).
            // NVS commit plus e-paper notification overflowed that stack and
            // reset the board; defer the complete volume transaction to the
            // application's 8 KiB event task.
            (void)Application::GetInstance().ScheduleUi([this]() {
                auto* codec = GetAudioCodec();
                if (codec == nullptr) return;
                int volume = std::min(100, codec->output_volume() + 10);
                codec->SetOutputVolume(volume);
                ESP_LOGI(TAG, "VOLUME_UP (P1.0): volume=%d", volume);
                if (auto* display = GetDisplay())
                    display->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
            });
        });
        volume_up_button_->OnLongPress([this]() {
            (void)Application::GetInstance().ScheduleUi([this]() {
                if (auto* codec = GetAudioCodec()) codec->SetOutputVolume(100);
                ESP_LOGI(TAG, "VOLUME_UP long: volume=100");
                if (auto* display = GetDisplay()) display->ShowNotification(Lang::Strings::MAX_VOLUME);
            });
        });

        volume_down_button_->OnClick([this]() {
            (void)Application::GetInstance().ScheduleUi([this]() {
                auto* codec = GetAudioCodec();
                if (codec == nullptr) return;
                int volume = std::max(0, codec->output_volume() - 10);
                codec->SetOutputVolume(volume);
                ESP_LOGI(TAG, "VOLUME_DOWN (P0.7): volume=%d", volume);
                if (auto* display = GetDisplay())
                    display->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
            });
        });
        volume_down_button_->OnLongPress([this]() {
            (void)Application::GetInstance().ScheduleUi([this]() {
                if (auto* codec = GetAudioCodec()) codec->SetOutputVolume(0);
                ESP_LOGI(TAG, "VOLUME_DOWN long: volume=0 (muted)");
                if (auto* display = GetDisplay()) display->ShowNotification(Lang::Strings::MUTED);
            });
        });

        ESP_LOGI(TAG,
                 "Buttons ready: BOOT=GPIO%d POWER=GPIO%d VOL-=P0.7 VOL+=P1.0",
                 static_cast<int>(BOOT_BUTTON_GPIO), static_cast<int>(POWER_BUTTON_GPIO));
    }

    // PCF8563 RTC：开机先灌芯片时间到系统；联网对时成功后再回写芯片。
    void InitializePcf8563() {
        auto& rtc = Pcf8563::GetInstance();
        if (!rtc.Begin(i2c_bus_)) {
            ESP_LOGW(TAG, "PCF8563 init skipped");
            return;
        }
        if (rtc.ApplyRtcToSystem()) {
            ESP_LOGI(TAG, "boot time source: PCF8563 (offline / pre-network)");
        } else {
            ESP_LOGW(TAG, "PCF8563 present but ApplyRtcToSystem failed");
        }
    }

    // CX25601N 充电 IC（I2C 0x6B）。老设备无此芯片，probe 失败则跳过。
    void InitializeCx25601n() {
        esp_err_t probe = i2c_master_probe(i2c_bus_, CX25601N_I2C_ADDR, 100);
        if (probe != ESP_OK) {
            ESP_LOGI(TAG, "CX25601N not found at 0x%02X (legacy board?), skip init",
                     CX25601N_I2C_ADDR);
            return;
        }
        esp_err_t err = cx25601n_init(i2c_bus_);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "CX25601N found at 0x%02X but init failed: %s", CX25601N_I2C_ADDR,
                     esp_err_to_name(err));
            return;
        }
        // 应用设置里保存的充电电流（默认 500mA）
        Settings charge_settings("charge");
        int ichg_ma = charge_settings.GetInt("ichg_ma", 500);
        if (ichg_ma != 500 && ichg_ma != 1000) {
            ichg_ma = 500;
        }
        err = cx25601n_set_ichg_ma(static_cast<uint32_t>(ichg_ma));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "CX25601N set ichg=%d failed: %s", ichg_ma, esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "CX25601N init OK at 0x%02X, ichg=%d mA", CX25601N_I2C_ADDR,
                     ichg_ma);
        }
    }

    // 开机把 SD 卡挂到 /sdcard。失败不致命（卡没插 / 没格式化都会失败）。
    void InitializeSdCard() {
        // microSD pin2 = CD/DAT3：1-bit 模式下不走数据线，但必须为高，否则卡易进 SPI。
        // ESP32-S3 GPIO46 仅输入，无法推挽拉高，用内部上拉（板级有外拉更好）。
        gpio_config_t dat3_cfg = {
            .pin_bit_mask = BIT64(SDMMC_DAT3_PIN),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t dat3_ret = gpio_config(&dat3_cfg);
        if (dat3_ret != ESP_OK) {
            ESP_LOGW(TAG, "SD DAT3/CD GPIO%d pull-up config failed: %s",
                     static_cast<int>(SDMMC_DAT3_PIN), esp_err_to_name(dat3_ret));
        } else {
            ESP_LOGI(TAG, "SD DAT3/CD GPIO%d input+pullup (idle high)",
                     static_cast<int>(SDMMC_DAT3_PIN));
        }

        if (!SdCardManager::GetInstance().Mount()) {
            ESP_LOGW(TAG, "SD card not mounted at boot (card may be absent)");
        }
        // 虚拟 U 盘 worker：默认保持 USB Serial/JTAG，设置页启用时再切 MSC。
        UsbVirtualDisk::GetInstance().Init();
    }

    // 每秒打印双核 CPU 占用与内部 SRAM 剩余（依赖
    // CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS + USE_TRACE_FACILITY +
    // RUN_TIME_STATS_USING_ESP_TIMER）。
    void StartSystemMonitor() {
        xTaskCreate(
            [](void*) {
                constexpr int kCoreCount = portNUM_PROCESSORS;
                configRUN_TIME_COUNTER_TYPE prev_idle[kCoreCount] = {};
                for (int c = 0; c < kCoreCount; ++c) {
                    prev_idle[c] = ulTaskGetIdleRunTimeCounterForCore(c);
                }
                uint64_t prev_us = static_cast<uint64_t>(esp_timer_get_time());
                // int cx25601n_log_tick = 0;

                while (true) {
                    vTaskDelay(pdMS_TO_TICKS(1000));

                    const uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
                    const uint64_t dt_us = now_us - prev_us;
                    int usage[kCoreCount] = {};
                    int total_usage = 0;
                    if (dt_us > 0) {
                        for (int c = 0; c < kCoreCount; ++c) {
                            const configRUN_TIME_COUNTER_TYPE now_idle =
                                ulTaskGetIdleRunTimeCounterForCore(c);
                            const configRUN_TIME_COUNTER_TYPE didle = now_idle - prev_idle[c];
                            uint64_t idle_pct = static_cast<uint64_t>(didle) * 100ULL / dt_us;
                            if (idle_pct > 100) {
                                idle_pct = 100;
                            }
                            usage[c] = 100 - static_cast<int>(idle_pct);
                            total_usage += usage[c];
                            prev_idle[c] = now_idle;
                        }
                    }
                    prev_us = now_us;
                    const int avg_usage = (kCoreCount > 0) ? (total_usage / kCoreCount) : 0;
                    const int core1_usage = (kCoreCount > 1) ? usage[1] : 0;

                    constexpr const char* kMonitorTag = "系统监控";
                    ESP_LOGI(kMonitorTag,
                             "@@@CPU   | 内核0: %3d%% | 内核1: %3d%% | 平均: %3d%%",
                             usage[0], core1_usage, avg_usage);

                    const unsigned free_kb = static_cast<unsigned>(
                        heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
                    const unsigned min_free_kb = static_cast<unsigned>(
                        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024);
                    ESP_LOGI(kMonitorTag,
                             "@@@内存  | 剩余: %6u KB | 历史最小: %6u KB",
                             free_kb, min_free_kb);

                    // ---- 电池电量（BQ27220）----
                    auto& gauge = Bq27220Gauge::GetInstance();
                    int battery_level = 0;
                    bool charging = false;
                    bool discharging = false;
                    if (gauge.GetBatteryLevel(battery_level, charging, discharging)) {
                        uint16_t mv = 0;
                        if (gauge.GetVoltageMv(mv)) {
                            ESP_LOGI(kMonitorTag,
                                     "@@@电池  | 电量: %3d%% | 电压: %5u mV | "
                                     "充电: %s | 放电: %s",
                                     battery_level, mv, charging ? "是" : "否",
                                     discharging ? "是" : "否");
                        } else {
                            ESP_LOGI(kMonitorTag,
                                     "@@@电池  | 电量: %3d%% | 电压: 读取失败 | "
                                     "充电: %s | 放电: %s",
                                     battery_level, charging ? "是" : "否",
                                     discharging ? "是" : "否");
                        }
                    }

                    // ---- CX25601N VREG 寄存器（每 5s，便于核对恒压目标）----
                    // {
                    //     if (++cx25601n_log_tick >= 5) {
                    //         cx25601n_log_tick = 0;
                    //         if (!cx25601n_is_ready()) {
                    //             ESP_LOGI(kMonitorTag, "@@@CX25601N | 未初始化或未检测到芯片");
                    //         } else {
                    //             uint8_t reg04 = 0;
                    //             uint8_t reg05 = 0;
                    //             uint8_t reg16 = 0;
                    //             uint32_t vreg_mv = 0;
                    //             uint8_t chrg_stat = 0;
                    //             uint8_t vbus_stat = 0;
                    //             bool en_chg = false;
                    //
                    //             const esp_err_t err04 = cx25601n_read_reg(0x04, &reg04);
                    //             const esp_err_t err05 = cx25601n_read_reg(0x05, &reg05);
                    //             const esp_err_t err16 = cx25601n_read_reg(0x16, &reg16);
                    //             const esp_err_t err_vreg = cx25601n_get_vreg_mv(&vreg_mv);
                    //             const esp_err_t err_chrg = cx25601n_get_chrg_stat(&chrg_stat);
                    //             const esp_err_t err_vbus = cx25601n_get_vbus_stat(&vbus_stat);
                    //             const esp_err_t err_en = cx25601n_is_charge_enabled(&en_chg);
                    //
                    //             if (err04 != ESP_OK || err05 != ESP_OK || err_vreg != ESP_OK) {
                    //                 ESP_LOGW(kMonitorTag,
                    //                          "@@@CX25601N | VREG 读取失败 | REG0x04=%s REG0x05=%s "
                    //                          "get_vreg=%s",
                    //                          esp_err_to_name(err04), esp_err_to_name(err05),
                    //                          esp_err_to_name(err_vreg));
                    //             } else {
                    //                 const uint8_t vreg_lo =
                    //                     static_cast<uint8_t>((reg04 >> 3) & 0x1F);
                    //                 const uint8_t vreg_hi = static_cast<uint8_t>(reg05 & 0x0F);
                    //                 const uint32_t code =
                    //                     static_cast<uint32_t>(vreg_lo) |
                    //                     (static_cast<uint32_t>(vreg_hi) << 5);
                    //                 const uint16_t reg_le =
                    //                     static_cast<uint16_t>(reg04) |
                    //                     (static_cast<uint16_t>(reg05) << 8);
                    //
                    //                 ESP_LOGI(kMonitorTag,
                    //                          "@@@CX25601N | VREG=%lu mV | code=%lu (0x%03lX) | "
                    //                          "REG0x04=0x%02X REG0x05=0x%02X | "
                    //                          "VREG[4:0]=%u@0x04[7:3] VREG[8:5]=%u@0x05[3:0] | "
                    //                          "16bit_LE=0x%04X | EN_CHG=%s(%s) | CHG=%s(%s) | "
                    //                          "VBUS=%s(%s)",
                    //                          static_cast<unsigned long>(vreg_mv),
                    //                          static_cast<unsigned long>(code),
                    //                          static_cast<unsigned long>(code),
                    //                          reg04, reg05, vreg_lo, vreg_hi, reg_le,
                    //                          en_chg ? "开" : "关",
                    //                          err_en == ESP_OK ? "OK" : esp_err_to_name(err_en),
                    //                          cx25601n_chrg_stat_str(chrg_stat),
                    //                          err_chrg == ESP_OK ? "OK" : esp_err_to_name(err_chrg),
                    //                          cx25601n_vbus_stat_str(vbus_stat),
                    //                          err_vbus == ESP_OK ? "OK" : esp_err_to_name(err_vbus));
                    //                 if (err16 == ESP_OK) {
                    //                     ESP_LOGI(kMonitorTag,
                    //                              "@@@CX25601N | REG0x16=0x%02X | EN_CHG(bit5)=%u "
                    //                              "EN_HIZ(bit4)=%u WDT[1:0]=%u",
                    //                              reg16, (reg16 >> 5) & 1, (reg16 >> 4) & 1,
                    //                              reg16 & 0x03);
                    //                 }
                    //             }
                    //         }
                    //     }
                    // }

                    // ---- 网络信号（参考 metalio-claw-4 系统监控）----
                    // WiFi → RSSI；4G(NT26) → CSQ。已连通才打数值，避免未就绪时刷无效值。
                    // {
                    //     auto& dual = static_cast<DualNetworkBoard&>(Board::GetInstance());
                    //     const NetworkType net_type = dual.GetNetworkType();
                    //     if (net_type == NetworkType::WIFI) {
                    //         auto& wifi = WifiStation::GetInstance();
                    //         if (wifi.IsConnected()) {
                    //             ESP_LOGI(kMonitorTag,
                    //                      "@@@信号  | 网络: WiFi | RSSI: %d dBm",
                    //                      static_cast<int>(wifi.GetRssi()));
                    //         } else {
                    //             ESP_LOGI(kMonitorTag, "@@@信号  | 网络: WiFi | 未连接");
                    //         }
                    //     } else {
                    //         auto& nt26 = static_cast<Nt26Board&>(dual.GetCurrentBoard());
                    //         const int csq = nt26.GetSignalStrength();
                    //         if (csq == 99 || csq < 0) {
                    //             ESP_LOGI(kMonitorTag, "@@@信号  | 网络: 4G | CSQ: 未知");
                    //         } else {
                    //             ESP_LOGI(kMonitorTag, "@@@信号  | 网络: 4G | CSQ: %2d", csq);
                    //         }
                    //     }
                    // }
                }
            },
            "sys_mon", 4096, nullptr, 1, nullptr);
    }

public:
    MetalioEInk4Board()
        : DualNetworkBoard(NT26_TX_PIN, NT26_RX_PIN, NT26_MRDY_PIN, NT26_SRDY_PIN, 1),
          boot_button_(BOOT_BUTTON_GPIO, false, kBootLongPressMs),
          power_button_(POWER_BUTTON_GPIO, false, kPowerLongPressMs) {
        InitializeI2c();
        InitializeIOExpander();
        InitializeVibrationMotor();
        // BQ27220 挂到 I2C；失败不崩溃，GetBatteryLevel 内会节流自愈重试。
        (void)Bq27220Gauge::GetInstance().Begin(i2c_bus_);
        InitializePcf8563();
        InitializeCx25601n();
        InitializeBTAudio();
        InitializeSdCard();
        InitializeSsd1677();
        const bool touch_ok = InitializeTouch();
        InitializeDisplay();
        if (!touch_ok) {
            ESP_LOGW(TAG, "Touch missing; TouchMissingScreen should be showing");
        }
        InitializeButtons();
        StartSystemMonitor();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BTAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                              AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_MIC_GPIO_WS,
                                              AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    virtual bool IsBtAudioModeReady() const override {
        return s_bt_audio_mode_ready.load();
    }

    virtual Display* GetDisplay() override { return display_; }

    // 转发到 Bq27220Gauge；通知栏 UpdateStatusBar 会按 level/充电态刷新电池图标。
    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        return Bq27220Gauge::GetInstance().GetBatteryLevel(level, charging, discharging);
    }

    virtual void PulseVibration() override { PulseVibrationMotor(); }

    virtual void SetVibration(bool on) override { SetVibrationMotor(on); }

    // 联网且 OTA 写入 server_time 后：系统时间 → RTC → 再读回系统，以芯片为准。
    virtual void OnNetworkTimeSynced() override {
        auto& rtc = Pcf8563::GetInstance();
        if (!rtc.IsReady()) {
            ESP_LOGW(TAG, "OnNetworkTimeSynced: PCF8563 not ready");
            return;
        }
        if (!rtc.SyncSystemToRtc()) {
            ESP_LOGW(TAG, "OnNetworkTimeSynced: SyncSystemToRtc failed");
            return;
        }
        if (!rtc.ApplyRtcToSystem()) {
            ESP_LOGW(TAG, "OnNetworkTimeSynced: ApplyRtcToSystem failed");
            return;
        }
        ESP_LOGI(TAG, "network time synced to PCF8563; system uses RTC");
    }
};

DECLARE_BOARD(MetalioEInk4Board);
