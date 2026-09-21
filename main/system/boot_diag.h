#pragma once

#include <cstdint>

// Early USB Serial/JTAG diagnostic service.
//
// The product command handler lives in RawDisplay::FrameDumpTask, which is
// created only after I2C, the IO expander, battery/RTC/charger, Bluetooth, SD,
// the e-paper panel and touch have all initialized. Any failure before that
// point leaves the device silent with no way to ask what happened.
//
// This module installs the USB Serial/JTAG driver at the top of app_main and
// answers a small read-only command set from a dedicated task, so boot progress
// and reset cause stay observable even when the product UI never comes up.
namespace boot_diag {

// Coarse boot progress. Append new stages before kCount; existing values are
// part of the host-visible protocol.
enum class Stage : uint32_t {
    kAppMain = 0,       // app_main entered
    kNvsReady,          // NVS initialized
    kAppStart,          // Application::Start entered
    kHalBegin,          // about to construct the board (runs board init)
    kBoardI2c,          // I2C bus + IO expander + power rails up
    kBoardSd,           // SD card mount attempted, virtual disk worker started
    kBoardPanel,        // SSD1677 panel reset/init done
    kBoardDisplay,      // RawDisplay constructed
    kBoardReady,        // board constructor finished
    kHomeShown,         // product home screen drawn
    kProvidersStarted,  // dashboard/notes/reminders/xiaozhi started
    kCount,
};

const char* StageName(Stage stage);

// Installs the USB Serial/JTAG driver (if not already installed) and starts the
// diagnostic task. Safe to call once; repeat calls are ignored.
void Init();

// Records boot progress. Lock-free, callable from any task.
void Mark(Stage stage);

// Current stage, for host-side queries and logging.
Stage Current();

// Called by RawDisplay once its own reader owns the RX path. After this the
// diagnostic task stops reading so it cannot steal product commands. If the
// product reader never starts, the diagnostic task keeps serving.
void SetDisplayReaderActive();

bool DisplayReaderActive();

}  // namespace boot_diag
