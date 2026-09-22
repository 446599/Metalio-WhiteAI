# 硬件测试专用固件：仅编 apps / hal / 板级驱动 / 精简 display
set(SOURCES
            "audio/audio_codec.cc"
            "audio/codecs/dummy_audio_codec.cc"
            "display/display.cc"
            "display/raw_display.cc"
            "display/system_view.cc"
            "display/input_view.cc"
            "display/history_view.cc"
            "display/reader_view.cc"
            "reader/reader_service.cc"
            "input/text_input.cc"
            "network/setup_model.cc"
            "network/wifi_setup.cc"
            "notes/note_writer.cc"
            "display/reminder_view.cc"
            "display/font/ai_ui_assets.c"
            "display/font/raw_font.cc"
            "display/font/font_loader.c"
            "xiaozhi/conversation.cc"
            "xiaozhi/capsule_store.cc"
            "xiaozhi/mcp_server.cc"
            "xiaozhi/system_tools.cc"
            "xiaozhi/memory_tools.cc"
            "system/device_control.cc"
            "system/quick_controls.cc"
            "display/quick_controls_view.cc"
            "system/boot_diag.cc"
            "power/sleep_service.cc"
            "display/lock_screen_view.cc"
            "notes/note_store.cc"
            "notes/note_service.cc"
            "reminders/reminder_store.cc"
            "reminders/reminder_service.cc"
            "dashboard/dashboard_data.cc"
            "dashboard/dashboard_service.cc"
    "dashboard/weather_provider.cc"
    "display/weather_view.cc"
            "xiaozhi/xiaozhi_client.cc"
            "chat/history_store.cc"
            "chat/history_service.cc"
            "xiaozhi/xiaozhi_audio.cc"
            "xiaozhi/xiaozhi_activation.cc"
            "hal/hal.cc"
            "system_info.cc"
            "application.cc"
            "settings.cc"
            "haptic_feedback.cc"
            "device_state_event.cc"
            "main.cc"
            )

# Do not merely defer nimble_port_init until the first tap: referenced
# controller objects already occupy IRAM at link time and reduce DRAM heap.
if(CONFIG_WHITEAI_EXPERIMENTAL_BLE_DISCOVERY)
    list(APPEND SOURCES "hal/hal_ble_scan.cc")
else()
    list(APPEND SOURCES "hal/hal_ble_unavailable.cc")
endif()

set(INCLUDE_DIRS
            "."
            "display"
            "display/font"
            "display/screen"
            "dashboard"
            "audio"
            "hal"
            )

set(HAL_COMMON_SOURCES
            "hal/common/board.cc"
            "hal/common/dual_network_board.cc"
            "hal/common/wifi_board.cc"
            "hal/common/nt26_board.cc"
            "hal/common/ml307_board.cc"
            "hal/common/uart_eth_modem.cc"
            "hal/common/button.cc"
            "hal/common/bt_audio_codec.cc"
            "hal/common/bq27220_gauge.cc"
            "hal/common/pcf8563.cc"
            "hal/common/cx25601n.cc"
            "hal/common/backlight.cc"
            "hal/common/i2c_device.cc"
            "hal/common/system_reset.cc"
            "hal/common/usb_virtual_disk.cc"
            "hal/common/adc_battery_monitor.cc"
            )
list(APPEND SOURCES ${HAL_COMMON_SOURCES})
list(APPEND INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/hal/common)

set(BOARD_TYPE "metalio-e-ink-4")
set(BUILTIN_TEXT_FONT font_puhui_basic_14_1)
set(BUILTIN_ICON_FONT font_awesome_14_1)

file(GLOB HAL_BOARD_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/hal/${BOARD_TYPE}/*.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/hal/${BOARD_TYPE}/*.c
)
list(APPEND SOURCES ${HAL_BOARD_SOURCES})
list(APPEND INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/hal/${BOARD_TYPE})

idf_component_register(SRCS ${SOURCES}
                    INCLUDE_DIRS ${INCLUDE_DIRS}
                    REQUIRES uart-uhci
                             reader_text
                             esp_driver_gpio
                             esp_driver_i2c
                             esp_driver_spi
                             esp_driver_uart
                             esp_driver_usb_serial_jtag
                             esp_driver_ledc
                             esp_driver_i2s
                             esp_driver_sdmmc
                             spi_flash
                             esp_app_format
                             app_update
                    WHOLE_ARCHIVE
                    )

target_link_libraries(${COMPONENT_LIB} PRIVATE idf::fatfs idf::sdmmc idf::esp_driver_sdmmc idf::espressif__zlib idf::esp_partition idf::espcoredump idf::esp-ml307 idf::esp-wifi-connect)
if(CONFIG_WHITEAI_EXPERIMENTAL_BLE_DISCOVERY)
    target_link_libraries(${COMPONENT_LIB} PRIVATE idf::bt)
endif()
if(TARGET idf::espressif__esp_tinyusb)
    # ESP-IDF 6 no longer exports the legacy idf::usb target; the TinyUSB
    # component brings the USB implementation transitively.
    target_link_libraries(${COMPONENT_LIB} PRIVATE idf::espressif__esp_tinyusb)
endif()


if(NOT BOARD_NAME)
    set(BOARD_NAME ${BOARD_TYPE})
endif()
target_compile_definitions(${COMPONENT_LIB}
                    PRIVATE BOARD_TYPE=\"${BOARD_TYPE}\" BOARD_NAME=\"${BOARD_NAME}\"
                    PRIVATE BUILTIN_TEXT_FONT=${BUILTIN_TEXT_FONT} BUILTIN_ICON_FONT=${BUILTIN_ICON_FONT}
                    )
