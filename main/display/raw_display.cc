#include "raw_display.h"
#include "chat/history_service.h"
#include "system/quick_controls.h"
#include "input/gesture.h"
#include "reader/reader_service.h"
#include "network/wifi_setup.h"
#include "notes/note_writer.h"
#include "binary_refresh.h"
#include "font/raw_font.h"
#include "font/text_layout.h"
#include "application.h"
#include "xiaozhi/conversation.h"

#include "board.h"
#include "dashboard/dashboard_data.h"
#include "dashboard/dashboard_service.h"
#include "esp_lcd_ssd1677_commands.h"
#include "esp_lcd_panel_ssd1677.h"
#include "hal/hal.h"
#include "hal/metalio-e-ink-4/config.h"
#include "settings.h"
#include "xiaozhi/xiaozhi_audio.h"
#include "xiaozhi/xiaozhi_activation.h"
#include "xiaozhi/xiaozhi_client.h"
#include "reminders/reminder_service.h"
#include "reminders/presentation.h"
#include "notes/note_service.h"
#include "system/boot_diag.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "hal/usb_serial_jtag_ll.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <strings.h>
#include <unistd.h>

namespace {
constexpr const char* TAG = "RawDisplay";
constexpr int kPanelW = 800;
constexpr int kPanelH = 480;
constexpr int kPortraitW = 480;
constexpr int kPortraitH = 800;
constexpr uint8_t kWhite = 0xff;

// Reset cause is kept on the RTC domain, so a crash that predates the serial
// session can still be read back from the stats command.
const char* ResetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "poweron";
        case ESP_RST_EXT: return "ext";
        case ESP_RST_SW: return "sw";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "int_wdt";
        case ESP_RST_TASK_WDT: return "task_wdt";
        case ESP_RST_WDT: return "wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "sdio";
        case ESP_RST_USB: return "usb";
        case ESP_RST_JTAG: return "jtag";
        case ESP_RST_EFUSE: return "efuse";
        case ESP_RST_PWR_GLITCH: return "pwr_glitch";
        case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
        default: return "unknown";
    }
}
constexpr int kTestButtonX = 28;
constexpr int kTestButtonW = kPortraitW - 56;
constexpr int kTestButtonH = 92;
// Keep the original five button positions stable so existing touch scripts
// remain valid; the page-turn probe occupies one additional row below them.
constexpr int kTestButtonY[6] = {132, 236, 340, 444, 548, 652};
constexpr int kTestButtonCount = sizeof(kTestButtonY) / sizeof(kTestButtonY[0]);
constexpr int kWipeStripWidths[] = {40, 80, 160, 64, 128, 200};
constexpr int kWipeStripWidthCount = sizeof(kWipeStripWidths) / sizeof(kWipeStripWidths[0]);
constexpr int kGrayVariantCount = 4;
constexpr int kPaperPageWidth = 400;
constexpr int kPaperPageBOriginX = 80;
constexpr int kPaperPageTop = 16;
constexpr int kPaperPageBottom = 784;

struct PaperPageSelectorByte {
    uint8_t clear;
    uint8_t enter;
    uint8_t direct;
    uint8_t current;
    uint8_t previous;
};

// Page membership is separate from ink coverage: a white pixel can still be
// inside a page. The actual white waveform is selected only for old ink that
// is absent in the next frame. Shared ink changes gray/black directly.
constexpr PaperPageSelectorByte ComposePaperPageByte(uint8_t old24, uint8_t old26,
                                                     uint8_t next24, uint8_t next26,
                                                     uint8_t old_page, uint8_t next_page,
                                                     bool force_all = false) {
    old24 = static_cast<uint8_t>(old24 & old_page);
    old26 = static_cast<uint8_t>(old26 & old24);
    next24 = static_cast<uint8_t>(next24 & next_page);
    next26 = static_cast<uint8_t>(next26 & next24);
    const uint8_t clear = static_cast<uint8_t>(old24 & ~next24);
    const uint8_t enter = static_cast<uint8_t>(~old24 & next24);
    const uint8_t direct = static_cast<uint8_t>(old24 & next24 & (old26 ^ next26));
    const uint8_t drive = force_all ? 0xFFu : static_cast<uint8_t>(clear | enter | direct);
    return {clear, enter, direct,
            static_cast<uint8_t>(drive & ~(next24 ^ next26)),
            static_cast<uint8_t>(drive & next24)};
}

constexpr uint8_t PaperPageCoverageByte(int portrait_x, int panel_byte, int origin_x) {
    return portrait_x >= origin_x && portrait_x < origin_x + kPaperPageWidth &&
                   panel_byte >= kPaperPageTop / 8 && panel_byte < kPaperPageBottom / 8
               ? 0xFFu : 0x00u;
}

constexpr uint8_t PaperPagePopcount8(uint8_t value) {
    value = static_cast<uint8_t>(value - ((value >> 1) & 0x55u));
    value = static_cast<uint8_t>((value & 0x33u) + ((value >> 2) & 0x33u));
    return static_cast<uint8_t>((value + (value >> 4)) & 0x0Fu);
}

static_assert(ComposePaperPageByte(0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF).clear == 0xFF);
static_assert(ComposePaperPageByte(0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF).current == 0xFF);
static_assert(ComposePaperPageByte(0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF).previous == 0x00);
static_assert(ComposePaperPageByte(0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0xFF).direct == 0xFF);
static_assert(ComposePaperPageByte(0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0xFF).current == 0x00);
static_assert(ComposePaperPageByte(0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0xFF).previous == 0xFF);
static_assert(ComposePaperPageByte(0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF).enter == 0xFF);
static_assert(ComposePaperPageByte(0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF).current == 0xFF);
static_assert(ComposePaperPageByte(0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF).previous == 0xFF);
static_assert(ComposePaperPageByte(0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF).current == 0x00);
static_assert(PaperPageCoverageByte(0, 2, 0) == 0xFF);
static_assert(PaperPageCoverageByte(0, 2, kPaperPageBOriginX) == 0x00);
static_assert(PaperPageCoverageByte(479, 97, kPaperPageBOriginX) == 0xFF);

// Free-Ink's SSD1677/X4 grayscale waveform.  This is an experimental LUT:
// the GDEM0397T81 glass revision still needs optical validation.  Bytes 0..104
// are the waveform, 105..109 are VGH/VSH1/VSH2/VSL/VCOM, and 110..111 are
// reserved by SSD1677.
constexpr uint8_t kGray4Lut[112] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x54, 0x54, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xAA, 0xA0, 0xA8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xA2, 0x22, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01, 0x01, 0x00,
    0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x8F, 0x8F, 0x8F, 0x8F, 0x8F, 0x17, 0x41, 0xA8, 0x32, 0x30,
    0x00, 0x00
};

// Slower OEM quality waveform. It is useful as a diagnostic because it has
// distinct phases for all four absolute levels, unlike the fast anti-ghosting
// waveform above. The two LUT families are deliberately kept as separate
// test versions; only one waveform variable changes between them.
constexpr uint8_t kGray4QualityLut[110] = {
    0x00, 0x4A, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x62, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x88, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xA8, 0x44, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x08, 0x0B, 0x02, 0x03, 0x00, 0x0C, 0x02, 0x07, 0x02, 0x00,
    0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x17, 0x41, 0xA8, 0x32, 0x30
};

// Spatial-wavefront experiment. This is exactly kGray4Lut with one global
// frame-rate/timing byte shortened (byte 104: 0x8F -> 0x4F). SSD1677 scans
// gates in its own fixed order; this byte changes waveform duration only and
// cannot command a left-to-right spatial wave. The test exists to make that
// limitation observable while retaining the known-good LUT for rollback.
constexpr uint8_t kGray4WavefrontLut[112] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x54, 0x54, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xAA, 0xA0, 0xA8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xA2, 0x22, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01, 0x01, 0x00,
    0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x8F, 0x8F, 0x8F, 0x8F, 0x4F, 0x17, 0x41, 0xA8, 0x32, 0x30,
    0x00, 0x00
};

// Paper Mono's three-level selector waveform.  The two RAM planes are not
// ordinary old/new images here: 0x24 and 0x26 are selector planes for white,
// gray and black classes.  Keep the generator next to the hardware tests so
// the button exercises the same 111-byte host-authored LUT as the reference
// driver, rather than a copied opaque byte table.
constexpr uint8_t kPaperVsBlack = 0x01;  // VSH1
constexpr uint8_t kPaperVsWhite = 0x02;  // VSL
constexpr uint8_t kPaperVsWeak = 0x03;   // VSH2

// These five bytes are part of the Paper Mono waveform contract, even though
// the first 105 bytes are the visible VS/TP schedule.  The SSD1677 driver
// sends them to registers 0x03/0x04/0x2C after command 0x32.  Leaving them at
// zero (the default of PaperMonoWaveLut) makes VSL/VCOM collapse toward an
// invalid low-voltage state; the nominal white background then settles as a
// gray veil.  Keep the values in one place beside the reference LUT builder.
constexpr uint8_t kPaperVoltVgh = 0x17;
constexpr uint8_t kPaperVoltVsh1 = 0x41;
constexpr uint8_t kPaperVoltVsh2 = 0xA8;
constexpr uint8_t kPaperVoltVsl = 0x32;
constexpr uint8_t kPaperVoltVcom = 0x30;

struct PaperMonoWaveLut {
    uint8_t b[111]{};

    void set_vs(uint8_t entry, uint8_t group, uint8_t phase, uint8_t vs) {
        if (entry >= 5 || group >= 10 || phase >= 4) return;
        const uint8_t shift = static_cast<uint8_t>((3 - phase) * 2);
        b[entry * 10 + group] = static_cast<uint8_t>(
            (b[entry * 10 + group] & ~(0x03u << shift)) | ((vs & 0x03u) << shift));
    }

    void set_tp(uint8_t group, uint8_t a, uint8_t bb, uint8_t c, uint8_t d, uint8_t repeat) {
        if (group >= 10) return;
        b[50 + group * 5 + 0] = a;
        b[50 + group * 5 + 1] = bb;
        b[50 + group * 5 + 2] = c;
        b[50 + group * 5 + 3] = d;
        b[50 + group * 5 + 4] = repeat;
    }

    void finish() {
        // 0x08 = 5 ms per waveform frame; two nibbles per group byte.
        for (uint8_t i = 0; i < 5; ++i) b[100 + i] = 0x88;
    }
};

uint16_t BuildPaperMonoTriLut(uint8_t out[112], bool background_topup = true) {
    if (out == nullptr) return 0;
    PaperMonoWaveLut lut;
    constexpr uint8_t kick = 16;
    constexpr uint8_t gray_frames = 24;
    constexpr uint8_t black_frames = 32;
    constexpr uint8_t bg_topup_black = 1;
    constexpr uint8_t bg_topup_white = 5;

    const uint16_t white_for[3] = {
        kick,
        static_cast<uint16_t>(kick + gray_frames / 3),
        static_cast<uint16_t>(black_frames > kick ? black_frames - kick : 0),
    };

    uint8_t group = 0;
    uint16_t frames = 0;
    if (kick > 0) {
        const uint8_t phase_len[3] = {
            bg_topup_black,
            bg_topup_white,
            static_cast<uint8_t>(kick - bg_topup_black - bg_topup_white),
        };
        for (uint8_t phase = 0; phase < 3; ++phase) {
            if (phase_len[phase] == 0) continue;
            lut.set_vs(1, group, phase, kPaperVsBlack);
            lut.set_vs(2, group, phase, kPaperVsBlack);
            lut.set_vs(3, group, phase, kPaperVsWhite);
        }
        // Entry 0 is unchanged white. Its tiny black/white top-up is part of
        // the same activation and is what keeps a static background clean.
        if (background_topup) {
            lut.set_vs(0, group, 0, kPaperVsBlack);
            lut.set_vs(0, group, 1, kPaperVsWhite);
        }
        lut.set_tp(group, phase_len[0], phase_len[1], phase_len[2], 0, 0);
        frames = static_cast<uint16_t>(frames + kick);
        ++group;
    }

    const uint16_t trajectory[3] = {
        white_for[0],
        static_cast<uint16_t>(white_for[1] + gray_frames),
        static_cast<uint16_t>(white_for[2] + black_frames),
    };
    const uint16_t tail = std::max(trajectory[0], std::max(trajectory[1], trajectory[2]));
    const uint16_t start[3] = {
        static_cast<uint16_t>(tail - trajectory[0]),
        static_cast<uint16_t>(tail - trajectory[1]),
        static_cast<uint16_t>(tail - trajectory[2]),
    };

    uint16_t bounds[7] = {
        0,
        start[0],
        start[1],
        static_cast<uint16_t>(start[1] + white_for[1]),
        start[2],
        static_cast<uint16_t>(start[2] + white_for[2]),
        tail,
    };
    std::sort(bounds, bounds + 7);
    uint16_t previous = 0;
    for (uint8_t i = 0; i < 7 && group < 10; ++i) {
        const uint16_t current = bounds[i];
        if (current <= previous) continue;
        const uint8_t length = static_cast<uint8_t>(current - previous);
        if (previous >= start[0]) lut.set_vs(1, group, 0, kPaperVsWhite);
        if (previous >= start[1]) {
            const uint16_t gray_end = static_cast<uint16_t>(start[1] + white_for[1]);
            lut.set_vs(2, group, 0, previous < gray_end ? kPaperVsWhite : kPaperVsWeak);
        }
        if (previous >= start[2]) {
            const uint16_t black_end = static_cast<uint16_t>(start[2] + white_for[2]);
            lut.set_vs(3, group, 0, previous < black_end ? kPaperVsWhite : kPaperVsBlack);
        }
        lut.set_tp(group, length, 0, 0, 0, 0);
        frames = static_cast<uint16_t>(frames + length);
        previous = current;
        ++group;
    }
    lut.finish();
    std::memcpy(out, lut.b, sizeof(lut.b));
    // epaper_panel_write_custom_lut() treats bytes 105..109 as the analog
    // tail (VGH, VSH1, VSH2, VSL, VCOM), unlike FreeInk's internal 111-byte
    // builder which writes these registers separately.  Copying the schedule
    // without this tail was the reason the text page's white class looked
    // uniformly gray on glass.
    out[105] = kPaperVoltVgh;
    out[106] = kPaperVoltVsh1;
    out[107] = kPaperVoltVsh2;
    out[108] = kPaperVoltVsl;
    out[109] = kPaperVoltVcom;
    out[110] = 0;
    out[111] = 0;
    return frames;
}

// Compact seven-segment digits remain useful for the hardware test pattern;
// the product home page uses the generated bitmap clock font below.
constexpr uint8_t kSegments[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66,
                                   0x6d, 0x7d, 0x07, 0x7f, 0x6f};

// Product layout: one 32 px safe area, quiet rules and generous line spacing.
// Rendering and touch routing share these bounds. The legacy dashboard and
// diagnostic waveforms below retain their original geometry.
constexpr int kUiInset = 32;
constexpr int kUiContentWidth = kPortraitW - kUiInset * 2;
constexpr int kUiTitleY = 72;
constexpr int kUiBodyY = 144;
constexpr int kUiRowPitch = 80;
constexpr int kUiRowHeight = 72;
constexpr int kUiCardPitch = 96;
constexpr int kUiCardHeight = 88;
constexpr int kUiRailY = 752;
constexpr int kUiHomeX[2] = {32, 248};
constexpr int kUiHomeY[2] = {296, 464};
constexpr int kUiHomeW = 200;
constexpr int kUiHomeH = 144;
constexpr int kUiHomeNavY = 664;
constexpr int kUiHomeNavH = 64;
constexpr int kAiActionY[2] = {616, 680};
constexpr int kAiActionX[2] = {32, 248};
constexpr int kAiActionW = 200;
constexpr int kAiActionH = 48;
constexpr int kAiLinesPerPage = 7;

constexpr int ProductRowAt(int x, int y, int count, int pitch, int height) {
    if (x < kUiInset || x >= kUiInset + kUiContentWidth || y < kUiBodyY ||
        y >= kUiBodyY + count * pitch || (y - kUiBodyY) % pitch >= height) return -1;
    return (y - kUiBodyY) / pitch;
}

constexpr int kTouchVirtualKeyMinY = kPortraitH;
constexpr int kTouchVirtualKeyMaxY = 980;
constexpr int kTouchVirtualKeyTolerance = 88;

constexpr int kMargin = 32;
constexpr int kContentWidth = kPortraitW - kMargin * 2;
// The home page is a small editorial day-sheet.  Every module sits on the
// same 32px safe area and uses the 4/8px spacing rhythm from the UI system.
// Keep the interactive bounds shared with the touch hit regions below.
constexpr int kHeaderRuleY = 148;
constexpr int kWeatherY = 158;
constexpr int kWeatherH = 86;
constexpr int kWeatherRuleY = 244;
constexpr int kHeroY = 260;
constexpr int kHeroH = 120;
constexpr int kHeroRuleY = 380;
constexpr int kScheduleHeaderY = 398;
constexpr int kScheduleFirstRowY = 434;
constexpr int kScheduleRowHeight = 44;
constexpr int kScheduleRowGap = 0;
constexpr int kQuotaY = 590;
constexpr int kQuotaH = 94;
constexpr int kQuotaRuleY = 684;
constexpr int kCustomY = 700;
constexpr int kCustomH = 78;
constexpr int kCustomRuleY = 788;
constexpr size_t kFrameDumpChunkBytes = 48;
constexpr size_t kFrameDumpLineBytes = 192;

uint32_t FrameCrc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u &
                                (0u - static_cast<uint32_t>(crc & 1u)));
        }
    }
    return ~crc;
}

bool SerialWriteAll(int fd, const char* data, size_t size) {
    // Replies are written through the USB Serial/JTAG driver. `fd` is kept only
    // so call sites can keep passing their channel handle; a missing
    // /dev/secondary VFS node must never suppress a reply.
    (void)fd;
    if (data == nullptr || !usb_serial_jtag_is_driver_installed()) return false;
    // VFS reports the requested byte count even when its per-character timeout
    // drops data. Use the driver's actual count and serialize complete protocol
    // lines with the default printf logger.
    struct OutputLock {
        OutputLock() { flockfile(stdout); }
        ~OutputLock() { funlockfile(stdout); }
    } lock;
    size_t offset=0;
    const int64_t deadline=esp_timer_get_time()+2000000;
    while (offset<size && esp_timer_get_time()<deadline) {
        const int written=usb_serial_jtag_write_bytes(data+offset,std::min(size-offset,size_t(256)),pdMS_TO_TICKS(10));
        if (written<0) return false;
        if (written>0) offset+=static_cast<size_t>(written);
        else vTaskDelay(pdMS_TO_TICKS(1));
    }
    return offset==size;
}

uint32_t Utf8Next(const char** cursor) {
    if (cursor == nullptr || *cursor == nullptr || **cursor == '\0') return 0;
    const auto* bytes = reinterpret_cast<const uint8_t*>(*cursor);
    const uint8_t first = *bytes;
    if (first < 0x80U) {
        *cursor = reinterpret_cast<const char*>(bytes + 1);
        return first;
    }
    if (first < 0xC2U || first > 0xF4U) {
        *cursor = reinterpret_cast<const char*>(bytes + 1);
        return 0xFFFD;
    }
    const int extra = (first & 0xE0U) == 0xC0U ? 1 : ((first & 0xF0U) == 0xE0U ? 2 : 3);
    uint32_t codepoint = first & (extra == 1 ? 0x1FU : (extra == 2 ? 0x0FU : 0x07U));
    ++bytes;
    for (int i = 0; i < extra; ++i) {
        if (bytes[i] == 0 || (bytes[i] & 0xC0U) != 0x80U) {
            *cursor = reinterpret_cast<const char*>(bytes - 1 + 1);
            return 0xFFFD;
        }
        codepoint = (codepoint << 6) | (bytes[i] & 0x3FU);
    }
    if (codepoint > 0x10FFFFU || (codepoint >= 0xD800U && codepoint <= 0xDFFFU) ||
        (extra == 1 && codepoint < 0x80U) ||
        (extra == 2 && codepoint < 0x800U) ||
        (extra == 3 && codepoint < 0x10000U)) {
        *cursor = reinterpret_cast<const char*>(bytes - extra);
        return 0xFFFD;
    }
    *cursor = reinterpret_cast<const char*>(bytes + extra);
    return codepoint;
}

const ui_glyph_t* FindGlyph(const ui_font_t& font, uint32_t codepoint) {
    uint16_t lo = 0;
    uint16_t hi = font.glyph_count;
    while (lo < hi) {
        const uint16_t mid = static_cast<uint16_t>(lo + (hi - lo) / 2U);
        if (font.codepoints[mid] < codepoint) lo = static_cast<uint16_t>(mid + 1U);
        else hi = mid;
    }
    return lo < font.glyph_count && font.codepoints[lo] == codepoint
               ? &font.glyphs[lo]
               : nullptr;
}

void CopyDisplayText(char* destination, size_t destination_size, const char* source) {
    dashboard::CopyText(destination, destination_size, source);
}
}

RawDisplay* RawDisplay::instance_ = nullptr;

RawDisplay::RawDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io,
                       esp_lcd_touch_handle_t touch, int width, int height)
    : panel_(panel), panel_io_(panel_io), touch_(touch) {
    raw_font::Init();
    width_ = width;
    height_ = height;
    portrait_size_ = static_cast<size_t>(kPortraitW / 8) * kPortraitH;
    panel_size_ = static_cast<size_t>(kPanelW / 8) * kPanelH;
    portrait_fb_ = static_cast<uint8_t*>(heap_caps_malloc(portrait_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    panel_fb_ = static_cast<uint8_t*>(heap_caps_malloc(panel_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    panel_prev_fb_ = static_cast<uint8_t*>(heap_caps_malloc(panel_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    panel_region_fb_ = static_cast<uint8_t*>(heap_caps_malloc(panel_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    frame_dump_fb_ = static_cast<uint8_t*>(heap_caps_malloc(panel_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    glass_nonwhite_fb_ = static_cast<uint8_t*>(heap_caps_malloc(panel_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    glass_black_fb_ = static_cast<uint8_t*>(heap_caps_malloc(panel_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    mutex_ = xSemaphoreCreateMutex();
    instance_ = this;
    if (portrait_fb_ == nullptr || panel_fb_ == nullptr || panel_prev_fb_ == nullptr ||
        panel_region_fb_ == nullptr || frame_dump_fb_ == nullptr || glass_nonwhite_fb_ == nullptr ||
        glass_black_fb_ == nullptr || mutex_ == nullptr) {
        ESP_LOGE(TAG, "raw framebuffer allocation failed");
    }
    if (panel_prev_fb_) std::memset(panel_prev_fb_, kWhite, panel_size_);
    if (glass_nonwhite_fb_) std::memset(glass_nonwhite_fb_, 0, panel_size_);
    if (glass_black_fb_) std::memset(glass_black_fb_, 0, panel_size_);
    if (touch_ != nullptr) {
        // Product-page navigation renders bounded UTF-8 lines on the touch
        // task's call stack. Keep the same headroom as the event task so a
        // button press cannot turn a page change into a stack-overflow reset.
        if (xTaskCreatePinnedToCore(&RawDisplay::TouchTaskEntry, "raw_touch", 8192, this, 4,
                                    &touch_task_, 0) != pdPASS) {
            ESP_LOGW(TAG, "raw touch task creation failed");
            touch_task_ = nullptr;
        }
    }
    if (frame_dump_fb_ != nullptr) {
        if (xTaskCreatePinnedToCore(&RawDisplay::FrameDumpTaskEntry, "frame_dump", 8192,
                                    this, 1, &frame_dump_task_, 0) != pdPASS) {
            ESP_LOGW(TAG, "serial frame task creation failed");
            frame_dump_task_ = nullptr;
        }
    }
}

RawDisplay::~RawDisplay() {
    if (instance_ == this) instance_ = nullptr;
    if (animation_task_ != nullptr) {
        animation_running_ = false;
        vTaskDelete(animation_task_);
        animation_task_ = nullptr;
    }
    if (touch_task_ != nullptr) {
        vTaskDelete(touch_task_);
        touch_task_ = nullptr;
    }
    frame_dump_stop_ = true;
    if (frame_dump_task_ != nullptr) {
        vTaskDelete(frame_dump_task_);
        frame_dump_task_ = nullptr;
    }
    if (portrait_fb_) heap_caps_free(portrait_fb_);
    if (panel_fb_) heap_caps_free(panel_fb_);
    if (panel_prev_fb_) heap_caps_free(panel_prev_fb_);
    if (panel_region_fb_) heap_caps_free(panel_region_fb_);
    if (frame_dump_fb_) heap_caps_free(frame_dump_fb_);
    if (glass_nonwhite_fb_) heap_caps_free(glass_nonwhite_fb_);
    if (glass_black_fb_) heap_caps_free(glass_black_fb_);
    if (mutex_) vSemaphoreDelete(mutex_);
}

bool RawDisplay::Lock(int timeout_ms) {
    return mutex_ != nullptr && xSemaphoreTake(mutex_, timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void RawDisplay::Unlock() { if (mutex_) xSemaphoreGive(mutex_); }

void RawDisplay::SetPixel(int x, int y, bool black) {
    if (x < 0 || x >= kPortraitW || y < 0 || y >= kPortraitH) return;
    uint8_t& b = portrait_fb_[static_cast<size_t>(y) * (kPortraitW / 8) + (x >> 3)];
    const uint8_t mask = static_cast<uint8_t>(0x80u >> (x & 7));
    if (black) b &= static_cast<uint8_t>(~mask); else b |= mask;
}

void RawDisplay::TouchTaskEntry(void* arg) {
    auto* self = static_cast<RawDisplay*>(arg);
    if (self != nullptr) self->TouchTask();
    vTaskDelete(nullptr);
}

void RawDisplay::TouchTask() {
    ESP_LOGI(TAG, "raw CST816S touch polling started");
    esp_lcd_touch_point_data_t point{};
    while (true) {
        uint8_t count = 1;
        bool pressed = false;
        if (touch_ != nullptr && esp_lcd_touch_read_data(touch_) == ESP_OK &&
            esp_lcd_touch_get_data(touch_, &point, &count, 1) == ESP_OK && count > 0) {
            pressed = true;
        }
        if (pressed) {
            // The cover exposes three capacitive keys below the visible panel.
            // CST816S reports those points in the same portrait coordinate
            // space (y=900 on this board), so keep them out of the framebuffer
            // hit boxes instead of clamping them onto the last screen row.
            const int reported_x = static_cast<int>(point.x);
            const int reported_y = static_cast<int>(point.y);
            const bool virtual_key = reported_y >= kTouchVirtualKeyMinY &&
                                     reported_y <= kTouchVirtualKeyMaxY;
            const int x = std::clamp(reported_x, 0, kPortraitW - 1);
            const int y = virtual_key ? reported_y
                                      : std::clamp(reported_y, 0, kPortraitH - 1);
            if (!touch_down_) {
                touch_down_ = true;
                touch_start_x_ = touch_last_x_ = x;
                touch_start_y_ = touch_last_y_ = y;
                touch_start_ms_ = esp_timer_get_time() / 1000;
            } else {
                touch_last_x_ = x;
                touch_last_y_ = y;
            }
        } else if (touch_down_) {
            const int64_t held_ms = esp_timer_get_time() / 1000 - touch_start_ms_;
            const int dx = touch_last_x_ - touch_start_x_;
            const int dy = touch_last_y_ - touch_start_y_;
            const bool tap = held_ms <= 800 && std::abs(dx) <= 32 && std::abs(dy) <= 32;
            const int x = touch_last_x_;
            const int y = touch_last_y_;
            touch_down_ = false;
            const int start_x=touch_start_x_,start_y=touch_start_y_;
            const auto pull=input::ControlPull(start_x,start_y,x,y,static_cast<int>(held_ms),quick_controls_open_.load());
            if(pull!=input::Pull::None) {
                Application::GetInstance().ScheduleUi([this,start_x,start_y,x,y,held_ms]() {
                    HandleQuickPull(start_x,start_y,x,y,static_cast<int>(held_ms));
                });
            } else if (tap) {
                if (touch_start_y_ >= kTouchVirtualKeyMinY &&
                    touch_start_y_ <= kTouchVirtualKeyMaxY) {
                    const int home_distance = std::abs(x - static_cast<int>(TOUCH_VK_HOME_X));
                    const int next_distance = std::abs(x - static_cast<int>(TOUCH_VK_NEXT_X));
                    const int prev_distance = std::abs(x - static_cast<int>(TOUCH_VK_PREV_X));
                    if (home_distance <= kTouchVirtualKeyTolerance &&
                        home_distance <= next_distance && home_distance <= prev_distance) {
                        Application::GetInstance().ScheduleUi([this]() { HandleHardwareKey(HardwareKey::Home); });
                    } else if (next_distance <= kTouchVirtualKeyTolerance &&
                               next_distance <= prev_distance) {
                        Application::GetInstance().ScheduleUi([this]() { HandleHardwareKey(HardwareKey::Next); });
                    } else if (prev_distance <= kTouchVirtualKeyTolerance) {
                        Application::GetInstance().ScheduleUi([this]() { HandleHardwareKey(HardwareKey::Previous); });
                    }
                } else {
                    Application::GetInstance().ScheduleUi([this, x, y]() { HandleHomeTap(x, y); });
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

bool RawDisplay::HandleAiKey(bool down) {
    if (down) {
        if (form_active_.load() || quick_controls_open_.load() || device::QuickControls::Instance().Snapshot().ble_busy) return false;
        if (xiaozhi::AudioSession::GetInstance().RecorderState().mode != audio::RecorderMode::Idle) return false;
        if (ai_key_down_.exchange(true)) return true;
        const bool accepted = xiaozhi::Client::GetInstance().ListenStart();
        // Queue only visual work. A slow e-paper flush must never delay UP.
        Application::GetInstance().RequestAiFocus();
        return accepted;
    }
    if (!ai_key_down_.exchange(false)) return false;
    const bool stopped = xiaozhi::Client::GetInstance().ListenStop();
    Application::GetInstance().RequestStatusUpdate(true);
    return stopped;
}

void RawDisplay::ShowAiConversation() {
    SetPowerSaveMode(false);
    DisplayLockGuard lock(this);
    if (animation_running_.load() || form_active_.load() || quick_controls_open_.load()) return;
    screen_test_mode_ = false;
    test_console_mode_ = false;
    product_page_ = ProductPage::AiResult;
    ai_text_page_ = 0;
    ai_show_transcript_ = voice_note_mode_;
    DrawHomeScreenLocked();
    FlushLocked();
}

void RawDisplay::HandleHomeTap(int x, int y) {
    if (HandleReminderTap(x,y)) return;
    if (HandleQuickTap(x,y)) return;
    if (HandleHistoryTap(x,y)) return;
    if (HandleSetupTap(x,y)) return;
    if (HandleReaderTap(x,y)) return;
    enum class Action { None, Gray4, PaperMono, PaperText, AnimDu, AnimFc, PaperPage };
    Action action = Action::None;
    int ai_action = -1;
    int recorder_action = -1;
    uint32_t alarm_id = 0;
    bool alarm_enable = false;
    const char* voice_notice = nullptr;
    bool restore_capsule = false;
    bool confirm_tap = false;
    bool wake = false;
    bool redraw = false;
    {
        DisplayLockGuard lock(this);
        if (test_console_mode_) {
            for (int i = 0; i < kTestButtonCount; ++i) {
                if (x >= kTestButtonX && x < kTestButtonX + kTestButtonW &&
                    y >= kTestButtonY[i] && y < kTestButtonY[i] + kTestButtonH) {
                    action = static_cast<Action>(i + 1);
                    break;
                }
            }
        } else if (screen_test_mode_) return;
        else if (power_save_) {
            wake = true;
        } else {
            auto in_rect = [x, y](int left, int top, int width, int height) {
                return x >= left && x < left + width && y >= top && y < top + height;
            };
            // Content taps share the hardware actions. The passive footer and
            // the gaps between list rows never activate a selection.
            ProductPage next_page = product_page_;
            switch (product_page_) {
                case ProductPage::Home: {
                    static constexpr ProductPage pages[] = {ProductPage::Alarm, ProductPage::TodayList,
                        ProductPage::Notes, ProductPage::AiResult, ProductPage::Apps, ProductPage::More};
                    for (int i = 0; i < 4; ++i) {
                        if (in_rect(kUiHomeX[i % 2], kUiHomeY[i / 2], kUiHomeW, kUiHomeH)) next_page = pages[i];
                    }
                    for (int i = 0; i < 2; ++i) {
                        if (in_rect(kUiHomeX[i], kUiHomeNavY, kUiHomeW, kUiHomeNavH)) next_page = pages[i + 4];
                    }
                    app_parent_ = ProductPage::Home;
                    if (next_page == ProductPage::AiResult) voice_note_mode_=false;
                    break;
                }
                case ProductPage::Apps:
                case ProductPage::More: {
                    const int count = product_page_ == ProductPage::Apps ? 7 : 5;
                    const int row = ProductRowAt(x, y, count, kUiRowPitch, kUiRowHeight);
                    if (row >= 0) {
                        navigation_index_ = row;
                        confirm_tap = true;
                    }
                    break;
                }
                case ProductPage::AiResult:
                case ProductPage::AiSteps: {
                    const auto state=xiaozhi::Conversation::GetInstance().Snapshot();
                    const bool busy=state.state==xiaozhi::TurnState::Connecting || state.state==xiaozhi::TurnState::Listening ||
                        state.state==xiaozhi::TurnState::Transcribing || state.state==xiaozhi::TurnState::Thinking || state.state==xiaozhi::TurnState::Speaking;
                    if(in_rect(344,72,104,48) && busy) ai_action=4;
                    else if (in_rect(32, 288, 184, 48)) {
                        ai_show_transcript_ = !ai_show_transcript_;
                        ai_text_page_ = 0;
                        redraw = true;
                    } else {
                        for (int row = 0; row < 2; ++row) for (int col = 0; col < 2; ++col) {
                            const int candidate=row*2+col;
                            if(!busy && !state.transcript.empty() && !state.truncated &&
                               (candidate!=3 || state.state==xiaozhi::TurnState::Done) &&
                               in_rect(kAiActionX[col], kAiActionY[row], kAiActionW, kAiActionH)) ai_action=candidate;
                        }
                    }
                    break;
                }
                case ProductPage::QuickNote:
                    if (in_rect(kUiInset, 504, kUiContentWidth, 64)) {
                        restore_capsule = true;
                        next_page = ProductPage::AiResult;
                    }
                    break;
                case ProductPage::TodayList: {
                    const auto now = time(nullptr);
                    if (!reminders::ValidClock(now)) break;
                    if (in_rect(32,136,64,48) || in_rect(384,136,64,48)) {
                        calendar_month_ = std::clamp(calendar_month_ + (x < 240 ? -1 : 1), -120, 120);
                        calendar_day_ = 1; calendar_events_page_ = 0; redraw = true;
                    } else if (in_rect(44,232,392,264)) {
                        struct tm month{}; localtime_r(&now,&month);
                        month.tm_mday=1; month.tm_mon+=calendar_month_; month.tm_hour=month.tm_min=month.tm_sec=0;
                        const auto first=mktime(&month);
                        const int lead=(month.tm_wday+6)%7;
                        struct tm next=month; ++next.tm_mon;
                        const int days=(mktime(&next)-first)/86400;
                        const int day=(y-232)/44*7+(x-44)/56-lead+1;
                        if (day>=1 && day<=days) { calendar_day_=day; calendar_events_page_=0; redraw=true; }
                    } else if (in_rect(32,688,200,48)) {
                        next_page=ProductPage::AiResult; voice_notice="按住 AI 键，说出日程与时间";
                    } else if (in_rect(248,688,200,48)) { ++calendar_events_page_; redraw=true; }
                    break;
                }
                case ProductPage::Alarm:
                    for (int row=0;row<4;++row) {
                        if (alarm_ids_[row] && in_rect(344,160+row*112,104,48)) {
                            alarm_id=alarm_ids_[row]; alarm_enable=!alarm_enabled_[row];
                        }
                    }
                    if (in_rect(32,600,416,48)) {
                        next_page=ProductPage::AiResult; voice_notice="按住 AI 键，说出闹钟时间";
                    } else if (alarm_pages_>1 && in_rect(32,672,200,48)) {
                        alarm_page_=(alarm_page_+alarm_pages_-1)%alarm_pages_; redraw=true;
                    } else if (alarm_pages_>1 && in_rect(248,672,200,48)) {
                        alarm_page_=(alarm_page_+1)%alarm_pages_; redraw=true;
                    }
                    break;
                case ProductPage::Notes: {
                    const int row=notes_ui::RowAt(x,y);
                    if (row>=0 && note_ids_[row]) {note_id_=note_ids_[row];note_text_page_=0;next_page=ProductPage::NoteDetail;}
                    else if (in_rect(32,672,200,48)) {next_page=ProductPage::AiResult;voice_note_mode_=false;voice_notice="按住 AI 键，说：帮我保存一条笔记";}
                    else if (in_rect(248,672,200,48)) {notes_page_=(notes_page_+1)%notes_pages_;navigation_index_=0;redraw=true;}
                    break;
                }
                case ProductPage::NoteDetail:
                    if (in_rect(32,672,200,48) && note_text_page_>0) {--note_text_page_;redraw=true;}
                    else if (in_rect(248,672,200,48) && note_text_page_+1<note_text_pages_) {++note_text_page_;redraw=true;}
                    break;
                case ProductPage::Recorder: {
                    const auto state=xiaozhi::AudioSession::GetInstance().RecorderState();
                    const bool active=state.mode!=audio::RecorderMode::Idle;
                    if (in_rect(32,496,416,64)) recorder_action=active ? 0 : 1;
                    else if (!active && state.has_clip && in_rect(32,584,416,64)) recorder_action=2;
                    else if (!active && in_rect(32,672,416,64)) {
                        next_page=ProductPage::AiResult; voice_notice="按住 AI 键录音，松开生成文字笔记";
                        voice_note_mode_=true; ai_show_transcript_=true;
                    }
                    break;
                }
                case ProductPage::ChatList:
                case ProductPage::ChatDetail:
                case ProductPage::WifiList:
                case ProductPage::WifiCredentials:
                case ProductPage::TextEntry:
                case ProductPage::NoteCompose:
                case ProductPage::Workbench:
                case ProductPage::Settings:
                    break;
                case ProductPage::CardBox: {
                    const auto snapshot = dashboard::DashboardData::GetInstance().GetSnapshot();
                    int count = 0;
                    for (const auto& card : snapshot.custom) if (card.enabled) ++count;
                    for (const auto& summary : snapshot.ai_summary) if (summary[0]) ++count;
                    const int selected=ProductRowAt(x,y,std::min(count,3),kUiCardPitch,kUiCardHeight);
                    if(selected>=0) {
                        if(SelectCardSnapshotLocked(selected)) next_page=ProductPage::CardDetail;
                    }
                    break;
                }
                case ProductPage::Keep:
                    next_page = ProductPage::Home;
                    break;
                case ProductPage::Reader:
                case ProductPage::CardDetail:
                case ProductPage::Confirmation:
                    break;
            }
            if (next_page != product_page_) {
                if (product_page_ == ProductPage::Recorder) xiaozhi::AudioSession::GetInstance().StopRecorder();
                product_page_ = next_page;
                if (next_page == ProductPage::Recorder) xiaozhi::AudioSession::GetInstance().RestoreRecorder();
                navigation_index_ = 0;
                redraw = true;
            }
        }

        // HOME from the legacy console or a diagnostic page lands here too;
        // draw it once after the router has released its page-specific branch.
        if (redraw && !wake && !test_console_mode_ && !screen_test_mode_) {
            DrawHomeScreenLocked();
            FlushLocked();
        }
    }
    if (action == Action::Gray4) {
        ShowGray4TestPattern();
        return;
    }
    if (action == Action::PaperMono) {
        (void)StartPaperMonoTest();
        return;
    }
    if (action == Action::PaperText) {
        (void)StartPaperMonoTextTest();
        return;
    }
    if (action == Action::AnimDu) {
        (void)StartAnimationTest(true);
        return;
    }
    if (action == Action::AnimFc) {
        (void)StartAnimationTest(false);
        return;
    }
    if (action == Action::PaperPage) {
        (void)StartPaperMonoPageTest();
        return;
    }
    if (wake) {
        SetPowerSaveMode(false);
        return;
    }
    if (alarm_id) {
        std::string error;
        if (!reminders::Service::Instance().SetEnabled(alarm_id,alarm_enable,error))
            ShowNotification("无法修改，请用语音重新设置时间",3000);
        else UpdateStatusBar(true);
    }
    if (recorder_action >= 0) {
        auto& audio=xiaozhi::AudioSession::GetInstance();
        if (recorder_action==0) audio.StopRecorder();
        else if (!audio.StartRecorder(recorder_action==2)) ShowNotification("音频忙碌，请稍后再试",2000);
        UpdateStatusBar(true);
    }
    if (voice_notice) ShowNotification(voice_notice,5000);
    if (restore_capsule) xiaozhi::Client::GetInstance().RestoreCapsule();
    if (ai_action >= 0) {
        auto& client = xiaozhi::Client::GetInstance();
        if (ai_action < 3) {
            if (!client.RunQuickAction(static_cast<xiaozhi::QuickAction>(ai_action))) {
                ShowNotification("暂不可操作，请等待当前回答或先说话", 4000);
            }
        } else if (ai_action == 3) {
            OpenConversationNote();
        } else {
            (void)client.Abort();
        }
        UpdateStatusBar(true);
    }
    if (confirm_tap) {
        // Dispatch after releasing the display mutex: the hardware handler
        // acquires it and may start network/audio work outside its own lock.
        (void)HandleHardwareKey(HardwareKey::Select);
    }
}

bool RawDisplay::HandleHardwareKey(HardwareKey key) {
    if (reminders::Service::Instance().IsActive()) return true;
    if (HandleQuickKey(key)) return true;
    if (HandleHistoryKey(key)) return true;
    if (HandleSetupKey(key)) return true;
    if (HandleReaderKey(key)) return true;
    bool scan_wifi = false;
    bool open_controls = false;
    bool restore_recent_capsule = false;
    bool refresh_tap = false;
    bool wake = false;
    bool redraw = false;
    bool handled = false;
    const char* hardware_notice = nullptr;

    {
        DisplayLockGuard lock(this);
        if (!portrait_fb_) return false;

        // The legacy test console owns its own touch geometry.  HOME is still
        // a useful escape hatch from the cover, while NEXT/PREV only advance
        // its test variant and never leak into the product router.
        if (test_console_mode_) {
            handled = true;
            if (key == HardwareKey::Home) {
                test_console_mode_ = false;
                product_page_ = ProductPage::Home;
                navigation_index_ = 0;
                redraw = true;
            } else if (key == HardwareKey::Next || key == HardwareKey::Previous) {
                if (key == HardwareKey::Next) ++test_variant_;
                else test_variant_ = test_variant_ == 0 ? kGrayVariantCount - 1
                                                        : test_variant_ - 1;
                DrawTestConsoleLocked();
                FlushLocked();
            }
        } else if (screen_test_mode_) {
            // A running diagnostic is intentionally not interrupted by a
            // cover-key tap, except for HOME which returns to the product UI.
            handled = key == HardwareKey::Home;
            if (handled) {
                screen_test_mode_ = false;
                product_page_ = ProductPage::Home;
                navigation_index_ = 0;
                redraw = true;
            }
        } else if (power_save_) {
            handled = true;
            wake = true;
        } else {
            handled = true;
            auto set_page = [this, &redraw](ProductPage page) {
                if (product_page_ == page) return;
                if (product_page_ == ProductPage::Recorder) xiaozhi::AudioSession::GetInstance().StopRecorder();
                if (product_page_ == ProductPage::Apps || product_page_ == ProductPage::Home) app_parent_ = product_page_;
                product_page_ = page;
                if (page == ProductPage::Recorder) xiaozhi::AudioSession::GetInstance().RestoreRecorder();
                if (page == ProductPage::AiResult) voice_note_mode_=false;
                navigation_index_ = 0;
                redraw = true;
            };
            auto parent_page = [this, &set_page]() {
                switch (product_page_) {
                    case ProductPage::Home: break;
                    case ProductPage::AiSteps: set_page(ProductPage::AiResult); break;
                    case ProductPage::CardDetail: set_page(ProductPage::CardBox); break;
                    case ProductPage::NoteDetail: set_page(ProductPage::Notes); break;
                    case ProductPage::Confirmation: set_page(ProductPage::QuickNote); break;
                    case ProductPage::Workbench: set_page(ProductPage::More); break;
                    case ProductPage::Settings: set_page(ProductPage::More); break;
                    case ProductPage::More: set_page(ProductPage::Home); break;
                    case ProductPage::Apps: set_page(ProductPage::Home); break;
                    default: set_page(app_parent_); break;
                }
            };
            auto open_home_selection = [this, &set_page]() {
                static constexpr ProductPage pages[] = {ProductPage::Alarm,ProductPage::TodayList,
                    ProductPage::Notes,ProductPage::AiResult,ProductPage::Apps,ProductPage::More};
                set_page(pages[navigation_index_ % 6]);
            };
            auto open_app_selection = [this, &set_page]() {
                static constexpr ProductPage pages[] = {ProductPage::Alarm,ProductPage::TodayList,
                    ProductPage::Recorder,ProductPage::AiResult,ProductPage::Notes,ProductPage::QuickNote,ProductPage::Reader};
                set_page(pages[navigation_index_ % 7]);
            };

            switch (key) {
                case HardwareKey::Home:
                    if (product_page_ != ProductPage::Home) {
                        set_page(ProductPage::Home);
                    }
                    break;
                case HardwareKey::Previous:
                case HardwareKey::Back:
                    if (product_page_==ProductPage::NoteDetail && key==HardwareKey::Previous && note_text_page_>0) {
                        --note_text_page_;redraw=true;
                    } else if (product_page_ == ProductPage::TodayList && key == HardwareKey::Previous) {
                        calendar_month_=std::max(-120,calendar_month_-1); calendar_day_=1; calendar_events_page_=0; redraw=true;
                    } else if (product_page_ == ProductPage::Alarm && alarm_page_>0 && key==HardwareKey::Previous) {
                        --alarm_page_; redraw=true;
                    } else if ((product_page_ == ProductPage::AiResult || product_page_ == ProductPage::AiSteps) && ai_text_page_ > 0) {
                        --ai_text_page_;
                        redraw = true;
                    } else {
                        parent_page();
                    }
                    break;
                case HardwareKey::Next:
                    if (product_page_ == ProductPage::Home) {
                        navigation_index_ = (navigation_index_ + 1) % 6;
                        redraw = true;
                    } else if (product_page_ == ProductPage::Apps) {
                        navigation_index_ = (navigation_index_ + 1) % 7;
                        redraw = true;
                    } else if (product_page_==ProductPage::Notes) {
                        if (navigation_index_<5 && note_ids_[navigation_index_+1]) ++navigation_index_;
                        else {notes_page_=(notes_page_+1)%notes_pages_;navigation_index_=0;}
                        redraw=true;
                    } else if (product_page_==ProductPage::NoteDetail) {
                        if (note_text_page_+1<note_text_pages_) {++note_text_page_;redraw=true;}
                    } else if (product_page_ == ProductPage::QuickNote) {
                        set_page(ProductPage::AiResult);restore_recent_capsule=true;
                    } else if (product_page_ == ProductPage::AiResult || product_page_ == ProductPage::AiSteps) {
                        if (ai_text_page_ + 1 < ai_page_count_) { ++ai_text_page_; redraw = true; }
                    } else if (product_page_ == ProductPage::TodayList) {
                        calendar_month_=std::min(120,calendar_month_+1); calendar_day_=1; calendar_events_page_=0; redraw=true;
                    } else if (product_page_ == ProductPage::Alarm) {
                        alarm_page_=(alarm_page_+1)%alarm_pages_; redraw=true;
                    } else if (product_page_ == ProductPage::CardBox) {
                        if(SelectCardSnapshotLocked(navigation_index_)) set_page(ProductPage::CardDetail);
                    } else if (product_page_ == ProductPage::CardDetail) {
                        set_page(ProductPage::Keep);
                    } else if (product_page_ == ProductPage::Keep) {
                        set_page(ProductPage::Home);
                    } else if (product_page_ == ProductPage::More) {
                        navigation_index_ = (navigation_index_ + 1) % 5;
                        redraw = true;
                    } else if (product_page_ == ProductPage::Confirmation) {
                        set_page(ProductPage::QuickNote);
                    }
                    break;
                case HardwareKey::Select:
                    if (product_page_ == ProductPage::Home) open_home_selection();
                    else if (product_page_ == ProductPage::Apps) open_app_selection();
                    else if (product_page_==ProductPage::Notes && note_ids_[navigation_index_]) {
                        note_id_=note_ids_[navigation_index_];note_text_page_=0;set_page(ProductPage::NoteDetail);
                    }
                    else if (product_page_ == ProductPage::AiResult) { ai_show_transcript_ = !ai_show_transcript_; ai_text_page_ = 0; redraw = true; }
                    else if (product_page_ == ProductPage::AiSteps) {
                        ai_show_transcript_=!ai_show_transcript_;ai_text_page_=0;redraw=true;
                    } else if (product_page_ == ProductPage::QuickNote) {
                        set_page(ProductPage::AiResult);restore_recent_capsule=true;
                    } else if (product_page_ == ProductPage::Reader) {
                        set_page(ProductPage::Apps);
                    } else if (product_page_ == ProductPage::CardBox) {
                        if(SelectCardSnapshotLocked(navigation_index_)) set_page(ProductPage::CardDetail);
                    } else if (product_page_ == ProductPage::CardDetail) {
                        set_page(ProductPage::Keep);
                    } else if (product_page_ == ProductPage::Keep) {
                        // Any hardware key wakes the keep-screen snapshot.
                        set_page(ProductPage::Home);
                    } else if (product_page_ == ProductPage::Confirmation) {
                        set_page(ProductPage::QuickNote);
                        hardware_notice = "打开最近胶囊后，选择存为笔记";
                    } else if (product_page_ == ProductPage::More) {
                        switch (navigation_index_ % 5) {
                            case 0:
                                ClearFormLocked(); wifi_page_=0; wifi_switch_confirm_=false;
                                set_page(ProductPage::WifiList); scan_wifi=true; break;
                            case 1: set_page(ProductPage::Workbench); break;
                            case 2: refresh_tap = true; break;
                            case 3: set_page(ProductPage::Settings); break;
                            default:
                                open_controls=true; break;
                        }
                    }

                    break;
            }

        }
        if (redraw && !wake && !test_console_mode_ && !screen_test_mode_) {
            DrawHomeScreenLocked();
            FlushLocked();
        }
    }

    if (wake) {
        SetPowerSaveMode(false);
        return true;
    }
    if(restore_recent_capsule) xiaozhi::Client::GetInstance().RestoreCapsule();
    if (open_controls) SetQuickControls(true);
    if (scan_wifi) (void)network::WifiSetup::Instance().Scan();
    if (refresh_tap) {
        dashboard::DashboardService::GetInstance().RefreshNow();
        ShowNotification("正在刷新天气与额度", 2000);
    }
    if (hardware_notice != nullptr) ShowNotification(hardware_notice, 1800);
    return handled;
}

void RawDisplay::FrameDumpTaskEntry(void* arg) {
    static_cast<RawDisplay*>(arg)->FrameDumpTask();
    vTaskDelete(nullptr);
}

void RawDisplay::FrameDumpTask() {
    // USB Serial/JTAG carries both the host commands and the frame replies.
    // Commands are read from the hardware FIFO so reopening the CDC port does
    // not make a pending TOUCH command disappear through a VFS read error.
    int fd = -1;
    char command[768] = {};
    size_t command_size = 0;
    bool command_overflow = false;
    bool serial_touch_down = false;
    int serial_touch_start_x = 0;
    int serial_touch_start_y = 0;
    int serial_touch_last_x = 0;
    int serial_touch_last_y = 0;
    int64_t serial_touch_start_us = 0;

    auto trim_command = [](char* line) {
        if (line == nullptr) return;
        size_t length = std::strlen(line);
        while (length > 0 && std::isspace(static_cast<unsigned char>(line[length - 1]))) {
            line[--length] = '\0';
        }
    };
    auto send_error = [&fd](const char* detail) {
        char response[160];
        const int size = std::snprintf(response, sizeof(response),
                                       "@@INPUT_ERROR detail=%s\n",
                                       detail != nullptr ? detail : "invalid");
        if (size > 0) (void)SerialWriteAll(fd, response, static_cast<size_t>(size));
    };
    auto send_touch_ack = [&fd](const char* action, int x, int y, int dx, bool changed,
                                bool busy) {
        char response[224];
        const int size = std::snprintf(
            response, sizeof(response),
            "@@INPUT_ACK kind=touch action=%s x=%d y=%d dx=%d changed=%d mode=0 busy=%d\n",
            action != nullptr ? action : "unknown", x, y, dx, changed ? 1 : 0,
            busy ? 1 : 0);
        if (size > 0) (void)SerialWriteAll(fd, response, static_cast<size_t>(size));
    };
    auto send_command_ack = [&fd](const char* action, bool changed = true) {
        char response[160];
        const int size = std::snprintf(
            response, sizeof(response),
            "@@INPUT_ACK kind=command action=%s changed=%d\n",
            action != nullptr ? action : "?", changed ? 1 : 0);
        if (size > 0) (void)SerialWriteAll(fd, response, static_cast<size_t>(size));
    };
    auto tap_is_actionable = [](int x, int y, bool power_save, bool test_mode, bool console_mode) {
        if (power_save || test_mode || console_mode) return true;
        if (y >= kTouchVirtualKeyMinY && y <= kTouchVirtualKeyMaxY) return true;
        // Product pages reserve the top 40 px for the status bar.  Treat all
        // other in-page taps as candidates and let HandleHomeTap apply the
        // page-specific hit map; this keeps serial touch injection in sync
        // with the physical touch task as new pages are added.
        return x >= kUiInset && x < kUiInset + kUiContentWidth && y >= 0 &&
               y < kPortraitH;
    };
    auto dispatch_cover_key = [this](int x, int y) {
        if (y < kTouchVirtualKeyMinY || y > kTouchVirtualKeyMaxY) return false;
        const int home_distance = std::abs(x - static_cast<int>(TOUCH_VK_HOME_X));
        const int next_distance = std::abs(x - static_cast<int>(TOUCH_VK_NEXT_X));
        const int prev_distance = std::abs(x - static_cast<int>(TOUCH_VK_PREV_X));
        if (home_distance <= kTouchVirtualKeyTolerance &&
            home_distance <= next_distance && home_distance <= prev_distance) {
            return HandleHardwareKey(HardwareKey::Home);
        }
        if (next_distance <= kTouchVirtualKeyTolerance && next_distance <= prev_distance) {
            return HandleHardwareKey(HardwareKey::Next);
        }
        if (prev_distance <= kTouchVirtualKeyTolerance) {
            return HandleHardwareKey(HardwareKey::Previous);
        }
        return false;
    };
    auto dispatch_injected_key = [this](const char* name) {
        if (name == nullptr) return false;
        if (strcasecmp(name, "HOME") == 0) return HandleHardwareKey(HardwareKey::Home);
        if (strcasecmp(name, "NEXT") == 0) return HandleHardwareKey(HardwareKey::Next);
        if (strcasecmp(name, "PREV") == 0 || strcasecmp(name, "PREVIOUS") == 0 ||
            strcasecmp(name, "BACK") == 0) {
            return HandleHardwareKey(HardwareKey::Previous);
        }
        if (strcasecmp(name, "SELECT") == 0) {
            return HandleHardwareKey(HardwareKey::Select);
        }
        return false;
    };
    auto inject_tap = [&](int x, int y, const char* action) {
        bool power_save = false;
        bool screen_test = false;
        bool console_mode = false;
        {
            DisplayLockGuard lock(this);
            power_save = power_save_;
            screen_test = screen_test_mode_;
            console_mode = test_console_mode_;
        }
        const bool actionable = tap_is_actionable(x, y, power_save, screen_test, console_mode);
        if (actionable) {
            if (!dispatch_cover_key(x, y)) HandleHomeTap(x, y);
        }
        send_touch_ack(action, x, y, 0, actionable, false);
    };
    auto release_serial_touch = [&](int x, int y) {
        if (!serial_touch_down) {
            send_error("touch_up_without_down");
            return;
        }
        serial_touch_last_x = x;
        serial_touch_last_y = y;
        const int dx = serial_touch_last_x - serial_touch_start_x;
        const int dy = serial_touch_last_y - serial_touch_start_y;
        const int64_t held_us = esp_timer_get_time() - serial_touch_start_us;
        const bool tap = held_us <= 800000 && std::abs(dx) <= 32 && std::abs(dy) <= 32;
        const bool pulled=HandleQuickPull(serial_touch_start_x,serial_touch_start_y,x,y,static_cast<int>(held_us/1000));
        if (tap && !pulled) {
            if (!dispatch_cover_key(x, y)) HandleHomeTap(x, y);
        }
        send_touch_ack("up", x, y, dx, tap || pulled, false);
        serial_touch_down = false;
        serial_touch_start_us = 0;
    };

    // Keep command RX and reply TX on the same buffered driver. No competing
    // raw FIFO reader is used once its ISR owns the hardware.
    flockfile(stdout);
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config{.tx_buffer_size=1024,.rx_buffer_size=1024};
        const auto result=usb_serial_jtag_driver_install(&config);
        if (result!=ESP_OK) {
            funlockfile(stdout);
            ESP_LOGE(TAG,"serial driver unavailable: %s",esp_err_to_name(result));
            return;
        }
    }
    usb_serial_jtag_vfs_use_driver();
    funlockfile(stdout);
    // The product reader owns the RX path from here on, so the early boot
    // diagnostic task stops reading and cannot split a command in two.
    boot_diag::SetDisplayReaderActive();
    ESP_LOGI(TAG, "serial input ready (FRAME?; TOUCH TAP/CLICK/DOWN/MOVE/UP)");
    bool secondary_open_failed = false;
    while (!frame_dump_stop_) {
        if (fd < 0 && !secondary_open_failed) {
            fd = ::open("/dev/secondary", O_WRONLY | O_NONBLOCK);
            if (fd < 0) {
                // Replies go through the driver, so an unavailable VFS node must
                // not gate command RX. Try once and keep serving commands.
                secondary_open_failed = true;
                ESP_LOGW(TAG, "secondary node unavailable (%s); replies still use the driver",
                         std::strerror(errno));
            }
        }

        char input[64];
        uint32_t received = 0;
        if (usb_serial_jtag_is_driver_installed()) {
            const int read_count = usb_serial_jtag_read_bytes(input, sizeof(input), 0);
            if (read_count > 0) received = static_cast<uint32_t>(read_count);
        } else if (usb_serial_jtag_ll_rxfifo_data_available()) {
            received = usb_serial_jtag_ll_read_rxfifo(
                reinterpret_cast<uint8_t*>(input), sizeof(input));
        }

        if (received > 0) {
            for (uint32_t i = 0; i < received; ++i) {
                const char c = input[i];
                if (c == '\r' || c == '\n') {
                    if (command_overflow) {
                        send_error("command_too_long");
                        command_overflow = false;
                        command_size = 0;
                        command[0] = '\0';
                        continue;
                    }
                    if (command_size == 0) continue;
                    command[command_size] = '\0';
                    trim_command(command);

                    if (strcasecmp(command, "FRAME?") == 0) {
                        DumpFrameToSerial(fd, false);
                    } else if (strcasecmp(command, "FRAME_PANEL?") == 0) {
                        DumpFrameToSerial(fd, true);
                    } else if (strcasecmp(command, "FONT?") == 0) {
                        char response[96];
                        const int size = std::snprintf(response, sizeof(response),
                            "@@FONT_ACK ready=%d glyphs=%lu\n", raw_font::Ready() ? 1 : 0,
                            static_cast<unsigned long>(raw_font::Count()));
                        if (size > 0) (void)SerialWriteAll(fd, response, static_cast<size_t>(size));
                    } else if (strcasecmp(command, "CAPSULE_STATE?") == 0) {
                        const auto state = xiaozhi::Conversation::GetInstance().Snapshot();
                        char response[192];
                        const int size = std::snprintf(response, sizeof(response),
                            "@@CAPSULE_ACK state=%u turn=%lu transcript_bytes=%u answer_bytes=%u saved=%d held=%d\n",
                            static_cast<unsigned>(state.state), static_cast<unsigned long>(state.turn),
                            static_cast<unsigned>(state.transcript.size()), static_cast<unsigned>(state.answer.size()),
                            state.saved ? 1 : 0, ai_key_down_.load() ? 1 : 0);
                        if (size > 0) (void)SerialWriteAll(fd, response, static_cast<size_t>(size));
                    } else if (strcasecmp(command, "SCREEN_TEST?") == 0) {
                        ShowScreenTestPattern();
                        send_command_ack("screen_test");
                    } else if (strcasecmp(command, "SCREEN_TEST_STEP?") == 0) {
                        UpdateStatusBar(true);
                        send_command_ack("screen_test_step");
                    } else if (strcasecmp(command, "GRAY4_TEST?") == 0) {
                        ShowGray4TestPattern();
                        send_command_ack("gray4_test");
                    } else if (strcasecmp(command, "WIPE_TEST?") == 0) {
                        ShowWipeTestPattern();
                        send_command_ack("wipe_test");
                    } else if (strcasecmp(command, "ANIM_TEST?") == 0) {
                        const bool started = StartAnimationTest(true);
                        send_command_ack(started ? "anim_start_du" : "anim_busy", started);
                    } else if (strcasecmp(command, "ANIM_TEST_FC?") == 0) {
                        const bool started = StartAnimationTest(false);
                        send_command_ack(started ? "anim_start_fc" : "anim_busy", started);
                    } else if (strcasecmp(command, "WAVEFRONT_TEST?") == 0) {
                        const bool started = StartWavefrontTest();
                        send_command_ack(started ? "wavefront_start" : "anim_busy", started);
                    } else if (strcasecmp(command, "PAPER_MONO_TEST?") == 0) {
                        const bool started = StartPaperMonoTest();
                        send_command_ack(started ? "paper_mono_start" : "anim_busy", started);
                    } else if (strcasecmp(command, "PAPER_TEXT_TEST?") == 0) {
                        const bool started = StartPaperMonoTextTest();
                        send_command_ack(started ? "paper_text_start" : "anim_busy", started);
                    } else if (strcasecmp(command, "PAPER_PAGE_TEST?") == 0 ||
                               strcasecmp(command, "PAGE_TURN_3L?") == 0) {
                        const bool started = StartPaperMonoPageTest();
                        send_command_ack(started ? "paper_page_start" : "anim_busy", started);
                    } else if (strcasecmp(command, "WIFI_CONFIG?") == 0 ||
                               strcasecmp(command, "WIFI_SETUP?") == 0) {
                        // The credential page is served from the device AP, so
                        // the Wi-Fi password never has to pass through a host
                        // command or a log line.
                        if (!GetHAL().IsWifiMode()) {
                            send_error("wifi_config_needs_wifi_mode");
                        } else {
                            // Settings only commits in its destructor, so the
                            // flag has to leave scope before the reboot.
                            {
                                Settings settings("wifi", true);
                                settings.SetInt("force_ap", 1);
                            }
                            send_command_ack("wifi_config_restart");
                            vTaskDelay(pdMS_TO_TICKS(300));
                            esp_restart();
                        }
                    } else if (strcasecmp(command, "BIND?") == 0 ||
                               strcasecmp(command, "ACTIVATION?") == 0) {
                        // The binding code is meant to be read off the panel, so
                        // reporting it to the paired host console is the same
                        // information the user already sees.
                        const xiaozhi::Activation::State state =
                            xiaozhi::Activation::GetInstance().Snapshot();
                        char response[256];
                        const int size = std::snprintf(
                            response, sizeof(response),
                            "@@BIND checked=%d code=%s bound=%d challenge=%d time=%d "
                            "message=%s\n",
                            state.checked ? 1 : 0, state.has_code ? state.code : "-",
                            state.bound ? 1 : 0, state.has_challenge ? 1 : 0,
                            state.has_server_time ? 1 : 0,
                            state.message[0] != '\0' ? state.message : "-");
                        if (size > 0) {
                            (void)SerialWriteAll(fd, response, static_cast<size_t>(size));
                        }
                    } else if (strcasecmp(command, "AI_TEXT?") == 0) {
                        // Read-only view of the AI card, so a conversation can be
                        // verified without photographing the panel.  It is the
                        // same text the dashboard already renders.
                        const dashboard::Snapshot snapshot =
                            dashboard::DashboardData::GetInstance().GetSnapshot();
                        char response[400];
                        const int size = std::snprintf(
                            response, sizeof(response),
                            "@@AI_TEXT status=%s count=%u\n@@AI_LINE 0 %s\n@@AI_LINE 1 %s\n"
                            "@@AI_LINE 2 %s\n",
                            snapshot.ai_status, static_cast<unsigned>(snapshot.ai_count),
                            snapshot.ai_summary[0], snapshot.ai_summary[1],
                            snapshot.ai_summary[2]);
                        if (size > 0) {
                            (void)SerialWriteAll(fd, response, static_cast<size_t>(size));
                        }
                    } else if (strcasecmp(command, "REMINDER_STATE?") == 0) {
                        const auto response = "@@REMINDER_STATE " +
                            reminders::Service::Instance().Status() + "\n";
                        (void)SerialWriteAll(fd, response.data(), response.size());
                    } else if (strncasecmp(command, "MCP ", 4) == 0) {
                        // USB-only diagnostics use the same parser/store as the
                        // server, with a separate retry cache and counters.
                        const auto reply = reminders::Service::Instance().HandleMcp(command + 4, 0, true);
                        const auto response = "@@MCP_REPLY " + (reply.empty() ? "null" : reply) + "\n";
                        (void)SerialWriteAll(fd, response.data(), response.size());
                    } else if (strcasecmp(command, "XIAOZHI_STATS?") == 0) {
                        xiaozhi::AudioSessionStats stats =
                            xiaozhi::AudioSession::GetInstance().Stats();
                        const auto& client = xiaozhi::Client::GetInstance();
                        stats.transport_connected = client.IsConnected();
                        stats.session_ready = client.IsSessionReady();
                        char response[768];
                        const int size = std::snprintf(
                            response, sizeof(response),
                            "@@XIAOZHI_STATS frames_sent=%lu send_err=%lu recv=%lu dropped=%lu "
                            "decoded=%lu enc_err=%lu dec_err=%lu in_frames=%lu in_fail=%lu "
                            "peak=%d up=%d down=%d out=%d frame_ms=%d cap=%d play=%d "
                            "enc=%d dec=%d gated=%lu opens=%lu params=%d ws=%d sess=%d "
                            "enabled=%d ver=%d "
                            "cap_stack=%lu play_stack=%lu heap=%lu psram=%lu reset=%s "
                            "play_tts_stack=%lu play_tts_n=%lu play_alloc_fail=%lu play_need=%lu largest=%lu\n",
                            static_cast<unsigned long>(stats.frames_sent),
                            static_cast<unsigned long>(stats.send_errors),
                            static_cast<unsigned long>(stats.packets_received),
                            static_cast<unsigned long>(stats.packets_dropped),
                            static_cast<unsigned long>(stats.packets_decoded),
                            static_cast<unsigned long>(stats.encode_errors),
                            static_cast<unsigned long>(stats.decode_errors),
                            static_cast<unsigned long>(stats.input_frames),
                            static_cast<unsigned long>(stats.input_failures), stats.input_peak,
                            stats.uplink_rate, stats.downlink_rate, stats.output_rate,
                            stats.frame_duration_ms,
                            stats.capturing ? 1 : 0, stats.playback_open ? 1 : 0,
                            stats.encoder_open ? 1 : 0, stats.decoder_open ? 1 : 0,
                            static_cast<unsigned long>(stats.frames_gated),
                            static_cast<unsigned long>(stats.gate_opens),
                            stats.params_seen ? 1 : 0, stats.transport_connected ? 1 : 0,
                            stats.session_ready ? 1 : 0, client.Enabled() ? 1 : 0,
                            stats.protocol_version,
                            static_cast<unsigned long>(stats.capture_stack_free),
                            static_cast<unsigned long>(stats.playback_stack_free),
                            static_cast<unsigned long>(stats.internal_heap_free),
                            static_cast<unsigned long>(stats.psram_free),
                            ResetReasonName(esp_reset_reason()),
                            static_cast<unsigned long>(stats.playback_tts_stack_free),
                            static_cast<unsigned long>(stats.playback_tts_stack_samples),
                            static_cast<unsigned long>(stats.playback_create_failures),
                            static_cast<unsigned long>(stats.playback_stack_bytes),
                            static_cast<unsigned long>(stats.internal_largest_free));
                        if (size > 0 && static_cast<size_t>(size) < sizeof(response)) {
                            (void)SerialWriteAll(fd, response, static_cast<size_t>(size));
                        }
                    } else if (strcasecmp(command, "RECORDER_STATE?") == 0) {
                        const auto state=xiaozhi::AudioSession::GetInstance().RecorderState();
                        char response[160];
                        const int size=std::snprintf(response,sizeof(response),
                            "@@RECORDER_STATE mode=%u seconds=%lu clip=%d saved=%d failed=%d revision=%lu\n",
                            static_cast<unsigned>(state.mode),static_cast<unsigned long>(state.seconds),
                            state.has_clip,state.saved,state.failed,static_cast<unsigned long>(state.revision));
                        if (size>0) (void)SerialWriteAll(fd,response,static_cast<size_t>(size));
                    } else if (strcasecmp(command, "XIAOZHI_AUDIO_TEST?") == 0) {
                        // Mic capture plus a 1 kHz speaker tone; blocks this task
                        // for a few seconds, so run it while the UI is idle.
                        std::string detail;
                        const bool ok =
                            xiaozhi::AudioSession::GetInstance().SelfTest(&detail);
                        char response[400];
                        const int size = std::snprintf(
                            response, sizeof(response),
                            "@@XIAOZHI_AUDIO_TEST_ACK ok=%d detail=%s\n", ok ? 1 : 0,
                            detail.c_str());
                        if (size > 0) {
                            (void)SerialWriteAll(fd, response, static_cast<size_t>(size));
                        }
                    } else if (strcasecmp(command, "XIAOZHI_DOWNLINK_TEST?") == 0) {
                        std::string detail;
                        const bool ok =
                            xiaozhi::AudioSession::GetInstance().DownlinkLoopTest(&detail);
                        char response[400];
                        const int size = std::snprintf(
                            response, sizeof(response),
                            "@@XIAOZHI_DOWNLINK_TEST_ACK ok=%d detail=%s\n", ok ? 1 : 0,
                            detail.c_str());
                        if (size > 0) {
                            (void)SerialWriteAll(fd, response, static_cast<size_t>(size));
                        }
                    } else if (strcasecmp(command, "TEST_VERSION_NEXT?") == 0) {
                        {
                            DisplayLockGuard lock(this);
                            test_variant_ = static_cast<uint8_t>((test_variant_ + 1) % kGrayVariantCount);
                            test_console_mode_ = true;
                            DrawTestConsoleLocked();
                            FlushLocked();
                        }
                        send_command_ack("test_version_next");
                    } else if (strcasecmp(command, "TEST_CONSOLE?") == 0) {
                        ShowHomeScreen();
                        send_command_ack("test_console");
                    } else if (strcasecmp(command, "HOME?") == 0) {
                        ShowProductHomeScreen();
                        send_command_ack("home");
                    } else if (strncasecmp(command, "KEY ", 4) == 0 ||
                               strncasecmp(command, "BUTTON ", 7) == 0) {
                        const size_t prefix = strncasecmp(command, "KEY ", 4) == 0 ? 4U : 7U;
                        char key_name[24] = {};
                        char action[24] = {};
                        const int parsed = std::sscanf(command + prefix, "%23s %23s",
                                                       key_name, action);
                        const bool ai_key = strcasecmp(key_name, "BOOT") == 0 || strcasecmp(key_name, "AI") == 0;
                        if (parsed < 1) {
                            send_error("key_args");
                        } else if (ai_key) {
                            if (strcasecmp(action, "DOWN") == 0 || strcasecmp(action, "PRESS") == 0) {
                                (void)HandleAiKey(true);
                                send_command_ack("ai_down");
                            } else if (strcasecmp(action, "UP") == 0 || strcasecmp(action, "RELEASE") == 0) {
                                (void)HandleAiKey(false);
                                send_command_ack("ai_up");
                            } else {
                                send_error("ai_requires_down_or_up");
                            }
                        } else if (parsed >= 2 && strcasecmp(action, "CLICK") != 0 && strcasecmp(action, "PRESS") != 0) {
                            send_error("key_args");
                        } else if (!dispatch_injected_key(key_name)) {
                            send_error("unsupported_key");
                        } else {
                            char ack_name[48];
                            std::snprintf(ack_name, sizeof(ack_name), "key_%s", key_name);
                            send_command_ack(ack_name);
                        }
                    } else if (strcasecmp(command, "STATE?") == 0) {
                        const char* page_name = "home";
                        bool screen_test = false;
                        bool power_save = false;
                        bool test_console = false;
                        {
                            DisplayLockGuard lock(this);
                            screen_test = screen_test_mode_;
                            power_save = power_save_;
                            test_console = test_console_mode_;
                            switch (product_page_) {
                                case ProductPage::Home: page_name = "home"; break;
                                case ProductPage::AiResult: page_name = "ai_result"; break;
                                case ProductPage::AiSteps: page_name = "ai_steps"; break;
                                case ProductPage::QuickNote: page_name = "quick_note"; break;
                                case ProductPage::Reader: page_name = "reader"; break;
                                case ProductPage::TodayList: page_name = "calendar"; break;
                                case ProductPage::CardBox: page_name = "card_box"; break;
                                case ProductPage::CardDetail: page_name = "card_detail"; break;
                                case ProductPage::Keep: page_name = "keep"; break;
                                case ProductPage::Apps: page_name = "apps"; break;
                                case ProductPage::Workbench: page_name = "workbench"; break;
                                case ProductPage::Settings: page_name = "settings"; break;
                                case ProductPage::Confirmation: page_name = "confirmation"; break;
                                case ProductPage::More: page_name = "more"; break;
                                case ProductPage::Alarm: page_name = "alarm"; break;
                                case ProductPage::Recorder: page_name = "recorder"; break;
                                case ProductPage::ChatList: page_name = "chat_history"; break;
                                case ProductPage::ChatDetail: page_name = "chat_detail"; break;
                                case ProductPage::Notes: page_name = "notes"; break;
                                case ProductPage::NoteDetail: page_name = "note_detail"; break;
                                case ProductPage::WifiList: page_name = "wifi_list"; break;
                                case ProductPage::WifiCredentials: page_name = "wifi_credentials"; break;
                                case ProductPage::TextEntry: page_name = "text_input"; break;
                                case ProductPage::NoteCompose: page_name = "note_compose"; break;
                            }
                        }
                        char response[160];
                        const int size = std::snprintf(
                            response, sizeof(response),
                            "@@STATE mode=0 page=%s test=%d console=%d power_save=%d busy=0\n",
                            page_name, screen_test ? 1 : 0, test_console ? 1 : 0,
                            power_save ? 1 : 0);
                        if (size > 0) (void)SerialWriteAll(fd, response, static_cast<size_t>(size));
                    } else if (strcasecmp(command, "TOUCH BACK") == 0) {
                        (void)HandleHardwareKey(HardwareKey::Previous);
                        send_touch_ack("back", -1, -1, 0, true, false);
                    } else if (strncasecmp(command, "TOUCH TAP ", 10) == 0 ||
                               strncasecmp(command, "TOUCH CLICK ", 12) == 0) {
                        int x = 0;
                        int y = 0;
                        char extra = '\0';
                        const size_t prefix = strncasecmp(command, "TOUCH TAP ", 10) == 0 ? 10U : 12U;
                        const int parsed = std::sscanf(command + prefix, "%d %d %c", &x, &y, &extra);
                        if (parsed != 2 || x < 0 || x >= kPortraitW || y < 0 ||
                            y > kTouchVirtualKeyMaxY) {
                            send_error("touch_tap_args");
                        } else {
                            inject_tap(x, y, prefix == 10U ? "tap" : "click");
                        }
                    } else if (strncasecmp(command, "TOUCH DOWN ", 11) == 0 ||
                               strncasecmp(command, "TOUCH MOVE ", 11) == 0 ||
                               strncasecmp(command, "TOUCH UP ", 9) == 0) {
                        const bool is_down = strncasecmp(command, "TOUCH DOWN ", 11) == 0;
                        const bool is_move = strncasecmp(command, "TOUCH MOVE ", 11) == 0;
                        const size_t prefix = is_down || is_move ? 11U : 9U;
                        int x = 0;
                        int y = 0;
                        char extra = '\0';
                        const int parsed = std::sscanf(command + prefix, "%d %d %c", &x, &y, &extra);
                        if (parsed != 2 || x < 0 || x >= kPortraitW || y < 0 ||
                            y > kTouchVirtualKeyMaxY) {
                            send_error("touch_state_args");
                        } else if (is_down) {
                            serial_touch_down = true;
                            serial_touch_start_x = serial_touch_last_x = x;
                            serial_touch_start_y = serial_touch_last_y = y;
                            serial_touch_start_us = esp_timer_get_time();
                            send_touch_ack("down", x, y, 0, false, false);
                        } else if (is_move) {
                            if (!serial_touch_down) {
                                send_error("touch_move_without_down");
                            } else {
                                serial_touch_last_x = x;
                                serial_touch_last_y = y;
                                send_touch_ack("move", x, y, x - serial_touch_start_x, false, false);
                            }
                        } else {
                            release_serial_touch(x, y);
                        }
                    } else if (strncasecmp(command, "TOUCH SWIPE ", 12) == 0) {
                        // The home page has no swipe navigation yet; preserve the
                        // protocol and acknowledge the command for host tools.
                        send_touch_ack("swipe", -1, -1, 0, false, false);
                    } else if (strcasecmp(command, "INPUT HELP?") == 0) {
                        static constexpr char kHelp[] =
                            "@@INPUT_HELP FRAME?/FRAME_PANEL? | TOUCH TAP/CLICK x y | "
                            "TOUCH DOWN/MOVE/UP x y | TOUCH BACK | SCREEN_TEST? | "
                            "ANIM_TEST? (DU 0x1C) | ANIM_TEST_FC? (0xFC) | "
                            "WAVEFRONT_TEST? | PAPER_MONO_TEST? | PAPER_TEXT_TEST? | "
                            "PAPER_PAGE_TEST? | XIAOZHI_STATS? | XIAOZHI_AUDIO_TEST? | "
                            "XIAOZHI_DOWNLINK_TEST? | AI_TEXT? | BIND? | WIFI_CONFIG? | "
                            "REMINDER_STATE? | MCP {json-rpc} | "
                            "HOME? | KEY/BUTTON HOME|PREV|NEXT|SELECT CLICK\n";
                        (void)SerialWriteAll(fd, kHelp, sizeof(kHelp) - 1U);
                    } else if (strncasecmp(command, "TOUCH", 5) == 0) {
                        send_error("unknown_input_command");
                    } else {
                        send_error("unknown_command");
                    }
                    command_size = 0;
                    command[0] = '\0';
                } else if (command_size + 1U < sizeof(command)) {
                    command[command_size++] = c;
                } else {
                    command_overflow = true;
                    command_size = 0;
                    command[0] = '\0';
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (fd >= 0) ::close(fd);
}

void RawDisplay::DumpFrameToSerial(int fd, bool panel_frame) {
    if (frame_dump_fb_ == nullptr || !Lock(1000)) {
        static constexpr char kBusy[] = "@@RAW_FRAME_ERROR busy\n";
        (void)SerialWriteAll(fd, kBusy, sizeof(kBusy) - 1U);
        return;
    }
    // Never export pixels from a password editor, including the revealed view.
    if (edit_target_==EditTarget::WifiPassword) {
        Unlock();
        static constexpr char hidden[]="@@RAW_FRAME_ERROR sensitive_input\n";
        (void)SerialWriteAll(fd,hidden,sizeof(hidden)-1);
        return;
    }
    const size_t bytes = panel_frame ? panel_size_ : portrait_size_;
    const int width = panel_frame ? kPanelW : kPortraitW;
    const int height = panel_frame ? kPanelH : kPortraitH;
    std::memcpy(frame_dump_fb_, panel_frame ? panel_fb_ : portrait_fb_, bytes);
    const uint32_t crc = FrameCrc32(frame_dump_fb_, bytes);
    const uint32_t sequence = ++frame_dump_sequence_;
    const bool history_valid = panel_history_valid_;
    const uint32_t fast_refresh_count = fast_refresh_count_;
    Unlock();

    char header[224];
    const int header_size = std::snprintf(
        header, sizeof(header),
        "@@RAW_FRAME_BEGIN kind=%s seq=%u w=%d h=%d stride=%d bytes=%u crc=%08x mode=0 history=%d fast=%u gc=0\n",
        panel_frame ? "panel" : "portrait", static_cast<unsigned>(sequence), width, height,
        width / 8, static_cast<unsigned>(bytes), static_cast<unsigned>(crc),
        history_valid ? 1 : 0, static_cast<unsigned>(fast_refresh_count));
    if (header_size <= 0 || !SerialWriteAll(fd, header, static_cast<size_t>(header_size))) return;

    static constexpr char kHex[] = "0123456789abcdef";
    for (size_t offset = 0; offset < bytes; offset += kFrameDumpChunkBytes) {
        const size_t chunk = std::min(kFrameDumpChunkBytes, bytes - offset);
        char line[kFrameDumpLineBytes];
        int line_size = std::snprintf(line, sizeof(line), "@@RAW_FRAME_DATA offset=%u data=",
                                       static_cast<unsigned>(offset));
        if (line_size <= 0 || static_cast<size_t>(line_size) + chunk * 2U + 2U >= sizeof(line)) return;
        for (size_t i = 0; i < chunk; ++i) {
            const uint8_t value = frame_dump_fb_[offset + i];
            line[line_size++] = kHex[value >> 4];
            line[line_size++] = kHex[value & 0x0f];
        }
        line[line_size++] = '\n';
        if (!SerialWriteAll(fd, line, static_cast<size_t>(line_size))) return;
    }
    static constexpr char kEnd[] = "@@RAW_FRAME_END\n";
    (void)SerialWriteAll(fd, kEnd, sizeof(kEnd) - 1U);
}

void RawDisplay::FillRect(int x, int y, int w, int h, bool black) {
    if (w <= 0 || h <= 0) return;
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(kPortraitW, x + w);
    const int y1 = std::min(kPortraitH, y + h);
    for (int yy = y0; yy < y1; ++yy) {
        for (int xx = x0; xx < x1; ++xx) SetPixel(xx, yy, black);
    }
}

void RawDisplay::StrokeRect(int x, int y, int w, int h, int thickness) {
    if (w <= 0 || h <= 0 || thickness <= 0) return;
    FillRect(x, y, w, thickness, true);
    FillRect(x, y + h - thickness, w, thickness, true);
    FillRect(x, y, thickness, h, true);
    FillRect(x + w - thickness, y, thickness, h, true);
}

void RawDisplay::FillRoundRect(int x, int y, int w, int h, int radius, bool black) {
    if (w <= 0 || h <= 0) return;
    radius = std::max(0, std::min(radius, std::min(w, h) / 2));
    for (int yy = 0; yy < h; ++yy) {
        int inset = 0;
        if (radius > 0 && (yy < radius || yy >= h - radius)) {
            const int edge = yy < radius ? radius - 1 - yy : yy - (h - radius);
            const int inside = radius * radius - edge * edge;
            const int span = static_cast<int>(std::sqrt(static_cast<double>(std::max(0, inside))));
            inset = radius - span;
        }
        FillRect(x + inset, y + yy, w - inset * 2, 1, black);
    }
}

void RawDisplay::StrokeRoundRect(int x, int y, int w, int h, int radius, int thickness) {
    if (w <= 0 || h <= 0 || thickness <= 0) return;
    FillRoundRect(x, y, w, h, radius, true);
    if (w > thickness * 2 && h > thickness * 2) {
        FillRoundRect(x + thickness, y + thickness, w - thickness * 2,
                      h - thickness * 2, std::max(0, radius - thickness), false);
    }
}

void RawDisplay::FillCircle(int cx, int cy, int radius, bool black) {
    if (radius <= 0) return;
    const int r2 = radius * radius;
    for (int yy = -radius; yy <= radius; ++yy) {
        for (int xx = -radius; xx <= radius; ++xx) {
            if (xx * xx + yy * yy <= r2) SetPixel(cx + xx, cy + yy, black);
        }
    }
}

void RawDisplay::StrokeCircle(int cx, int cy, int radius, int thickness) {
    if (radius <= 0 || thickness <= 0) return;
    const int outer = radius * radius;
    const int inner_radius = std::max(0, radius - thickness);
    const int inner = inner_radius * inner_radius;
    for (int yy = -radius; yy <= radius; ++yy) {
        for (int xx = -radius; xx <= radius; ++xx) {
            const int d2 = xx * xx + yy * yy;
            if (d2 <= outer && d2 >= inner) SetPixel(cx + xx, cy + yy, true);
        }
    }
}

void RawDisplay::DrawTextInk(int x, int y, const char* text, const ui_font_t& font, bool black) {
    if (!text) return;
    int cursor = x;
    while (*text) {
        const auto glyph = raw_font::Lookup(font, Utf8Next(&text));
        for (int yy = 0; yy < glyph.height; ++yy) {
            for (int xx = 0; xx < glyph.width; ++xx) {
                if (raw_font::Pixel(glyph, xx, yy)) SetPixel(cursor + glyph.x + xx, y + glyph.y + yy, black);
            }
        }
        cursor += glyph.advance;
    }
}

void RawDisplay::DrawText(int x, int y, const char* text, const ui_font_t& font) {
    DrawTextInk(x, y, text, font, true);
}

void RawDisplay::SetPaperMonoClassPixelLocked(int x, int y, uint8_t level) {
    if (panel_fb_ == nullptr || panel_prev_fb_ == nullptr || level == 0) {
        return;
    }
    if (paper_page_draw_origin_x_ >= 0) {
        x += paper_page_draw_origin_x_;
        if (x < paper_page_draw_origin_x_ ||
            x >= paper_page_draw_origin_x_ + kPaperPageWidth ||
            y < kPaperPageTop || y >= kPaperPageBottom) return;
    }
    if (x < 0 || x >= kPortraitW || y < 0 || y >= kPortraitH) return;

    // The raw UI is portrait (480x800), while SSD1677 RAM is landscape
    // (800x480). Keep the same fixed 270-degree mapping as FlushLocked(), but
    // write the Paper Mono selector masks directly instead of collapsing the
    // raster to a 1-bit portrait framebuffer first.
    const int sx = y;
    const int sy = kPortraitW - 1 - x;
    const size_t offset = static_cast<size_t>(sy) * (kPanelW / 8) + (sx >> 3);
    const uint8_t mask = static_cast<uint8_t>(0x80u >> (sx & 7));
    panel_fb_[offset] |= mask;  // q24: every non-white class
    if (level >= 2) panel_prev_fb_[offset] |= mask;  // q26: black endpoint
}

void RawDisplay::FillPaperMonoClassRectLocked(int x, int y, int w, int h, uint8_t level) {
    if (level == 0 || w <= 0 || h <= 0) return;
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(kPortraitW, x + w);
    const int y1 = std::min(kPortraitH, y + h);
    for (int yy = y0; yy < y1; ++yy) {
        for (int xx = x0; xx < x1; ++xx) {
            SetPaperMonoClassPixelLocked(xx, yy, level);
        }
    }
}

void RawDisplay::DrawPaperMonoTextLocked(int x, int y, const char* text,
                                         const ui_font_t& font, uint8_t ink_level) {
    if (text == nullptr || text[0] == '\0' || font.bitmap == nullptr ||
        font.glyphs == nullptr || font.codepoints == nullptr || ink_level == 0) {
        return;
    }

    // ai_ui_assets.c is a checked-in, rasterized 2bpp font.  Quantize its
    // coverage values directly to the three physical classes: 0=white,
    // 1=Paper Mono gray, 2=black.  No ordered/error dithering is introduced;
    // the edge pixels remain real raster coverage from the source glyph.
    const bool strong_edges = (test_variant_ & 1U) != 0U;
    int cursor = x;
    const char* p = text;
    while (*p != '\0') {
        const uint32_t codepoint = Utf8Next(&p);
        const ui_glyph_t* glyph = FindGlyph(font, codepoint);
        if (glyph == nullptr) {
            cursor += std::max(1, static_cast<int>(font.height / 2));
            continue;
        }

        const int glyph_stride = font.bits_per_pixel == 1
                                     ? (static_cast<int>(glyph->width) + 7) / 8
                                     : (static_cast<int>(glyph->width) + 3) / 4;
        for (int yy = 0; yy < font.height; ++yy) {
            const uint8_t* row = font.bitmap + glyph->offset + yy * glyph_stride;
            for (int xx = 0; xx < glyph->width; ++xx) {
                uint8_t coverage = 0;
                if (font.bits_per_pixel == 1) {
                    coverage = static_cast<uint8_t>((row[xx >> 3] >> (7 - (xx & 7))) & 0x01U);
                    coverage = coverage != 0 ? 3 : 0;
                } else if (font.bits_per_pixel == 2) {
                    const uint8_t packed = row[xx >> 2];
                    coverage = static_cast<uint8_t>((packed >> (6 - 2 * (xx & 3))) & 0x03U);
                } else {
                    // The embedded raw UI set is intentionally 2bpp. Keep a
                    // future 4bpp font from silently becoming black text.
                    continue;
                }
                if (coverage == 0) continue;

                uint8_t level = 0;
                if (ink_level >= 2) {
                    // Black ink: full coverage is black; the antialiased
                    // fringe is the Paper Mono middle tone. Variant B uses a
                    // stronger threshold for a crisper small-font sample.
                    const uint8_t black_threshold = strong_edges ? 2 : 3;
                    level = coverage >= black_threshold ? 2 : 1;
                } else {
                    // Gray ink: only the denser part of a glyph receives the
                    // middle tone, leaving the fringe on the white class.
                    level = coverage >= 2 ? 1 : 0;
                }
                if (level != 0) SetPaperMonoClassPixelLocked(cursor + xx, y + yy, level);
            }
        }
        cursor += static_cast<int>(glyph->width) + 1;
    }
}

int RawDisplay::TextWidth(const char* text, const ui_font_t& font) const {
    if (text == nullptr || text[0] == '\0') return 0;
    int width = 0;
    const char* p = text;
    while (*p != '\0') {
        const uint32_t codepoint = Utf8Next(&p);
        width += raw_font::Lookup(font, codepoint).advance;
    }
    return std::max(0, width - 1);
}

void RawDisplay::FitText(const char* text, const ui_font_t& font, int max_width,
                         char* out, size_t out_size) const {
    if (out == nullptr || out_size == 0) return;
    out[0] = '\0';
    if (text == nullptr || max_width <= 0) return;
    const char* p = text;
    size_t written = 0;
    int width = 0;
    bool truncated = false;
    while (*p != '\0') {
        const char* start = p;
        const uint32_t codepoint = Utf8Next(&p);
        const int advance = raw_font::Lookup(font, codepoint).advance;
        const size_t bytes = static_cast<size_t>(p - start);
        if (width + advance - 1 > max_width || written + bytes >= out_size) {
            truncated = true;
            break;
        }
        std::memcpy(out + written, start, bytes);
        written += bytes;
        width += advance;
    }
    out[written] = '\0';
    if (!truncated) return;
    constexpr const char* ellipsis = "…";
    const size_t ellipsis_bytes = std::strlen(ellipsis);
    const int ellipsis_width = TextWidth(ellipsis, font);
    if (ellipsis_width > max_width || ellipsis_bytes >= out_size) return;
    while (written > 0 && (TextWidth(out, font) + 1 + ellipsis_width > max_width ||
                           written + ellipsis_bytes >= out_size)) {
        do { --written; } while (written > 0 && (static_cast<uint8_t>(out[written]) & 0xC0) == 0x80);
        out[written] = '\0';
    }
    std::memcpy(out + written, ellipsis, ellipsis_bytes + 1);
}

void RawDisplay::FitTextLines(const char* text, const ui_font_t& font, int max_width,
                              char* line1, size_t line1_size, char* line2,
                              size_t line2_size) const {
    if (line1 == nullptr || line2 == nullptr || line1_size == 0 || line2_size == 0) return;
    line1[0] = '\0';
    line2[0] = '\0';
    if (text == nullptr || text[0] == '\0' || max_width <= 0) return;

    struct Unit {
        const char* start;
        size_t bytes;
        int advance;
        uint32_t codepoint;
    };
    // Dashboard fields are bounded to a few dozen bytes.  Keeping the units
    // on the task stack makes wrapping deterministic and avoids heap churn.
    Unit units[96]{};
    size_t unit_count = 0;
    int total_width = 0;
    const char* cursor = text;
    while (*cursor != '\0' && unit_count < sizeof(units) / sizeof(units[0])) {
        const char* start = cursor;
        const uint32_t codepoint = Utf8Next(&cursor);
        const int advance = raw_font::Lookup(font, codepoint).advance;
        units[unit_count++] = {start, static_cast<size_t>(cursor - start), advance, codepoint};
        total_width += advance;
    }
    if (unit_count == 0) return;

    auto append_unit = [](char* destination, size_t destination_size, size_t* written,
                          const Unit& unit) {
        if (destination == nullptr || written == nullptr || *written >= destination_size) return;
        const size_t room = destination_size - *written - 1U;
        const size_t bytes = std::min(room, unit.bytes);
        if (bytes > 0) {
            std::memcpy(destination + *written, unit.start, bytes);
            *written += bytes;
            destination[*written] = '\0';
        }
    };
    auto append_ellipsis = [&](char* destination, size_t destination_size, size_t* written) {
        static constexpr char kEllipsis[] = "…";
        if (destination == nullptr || written == nullptr || *written >= destination_size) return;
        const size_t ellipsis_bytes = sizeof(kEllipsis) - 1U;
        if (*written + ellipsis_bytes + 1U < destination_size) {
            std::memcpy(destination + *written, kEllipsis, ellipsis_bytes);
            *written += ellipsis_bytes;
            destination[*written] = '\0';
        }
    };

    // A short value stays on one line.  For a longer value, choose a split
    // near half the measured width instead of filling the first line and
    // leaving a visibly short orphan on the second line.
    if (total_width <= max_width) {
        size_t written = 0;
        for (size_t i = 0; i < unit_count; ++i) append_unit(line1, line1_size, &written, units[i]);
        return;
    }

    const int target_width = std::min(max_width, std::max(1, total_width / 2));
    size_t split = 0;
    int first_width = 0;
    for (size_t i = 0; i < unit_count; ++i) {
        if (first_width + units[i].advance > target_width) break;
        first_width += units[i].advance;
        split = i + 1;
    }
    if (split == 0) split = 1;

    // Chinese summaries have no spaces to guide wrapping.  If the measured
    // midpoint would leave the last character of a two-character word at the
    // end of the first line, prefer a nearby even boundary when it keeps both
    // lines inside the box.  This avoids awkward breaks such as “准 / 备”.
    size_t cjk_units = 0;
    for (size_t i = 0; i < unit_count; ++i) {
        if (units[i].codepoint >= 0x4e00U && units[i].codepoint <= 0x9fffU) ++cjk_units;
    }
    if ((cjk_units * 3U >= unit_count * 2U) && (split & 1U) != 0U) {
        size_t balanced_split = split;
        int balanced_score = INT_MAX;
        for (int direction : {-1, 1}) {
            const int candidate_int = static_cast<int>(split) + direction;
            if (candidate_int <= 0 || candidate_int >= static_cast<int>(unit_count) ||
                (candidate_int & 1) != 0) {
                continue;
            }
            int candidate_first_width = 0;
            for (int i = 0; i < candidate_int; ++i) {
                candidate_first_width += units[static_cast<size_t>(i)].advance;
            }
            if (candidate_first_width > max_width) continue;
            const int candidate_second_width = total_width - candidate_first_width;
            const int visible_second_width = std::min(max_width, candidate_second_width);
            const int score = std::abs(candidate_first_width - visible_second_width);
            if (score <= balanced_score) {
                balanced_score = score;
                balanced_split = static_cast<size_t>(candidate_int);
                first_width = candidate_first_width;
            }
        }
        split = balanced_split;
    }
    while (split > 1 && first_width > max_width) {
        --split;
        first_width -= units[split].advance;
    }

    size_t written1 = 0;
    for (size_t i = 0; i < split; ++i) append_unit(line1, line1_size, &written1, units[i]);

    size_t written2 = 0;
    int second_width = 0;
    int remaining_width = 0;
    for (size_t i = split; i < unit_count; ++i) remaining_width += units[i].advance;
    const int ellipsis_width = TextWidth("…", font);
    const bool needs_ellipsis = remaining_width > max_width;
    const int second_limit = needs_ellipsis
                                 ? std::max(1, max_width - std::max(0, ellipsis_width))
                                 : max_width;
    bool truncated = false;
    for (size_t i = split; i < unit_count; ++i) {
        if (second_width + units[i].advance > second_limit) {
            truncated = true;
            break;
        }
        append_unit(line2, line2_size, &written2, units[i]);
        second_width += units[i].advance;
    }
    if (truncated || needs_ellipsis) append_ellipsis(line2, line2_size, &written2);
}

void RawDisplay::DrawTextCentered(int x, int y, int width, int height, const char* text,
                                  const ui_font_t& font) {
    if (text == nullptr || text[0] == '\0' || width <= 0 || height <= 0) return;
    const int text_width = TextWidth(text, font);
    const int draw_x = x + std::max(0, (width - text_width) / 2);
    const int draw_y = y + std::max(0, (height - static_cast<int>(font.height)) / 2);
    DrawText(draw_x, draw_y, text, font);
}

void RawDisplay::DrawTextLinesCentered(int x, int y, int width, int height,
                                       const char* line1, const char* line2,
                                       const ui_font_t& font) {
    if (line1 == nullptr || line1[0] == '\0') return;
    const bool has_second = line2 != nullptr && line2[0] != '\0';
    constexpr int kLineGap = 4;
    const int line_height = static_cast<int>(font.height);
    const int block_height = has_second ? line_height * 2 + kLineGap : line_height;
    int draw_y = y + std::max(0, (height - block_height) / 2);
    DrawTextCentered(x, draw_y, width, line_height, line1, font);
    if (has_second) {
        draw_y += line_height + kLineGap;
        DrawTextCentered(x, draw_y, width, line_height, line2, font);
    }
}

void RawDisplay::DrawCardIcon(int card, int x, int y) {
    // Icons are deliberately geometric so the UI stays crisp at 1-bit output.
    switch (card) {
        case 0:  // AI sparkle
            FillCircle(x + 14, y + 16, 5, true);
            FillCircle(x + 34, y + 30, 4, true);
            StrokeCircle(x + 25, y + 24, 21, 3);
            FillRect(x + 22, y, 6, 10, true);
            FillRect(x + 22, y + 38, 6, 10, true);
            break;
        case 1:  // schedule
            StrokeRoundRect(x + 2, y + 5, 44, 40, 5, 3);
            FillRect(x + 2, y + 14, 44, 4, true);
            FillRect(x + 12, y, 4, 11, true);
            FillRect(x + 32, y, 4, 11, true);
            FillRect(x + 12, y + 25, 7, 5, true);
            FillRect(x + 24, y + 25, 7, 5, true);
            break;
        case 2:  // weather
            FillCircle(x + 20, y + 27, 13, true);
            FillCircle(x + 32, y + 22, 12, true);
            FillRect(x + 12, y + 27, 29, 13, true);
            for (int i = 0; i < 3; ++i) {
                FillRect(x + 10 + i * 13, y + 43, 3, 7, true);
            }
            break;
        case 3:  // quota gauge
            StrokeCircle(x + 25, y + 25, 22, 4);
            FillRect(x + 25, y + 5, 5, 22, true);
            FillRect(x + 25, y + 25, 17, 5, true);
            break;
        default:  // custom card
            StrokeRoundRect(x + 2, y + 4, 44, 42, 5, 3);
            FillRect(x + 11, y + 14, 8, 8, true);
            FillRect(x + 25, y + 14, 8, 8, true);
            FillRect(x + 11, y + 29, 22, 5, true);
            break;
    }
}

void RawDisplay::DrawBattery(int x, int y, int percent, bool charging) {
    const int clamped = std::clamp(percent, 0, 100);
    StrokeRoundRect(x, y, 44, 22, 4, 3);
    FillRect(x + 44, y + 7, 4, 8, true);
    const int fill = (40 * clamped) / 100;
    if (fill > 0) FillRoundRect(x + 3, y + 3, fill, 16, 2, true);
    if (charging) {
        FillRect(x + 19, y - 7, 5, 9, true);
        FillRect(x + 15, y - 3, 13, 5, true);
    }
}

void RawDisplay::DrawDigit(int x, int y, int scale, int digit) {
    if (digit < 0 || digit > 9) return;
    const uint8_t s = kSegments[digit];
    auto bar = [&](int bx, int by, int bw, int bh) {
        for (int yy = 0; yy < bh * scale; ++yy)
            for (int xx = 0; xx < bw * scale; ++xx) SetPixel(x + (bx + xx / scale) * scale, y + (by + yy / scale) * scale, true);
    };
    if (s & 1) bar(2, 0, 5, 1);
    if (s & 2) bar(7, 1, 1, 5);
    if (s & 4) bar(7, 7, 1, 5);
    if (s & 8) bar(2, 12, 5, 1);
    if (s & 16) bar(1, 7, 1, 5);
    if (s & 32) bar(1, 1, 1, 5);
    if (s & 64) bar(2, 6, 5, 1);
}

void RawDisplay::DrawPercent(int x, int y, int scale, int value) {
    char buf[5]; std::snprintf(buf, sizeof(buf), "%d", std::clamp(value, 0, 100));
    int cursor = x;
    for (const char* p = buf; *p; ++p) { DrawDigit(cursor, y, scale, *p - '0'); cursor += 10 * scale; }
    for (int yy = 0; yy < scale; ++yy) for (int xx = 0; xx < scale; ++xx) SetPixel(cursor + xx, y + yy, true);
}

void RawDisplay::DrawTestPatternLocked() {
    std::memset(portrait_fb_, kWhite, portrait_size_);

    auto hline = [&](int y, int x0, int x1, int thickness = 1) {
        for (int yy = y; yy < y + thickness; ++yy)
            for (int x = x0; x <= x1; ++x) SetPixel(x, yy, true);
    };
    auto vline = [&](int x, int y0, int y1, int thickness = 1) {
        for (int xx = x; xx < x + thickness; ++xx)
            for (int y = y0; y <= y1; ++y) SetPixel(xx, y, true);
    };

    // Border and 40-pixel calibration grid expose clipping, rotation and
    // row/column addressing errors without relying on any UI framework.
    hline(0, 0, kPortraitW - 1, 4);
    hline(kPortraitH - 4, 0, kPortraitW - 1, 4);
    vline(0, 0, kPortraitH - 1, 4);
    vline(kPortraitW - 4, 0, kPortraitH - 1, 4);
    for (int x = 40; x < kPortraitW - 1; x += 40) vline(x, 4, kPortraitH - 5);
    for (int y = 40; y < kPortraitH - 1; y += 40) hline(y, 4, kPortraitW - 5);

    // Center cross and nested boxes check aspect ratio and byte alignment.
    const int cx = kPortraitW / 2;
    const int cy = kPortraitH / 2;
    hline(cy - 1, 4, kPortraitW - 5, 3);
    vline(cx - 1, 4, kPortraitH - 5, 3);
    hline(cy - 48, cx - 48, cx + 48, 2);
    hline(cy + 46, cx - 48, cx + 48, 2);
    vline(cx - 48, cy - 48, cy + 48, 2);
    vline(cx + 46, cy - 48, cy + 48, 2);

    // Dither blocks provide a simple black/white transition test.
    for (int y = 100; y < 220; ++y) {
        for (int x = 40; x < 160; ++x) {
            if (((x / 8) + (y / 8)) & 1) SetPixel(x, y, true);
        }
    }
    for (int y = 100; y < 220; ++y) {
        for (int x = kPortraitW - 160; x < kPortraitW - 40; ++x) {
            if ((x / 4) & 1) SetPixel(x, y, true);
        }
    }

    // Corner numbers make the panel orientation immediately apparent.
    DrawDigit(16, 16, 4, 1);
    DrawDigit(kPortraitW - 48, 16, 4, 2);
    DrawDigit(16, kPortraitH - 76, 4, 3);
    DrawDigit(kPortraitW - 48, kPortraitH - 76, 4, 4);
}

void RawDisplay::DrawTestPatternVariantLocked(uint32_t step) {
    DrawTestPatternLocked();
    // Move a solid 48x48 block through the center test window.  The block is
    // deliberately confined to the same region used by the partial refresh.
    const int x = 190 + static_cast<int>((step % 4) * 48);
    const int y = 350;
    for (int yy = y; yy < y + 48; ++yy)
        for (int xx = x; xx < x + 48; ++xx) SetPixel(xx, yy, true);
}

void RawDisplay::ShowScreenTestPattern() {
    DisplayLockGuard lock(this);
    if (!portrait_fb_) return;
    screen_test_mode_ = true;
    screen_test_tick_ = 0;
    DrawTestPatternLocked();
    FlushLocked();
}

void RawDisplay::DrawTestConsoleLocked() {
    std::memset(portrait_fb_, kWhite, portrait_size_);
    DrawText(32, 34, "SSD1677 TEST CONSOLE", ui_font_title);
    char version[48];
    const int wipe = kWipeStripWidths[test_variant_ % kWipeStripWidthCount];
    std::snprintf(version, sizeof(version), "VERSION %u  WIPE %dPX",
                  static_cast<unsigned>(test_variant_), wipe);
    DrawText(34, 92, version, ui_font_status);

    const char* labels[] = {"GRAY4 TEST", "PAPER BANDS 3L", "PAPER TEXT 3L",
                            "ANIM DU 0x1C", "ANIM FC 0xFC", "PAGE TURN 3L"};
    for (int i = 0; i < kTestButtonCount; ++i) {
        const int y = kTestButtonY[i];
        StrokeRoundRect(kTestButtonX, y, kTestButtonW, kTestButtonH, 10, 3);
        const int width = TextWidth(labels[i], ui_font_title);
        DrawText(kTestButtonX + (kTestButtonW - width) / 2, y + 34, labels[i], ui_font_title);
    }
    DrawText(32, 748, "PAGE TURN 3L: masked white clear", ui_font_small);
    char test_hint[64];
    FitText("NEXT / 切换测试版本", ui_font_small, kPortraitW - 64, test_hint,
            sizeof(test_hint));
    DrawText(32, 772, test_hint, ui_font_small);
}

void RawDisplay::ShowHomeScreen() {
    DisplayLockGuard lock(this);
    if (!portrait_fb_) return;
    screen_test_mode_ = false;
    test_console_mode_ = true;
    DrawTestConsoleLocked();
    FlushLocked();
}

void RawDisplay::ShowProductHomeScreen() {
    DisplayLockGuard lock(this);
    if (!portrait_fb_) return;
    screen_test_mode_ = false;
    test_console_mode_ = false;
    product_page_ = ProductPage::Home;
    navigation_index_ = 0;
    quick_note_state_ = 0;
    book_list_page_ = 0;
    bool discharging = false;
    Board::GetInstance().GetBatteryLevel(battery_percent_, charging_, discharging);
    last_dashboard_revision_ = 0;
    DrawHomeScreenLocked();
    FlushLocked();
}

void RawDisplay::DrawLegacyDashboardScreenLocked() {
    std::memset(portrait_fb_, kWhite, portrait_size_);
    const dashboard::Snapshot snapshot = dashboard::DashboardData::GetInstance().GetSnapshot();
    last_dashboard_revision_ = snapshot.revision;

    // Power-save keeps a deliberately sparse image.  It is still rendered by
    // the same raw framebuffer path, so waking the device never needs LVGL or
    // a second display backend.
    if (power_save_) {
        const char* text = "休眠中";
        DrawText((kPortraitW - TextWidth(text, ui_font_title)) / 2, 350, text, ui_font_title);
        return;
    }

    time_t now = time(nullptr);
    struct tm tmv{};
    localtime_r(&now, &tmv);
    char clock_text[8];
    std::snprintf(clock_text, sizeof(clock_text), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    const char* weekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
    char weekday_text[8];
    std::snprintf(weekday_text, sizeof(weekday_text), "周%s", weekdays[std::clamp(tmv.tm_wday, 0, 6)]);
    char date_text[16];
    std::snprintf(date_text, sizeof(date_text), "%02d.%02d", tmv.tm_mon + 1, tmv.tm_mday);
    const int64_t now_ms = esp_timer_get_time() / 1000;
    const bool notification_active = notification_text_[0] != '\0' &&
                                     notification_deadline_ms_ > now_ms;

    // Header: a large clock anchors the page while the date block stays in a
    // narrow, deliberately quiet column.  The tiny index makes this read as
    // a daily sheet rather than another app dashboard.
    DrawText(32, 14, "今日 / 01", ui_font_small);
    DrawText(32, 35, clock_text, ui_font_clock);
    FillRect(304, 36, 1, 82, true);
    DrawTextCentered(320, 38, 120, 42, weekday_text, ui_font_title);
    DrawTextCentered(320, 85, 120, 32, date_text, ui_font_status);
    char network_text[32];
    CopyDisplayText(network_text, sizeof(network_text),
                    status_text_[0] != '\0' ? status_text_ : snapshot.network);
    char network_fit[24];
    FitText(network_text, ui_font_small, 184, network_fit, sizeof(network_fit));
    if (notification_active) {
        // A notification temporarily occupies the metadata rail, leaving the
        // large clock and date untouched.
        StrokeRoundRect(kMargin, 120, kContentWidth, 26, 6, 1);
        char notice[96];
        FitText(notification_text_, ui_font_small, kContentWidth - 24, notice, sizeof(notice));
        const int notice_width = TextWidth(notice, ui_font_small);
        DrawText(kMargin + std::max(12, (kContentWidth - notice_width) / 2),
                 121, notice, ui_font_small);
    } else {
        DrawText(32, 122, network_fit, ui_font_small);
        DrawBattery(400, 122, battery_percent_, charging_);
    }
    FillRect(kMargin, kHeaderRuleY, kContentWidth, 1, true);

    // Weather strip: four balanced columns and one quiet metadata line.  The
    // rules give it a clear reading boundary without making the page feel like
    // a stack of rounded cards when a provider has no forecast yet.
    FillRect(kMargin, kWeatherY, kContentWidth, 1, true);
    const int weather_center_y = kWeatherY + 30;
    StrokeCircle(62, weather_center_y, 9, 2);
    FillRect(62, weather_center_y - 15, 2, 8, true);
    FillRect(62, weather_center_y + 7, 2, 8, true);
    FillRect(47, weather_center_y, 8, 2, true);
    FillRect(71, weather_center_y, 8, 2, true);
    char weather_location[32];
    FitText(snapshot.weather.location, ui_font_body, 80, weather_location, sizeof(weather_location));
    DrawTextCentered(82, kWeatherY + 12, 78, 38, weather_location, ui_font_body);
    char temperature[24];
    if (snapshot.weather.valid) {
        std::snprintf(temperature, sizeof(temperature), "%d°", snapshot.weather.temperature_c);
    } else {
        std::snprintf(temperature, sizeof(temperature), "--°");
    }
    DrawTextCentered(166, kWeatherY + 10, 94, 42, temperature, ui_font_title);
    char weather_condition[48];
    CopyDisplayText(weather_condition, sizeof(weather_condition),
                    snapshot.weather.valid ? snapshot.weather.condition : "等待更新");
    char weather_condition_fit[48];
    FitText(weather_condition, ui_font_status, 142, weather_condition_fit,
            sizeof(weather_condition_fit));
    DrawTextCentered(274, kWeatherY + 16, 154, 34, weather_condition_fit, ui_font_status);
    char weather_detail[72];
    if (snapshot.weather.valid) {
        std::snprintf(weather_detail, sizeof(weather_detail), "体感 %d°  ·  湿度 %u%%",
                      snapshot.weather.feels_like_c, snapshot.weather.humidity);
    } else {
        CopyDisplayText(weather_detail, sizeof(weather_detail), "天气数据待更新");
    }
    char weather_detail_fit[72];
    FitText(weather_detail, ui_font_small, kContentWidth - 24, weather_detail_fit,
            sizeof(weather_detail_fit));
    DrawTextCentered(kMargin + 12, kWeatherY + 58, kContentWidth - 24, 22,
                     weather_detail_fit, ui_font_small);
    FillRect(kMargin, kWeatherRuleY, kContentWidth, 1, true);

    // AI focus section: a filled label and a large two-line summary create a
    // clear focal point without adding color or decorative noise.
    FillRect(kMargin, kHeroY, kContentWidth, 1, true);
    DrawText(48, kHeroY + 10, "01", ui_font_small);
    FillRect(48, kHeroY + 32, 100, 48, true);
    const int focus_label_width = TextWidth("AI 重点", ui_font_status);
    DrawTextInk(48 + std::max(0, (100 - focus_label_width) / 2), kHeroY + 42,
                "AI 重点", ui_font_status, false);
    FillRect(168, kHeroY + 16, 1, kHeroH - 32, true);
    const char* focus_source = snapshot.ai_count > 0 && snapshot.ai_summary[0][0] != '\0'
                                   ? snapshot.ai_summary[0]
                                   : "暂无重点";
    char focus_line1[96];
    char focus_line2[96];
    FitTextLines(focus_source, ui_font_body, 236, focus_line1, sizeof(focus_line1),
                 focus_line2, sizeof(focus_line2));
    const bool focus_has_second_line = focus_line2[0] != '\0';
    const int focus_block_height = static_cast<int>(ui_font_body.height) *
                                   (focus_has_second_line ? 2 : 1) +
                                   (focus_has_second_line ? 4 : 0);
    int focus_text_y = kHeroY + 18 + std::max(0, (kHeroH - 36 - focus_block_height) / 2);
    DrawTextCentered(188, focus_text_y, 236, ui_font_body.height, focus_line1, ui_font_body);
    if (focus_has_second_line) {
        DrawTextCentered(188, focus_text_y + ui_font_body.height + 4, 236, ui_font_body.height,
                         focus_line2, ui_font_body);
    }

    // Schedule: three equal reading lanes (time / title / state) replace the
    // previous loose text row.  Centering the title in its lane keeps long and
    // short labels optically balanced.
    FillRect(kMargin, kHeroRuleY, kContentWidth, 1, true);
    DrawText(32, kScheduleHeaderY, "02", ui_font_small);
    DrawText(70, kScheduleHeaderY - 2, "近期日程", ui_font_body);
    const int calendar_x = 400;
    StrokeRoundRect(calendar_x, kScheduleHeaderY + 3, 25, 22, 4, 1);
    FillRect(calendar_x, kScheduleHeaderY + 10, 25, 2, true);
    FillRect(calendar_x + 6, kScheduleHeaderY - 1, 3, 8, true);
    FillRect(calendar_x + 16, kScheduleHeaderY - 1, 3, 8, true);
    FillRect(kMargin, kScheduleFirstRowY - 2, kContentWidth, 1, true);
    for (size_t i = 0; i < dashboard::kScheduleCount; ++i) {
        if (i >= snapshot.schedule_count) break;
        const auto& item = snapshot.schedule[i];
        const int row_y = kScheduleFirstRowY +
                          static_cast<int>(i) * (kScheduleRowHeight + kScheduleRowGap);
        DrawTextCentered(32, row_y, 100, kScheduleRowHeight, item.time, ui_font_status);
        FillRect(132, row_y + 8, 1, kScheduleRowHeight - 16, true);
        char title[64];
        FitText(item.title, ui_font_status, 190, title, sizeof(title));
        DrawTextCentered(148, row_y, 192, kScheduleRowHeight, title, ui_font_status);
        FillRect(356, row_y + 8, 1, kScheduleRowHeight - 16, true);
        if (item.done) {
            FillCircle(402, row_y + kScheduleRowHeight / 2, 5, true);
        } else {
            StrokeCircle(402, row_y + kScheduleRowHeight / 2, 5, 1);
        }
        FillRect(kMargin, row_y + kScheduleRowHeight, kContentWidth, 1, true);
    }

    // Quota section: labels stay quiet and the values carry the hierarchy.  This
    // avoids the previous sentence-like line and remains useful offline.
    FillRect(kMargin, kQuotaY - 16, kContentWidth, 1, true);
    FillRect(240, kQuotaY + 14, 1, kQuotaH - 28, true);
    const int five = std::clamp(static_cast<int>(snapshot.quota.five_hour_remaining), 0, 100);
    const int weekly = std::clamp(static_cast<int>(snapshot.quota.weekly_remaining), 0, 100);
    DrawTextCentered(44, kQuotaY + 8, 184, 24, "Codex  ·  5小时", ui_font_small);
    DrawTextCentered(252, kQuotaY + 8, 184, 24, "本周额度", ui_font_small);
    char quota_left_value[16];
    char quota_right_value[16];
    if (snapshot.quota.valid) {
        std::snprintf(quota_left_value, sizeof(quota_left_value), "%d%%", five);
        std::snprintf(quota_right_value, sizeof(quota_right_value), "%d%%", weekly);
    } else {
        CopyDisplayText(quota_left_value, sizeof(quota_left_value), "--");
        CopyDisplayText(quota_right_value, sizeof(quota_right_value), "--");
    }
    DrawTextCentered(44, kQuotaY + 28, 184, 48, quota_left_value, ui_font_title);
    DrawTextCentered(252, kQuotaY + 28, 184, 48, quota_right_value, ui_font_title);
    if (!snapshot.quota.valid) {
        DrawTextCentered(44, kQuotaY + 68, 184, 20, "未连接", ui_font_small);
        DrawTextCentered(252, kQuotaY + 68, 184, 20, "未连接", ui_font_small);
    }
    // Custom section: the title and value sit in two centered lanes inside a
    // compact footer, leaving a generous bottom safe area.
    FillRect(kMargin, kQuotaRuleY, kContentWidth, 1, true);
    const dashboard::CustomCard* custom = nullptr;
    for (size_t i = 0; i < dashboard::kCustomCardCount; ++i) {
        if (snapshot.custom[i].enabled) {
            custom = &snapshot.custom[i];
            break;
        }
    }
    char custom_title[32];
    if (custom != nullptr && custom->title[0] != '\0') {
        FitText(custom->title, ui_font_status, 120, custom_title, sizeof(custom_title));
    } else {
        CopyDisplayText(custom_title, sizeof(custom_title), "自定义");
    }
    DrawTextCentered(48, kCustomY + 8, 128, 20, "自定义", ui_font_small);
    DrawTextCentered(48, kCustomY + 31, 128, 32, custom_title, ui_font_status);
    FillRect(192, kCustomY + 12, 1, kCustomH - 24, true);
    char custom_value[56];
    if (custom != nullptr && custom->value[0] != '\0') {
        CopyDisplayText(custom_value, sizeof(custom_value), custom->value);
    } else {
        CopyDisplayText(custom_value, sizeof(custom_value), "等待配置");
    }
    if (TextWidth(custom_value, ui_font_title) <= 224) {
        DrawTextCentered(208, kCustomY + 20, 224, 44, custom_value, ui_font_title);
    } else {
        char custom_line1[56];
        char custom_line2[56];
        FitTextLines(custom_value, ui_font_body, 216, custom_line1, sizeof(custom_line1),
                     custom_line2, sizeof(custom_line2));
        DrawTextLinesCentered(208, kCustomY + 14, 224, 56, custom_line1, custom_line2, ui_font_body);
    }
    // Keep the final rule inside the same vertical safe area as the side
    // margins instead of leaving a shallow strip at the panel edge.
    FillRect(kMargin, kCustomRuleY, kContentWidth, 1, true);
}

void RawDisplay::DrawProductLabelLocked(int x, int y, int width, const char* text,
                                        const ui_font_t& font) {
    char fitted[160];
    FitText(text, font, width, fitted, sizeof(fitted));
    DrawText(x, y, fitted, font);
}

void RawDisplay::DrawProductChevronLocked(int x, int y) {
    DrawProductIconLocked(lucide::Id::ChevronRight, x - 6, y - 5, 24, true);
}

void RawDisplay::DrawProductIconLocked(lucide::Id id, int x, int y, int size, bool black) {
    const auto& icon=lucide::Get(id,size);
    for (int row=0;row<icon.size;++row) for (int col=0;col<icon.size;++col) {
        const int bit=row*icon.size+col;
        if (icon.bits[bit/8] & (0x80>>(bit%8))) SetPixel(x+col,y+row,black);
    }
}

void RawDisplay::DrawProductIconRowLocked(int y, lucide::Id icon, const char* title,
                                         const char* detail, bool selected) {
    if (selected) FillRoundRect(32,y+8,40,40,12,true);
    DrawProductIconLocked(icon,38,y+14,28,!selected);
    DrawProductLabelLocked(88,y,328,title,ui_font_body);
    DrawProductLabelLocked(88,y+34,328,detail,ui_font_small);
    DrawProductChevronLocked(436,y+22);
    FillRect(88,y+kUiRowHeight-1,360,1,true);
}

void RawDisplay::DrawProductHeadingLocked(const char* title, const char* index) {
    DrawProductLabelLocked(kUiInset, kUiTitleY, kUiContentWidth - 112, title, ui_font_title);
    DrawProductLabelLocked(kPortraitW - kUiInset - 96, kUiTitleY + 12, 96,
                           index, ui_font_small);
    FillRect(kUiInset, kUiBodyY - 16, kUiContentWidth, 1, true);
}

void RawDisplay::DrawProductClockLocked(int x, int y, const char* text) {
    // Original 5x7 digits built from circles; no font download or runtime
    // rasterizer. Deliberately static: seconds and blinking colons cost refreshes.
    static constexpr uint8_t digits[10][7] = {
        {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},
        {14, 17, 1, 2, 4, 8, 31}, {30, 1, 1, 14, 1, 1, 30},
        {2, 6, 10, 18, 31, 2, 2}, {31, 16, 16, 30, 1, 1, 30},
        {14, 16, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},
        {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 1, 14},
    };
    constexpr int pitch = 16;
    for (const char* p = text; *p; ++p) {
        if (*p == ':') {
            FillCircle(x + 4, y + 2 * pitch + 4, 4, true);
            FillCircle(x + 4, y + 4 * pitch + 4, 4, true);
            x += 2 * pitch;
        } else if (*p >= '0' && *p <= '9') {
            for (int row = 0; row < 7; ++row) {
                for (int col = 0; col < 5; ++col) {
                    if (digits[*p - '0'][row] & (1 << (4 - col)))
                        FillCircle(x + col * pitch + 4, y + row * pitch + 4, 4, true);
                }
            }
            x += 6 * pitch;
        }
    }
}

void RawDisplay::DrawProductStatusBarLocked() {
    const dashboard::Snapshot snapshot = dashboard::DashboardData::GetInstance().GetSnapshot();
    if (product_page_ == ProductPage::Home) {
        DrawText(kUiInset, 16, "MIAO / INK", ui_font_small);
    } else {
        time_t now = time(nullptr);
        struct tm tmv{};
        localtime_r(&now, &tmv);
        char clock_text[8];
        std::snprintf(clock_text, sizeof(clock_text), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        DrawText(kUiInset, 16, clock_text, ui_font_small);
    }
    // Separate bounded slots keep long connection labels away from the battery.
    DrawProductLabelLocked(174, 16, 130, status_text_[0] ? status_text_ : snapshot.network, ui_font_small);
    char percent[8];
    std::snprintf(percent, sizeof(percent), "%d%%", std::clamp(battery_percent_, 0, 100));
    DrawText(406 - TextWidth(percent, ui_font_small), 16, percent, ui_font_small);
    const auto id=charging_ ? lucide::Id::BatteryCharging : battery_percent_>=75 ? lucide::Id::BatteryFull :
        battery_percent_>=40 ? lucide::Id::BatteryMedium : battery_percent_>0 ? lucide::Id::BatteryLow : lucide::Id::Battery;
    const auto glyph=raw_font::Lookup(ui_font_small,'8');
    const auto& icon=lucide::Get(id,32);
    const int center=16+glyph.y+glyph.height/2;
    DrawProductIconLocked(id,416,center-(icon.top+icon.bottom+1)/2,32,true);
}

void RawDisplay::DrawProductControlRailLocked(const char* context) {
    // One passive line. The capacitive cover keys and BOOT retain their routes;
    // there are no on-screen footer buttons or invisible footer hit targets.
    FillRect(kUiInset, kUiRailY, kUiContentWidth, 1, true);
    const bool notice = !quick_controls_open_.load() && !form_active_.load() && product_page_!=ProductPage::Reader && notification_text_[0] != '\0' &&
                        notification_deadline_ms_ > esp_timer_get_time() / 1000;
    const char* hint = notice ? notification_text_ :
                       (context ? context : "HOME 首页 / PREV 返回 / NEXT 下一项");
    DrawProductLabelLocked(kUiInset, kUiRailY + 10, kUiContentWidth, hint, ui_font_small);
}

void RawDisplay::DrawProductHomeLocked() {
    std::memset(portrait_fb_, kWhite, portrait_size_);
    const auto snapshot = dashboard::DashboardData::GetInstance().GetSnapshot();
    last_dashboard_revision_ = snapshot.revision;
    if (power_save_) {
        DrawTextCentered(kUiInset, 320, kUiContentWidth, 100, "休眠中", ui_font_title);
        return;
    }
    DrawProductStatusBarLocked();
    time_t now = time(nullptr);
    struct tm tmv{};
    localtime_r(&now, &tmv);
    const bool clock_valid = reminders::ValidClock(now);
    char clock_text[8];
    std::snprintf(clock_text, sizeof(clock_text), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    if (clock_valid) DrawProductClockLocked(40, 88, clock_text);
    else DrawTextCentered(32, 88, 416, 112, "等待校时", ui_font_title);
    static constexpr const char* weekdays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    char date_text[48];
    std::snprintf(date_text, sizeof(date_text), "%d 月 %d 日  %s", tmv.tm_mon + 1,
                  tmv.tm_mday, weekdays[std::clamp(tmv.tm_wday, 0, 6)]);
    DrawTextCentered(32, 216, 416, 40, clock_valid ? date_text : "联网后自动同步", ui_font_small);
    std::string next_alarm = "未设置", event_count = "暂无日程";
    int64_t earliest = 0;
    int events = 0;
    for (const auto& item : reminders::Service::Instance().List()) {
        const auto next = reminders::UpcomingOccurrence(item, now - 1);
        if (item.kind == "alarm" && next > 0 && (!earliest || next < earliest)) earliest = next;
        if (item.kind == "event" && next > 0) ++events;
    }
    if (earliest) next_alarm = reminders::LocalTime(earliest).substr(11, 5);
    if (events) event_count = std::to_string(events) + " 项安排";
    const char* names[] = {"闹钟", "日历", "笔记", "小智"};
    const char* details[] = {next_alarm.c_str(), event_count.c_str(), notes::DeviceStore().Ready() ? "记录与搜索" : "请检查 SD 卡",
        std::strcmp(snapshot.ai_status,"请绑定设备")==0 ? "请绑定设备" : "按住 AI 键说话"};
    for (int i = 0; i < 4; ++i) {
        const int x = kUiHomeX[i % 2], y = kUiHomeY[i / 2];
        StrokeRoundRect(x, y, kUiHomeW, kUiHomeH, 16, navigation_index_ == i ? 3 : 1);
        if(i==2)DrawProductIconLocked(lucide::Id::NotebookPen,x+20,y+16,40,true);
        else DrawProductAppIconLocked(i, x + 20, y + 16);
        DrawProductLabelLocked(x + 20, y + 64, kUiHomeW - 40, names[i], ui_font_body);
        DrawProductLabelLocked(x + 20, y + 108, kUiHomeW - 40, details[i], ui_font_small);
    }
    const char* nav[] = {"应用目录", "设备选项"};
    for (int i = 0; i < 2; ++i) {
        const int x = kUiHomeX[i];
        StrokeRoundRect(x, kUiHomeNavY, kUiHomeW, kUiHomeNavH, 16, navigation_index_ == i + 4 ? 3 : 1);
        DrawProductIconLocked(i ? lucide::Id::Settings2 : lucide::Id::LayoutGrid,x+16,kUiHomeNavY+20,24,true);
        DrawTextCentered(x+48, kUiHomeNavY, kUiHomeW-56, kUiHomeNavH, nav[i], ui_font_status);
    }
    const bool notice = notification_text_[0] && notification_deadline_ms_ > esp_timer_get_time()/1000;
    if (notice) DrawProductControlRailLocked("");
}

void RawDisplay::DrawProductAppIconLocked(int icon, int x, int y) {
    static constexpr lucide::Id ids[]={lucide::Id::AlarmClock,lucide::Id::CalendarDays,lucide::Id::Mic,lucide::Id::Bot};
    DrawProductIconLocked(ids[std::clamp(icon,0,3)],x,y,40,true);
}

void RawDisplay::DrawProductAlarmLocked() {
    std::memset(portrait_fb_, kWhite, portrait_size_);
    DrawProductStatusBarLocked();
    auto items = reminders::Service::Instance().List();
    items.erase(std::remove_if(items.begin(), items.end(), [](const auto& item) {
        return item.kind != "alarm";
    }), items.end());
    const auto now = time(nullptr);
    std::stable_sort(items.begin(), items.end(), [now](const auto& a, const auto& b) {
        const auto at = reminders::UpcomingOccurrence(a, now - 1), bt = reminders::UpcomingOccurrence(b, now - 1);
        return (at ? at : INT64_MAX) < (bt ? bt : INT64_MAX);
    });
    alarm_pages_ = std::max(1, (static_cast<int>(items.size()) + 3) / 4);
    alarm_page_ = std::clamp(alarm_page_, 0, alarm_pages_ - 1);
    char label[64];
    std::snprintf(label, sizeof(label), "%d 项", static_cast<int>(items.size()));
    DrawProductHeadingLocked("闹钟", label);
    std::fill(std::begin(alarm_ids_), std::end(alarm_ids_), 0);
    for (int row = 0; row < 4; ++row) {
        const int index = alarm_page_ * 4 + row;
        if (index >= static_cast<int>(items.size())) break;
        const auto& item = items[index];
        alarm_ids_[row] = item.id;
        alarm_enabled_[row] = item.enabled || item.snoozed_until;
        const int y = 144 + row * 112;
        const auto local = reminders::LocalTime(item.snoozed_until ? item.snoozed_until : item.at);
        DrawText(32, y, local.substr(11, 5).c_str(), ui_font_title);
        DrawProductLabelLocked(168, y + 10, 164, item.title.c_str(), ui_font_status);
        std::string repeat = item.snoozed_until ? "稍后提醒" : item.weekdays == 127 ? "每天" :
            item.weekdays == 31 ? "工作日" : item.weekdays == 96 ? "周末" :
            item.weekdays ? "每周重复" : local.substr(5,5) + " 仅一次";
        DrawProductLabelLocked(32, y + 60, 280, repeat.c_str(), ui_font_small);
        const bool enabled = alarm_enabled_[row];
        if (enabled) FillRoundRect(344, y + 16, 104, 48, 24, true);
        else StrokeRoundRect(344, y + 16, 104, 48, 24, 1);
        const char* state = enabled ? "已开启" : "已关闭";
        DrawTextInk(344 + (104 - TextWidth(state, ui_font_small))/2, y + 28, state, ui_font_small, !enabled);
        FillRect(32, y + 103, 416, 1, true);
    }
    if (items.empty()) {
        DrawProductAppIconLocked(0, 220, 212);
        DrawTextCentered(32, 292, 416, 48, "还没有闹钟", ui_font_body);
        DrawTextCentered(32, 356, 416, 40, "试着说：明早七点叫我", ui_font_small);
    }
    StrokeRoundRect(32, 600, 416, 48, 12, 1);
    DrawTextCentered(32, 600, 416, 48, "语音添加闹钟", ui_font_small);
    if (alarm_pages_ > 1) {
        for (int i = 0; i < 2; ++i) {
            StrokeRoundRect(kUiHomeX[i], 672, 200, 48, 12, 1);
            DrawTextCentered(kUiHomeX[i], 672, 200, 48, i ? "下一页" : "上一页", ui_font_small);
        }
    }
    std::snprintf(label, sizeof(label), "轻铃 / 第 %d 页，共 %d 页", alarm_page_ + 1, alarm_pages_);
    DrawProductControlRailLocked(label);
}

void RawDisplay::DrawProductRecorderLocked() {
    std::memset(portrait_fb_, kWhite, portrait_size_);
    DrawProductStatusBarLocked();
    DrawProductHeadingLocked("录音", "本地");
    const auto state = xiaozhi::AudioSession::GetInstance().RecorderState();
    const bool active = state.mode != audio::RecorderMode::Idle;
    DrawProductAppIconLocked(2, 220, 184);
    const char* status = state.mode == audio::RecorderMode::Recording ? "正在录音" :
                         state.mode == audio::RecorderMode::Playing ? "正在回放" :
                         state.mode == audio::RecorderMode::Loading ? "正在读取录音" :
                         state.failed ? "录音未完成" : state.has_clip ? "最近一段录音" : "记下此刻的声音";
    DrawTextCentered(32, 264, 416, 48, status, ui_font_body);
    char duration[32];
    std::snprintf(duration,sizeof(duration),"00:%02u",static_cast<unsigned>(state.seconds));
    DrawTextCentered(32, 336, 416, 56,
                     active ? "最长 30 秒" : state.has_clip ? duration : "最长 30 秒", ui_font_title);
    DrawTextCentered(32, 412, 416, 32, "本地回放 / 语音文字笔记", ui_font_small);
    FillRoundRect(32, 496, 416, 64, 16, true);
    const char* action = active ? "停止" : state.has_clip ? "重新录音" : "开始录音";
    DrawTextInk((480-TextWidth(action,ui_font_body))/2,512,action,ui_font_body,false);
    if (state.has_clip && !active) {
        StrokeRoundRect(32, 584, 416, 64, 16, 1);
        DrawTextCentered(32, 584, 416, 64, "回放录音", ui_font_body);
    }
    if (!active) {
        StrokeRoundRect(32, 672, 416, 64, 16, 1);
        DrawTextCentered(32, 672, 416, 64, "新建语音文字笔记", ui_font_status);
    }
    DrawProductControlRailLocked(active ? "点停止结束 / 返回时停止" :
                                 state.saved ? "最近录音已保存到 SD 卡" : "无 SD 存档 / 仅本次开机保留");
}

void RawDisplay::DrawProductAppsLocked() {
    std::memset(portrait_fb_, kWhite, portrait_size_);
    DrawProductStatusBarLocked();
    DrawProductHeadingLocked("应用目录", "7 项");
    static constexpr const char* names[] = {"闹钟", "日历", "录音", "小智", "AI 笔记", "我的胶囊", "阅读"};
    static constexpr const char* details[] = {"定时与重复提醒", "日期与日程", "离线录音、回放", "对话与语音助手", "备忘、清单与学习摘记", "保存的原文与回答", "继续阅读"};
    static constexpr lucide::Id icons[]={lucide::Id::AlarmClock,lucide::Id::CalendarDays,lucide::Id::Mic,
        lucide::Id::Bot,lucide::Id::NotebookPen,lucide::Id::StickyNote,lucide::Id::BookOpen};
    for (int i=0;i<7;++i) DrawProductIconRowLocked(kUiBodyY+i*kUiRowPitch,icons[i],names[i],details[i],navigation_index_==i);
    DrawProductControlRailLocked("返回首页 / 点击打开应用");
}

void RawDisplay::DrawProductAiLocked(bool details) {
    (void)details;
    std::memset(portrait_fb_, kWhite, portrait_size_);
    DrawProductStatusBarLocked();
    DrawProductLabelLocked(kUiInset,72,148,voice_note_mode_ ? "语音笔记" : "小智助手",ui_font_title);
    const auto dashboard = dashboard::DashboardData::GetInstance().GetSnapshot();
    if (std::strcmp(dashboard.ai_status,"请绑定设备")==0) {
        DrawText(32,168,"连接小智",ui_font_body);
        DrawProductLabelLocked(32,256,416,dashboard.ai_summary[0],ui_font_title);
        DrawText(32,344,"在 xiaozhi.me 输入绑定码",ui_font_status);
        DrawText(32,400,"绑定后即可使用语音和文字笔记",ui_font_small);
        DrawProductControlRailLocked("HOME 首页 / 等待绑定完成");
        return;
    }
    const auto snapshot = xiaozhi::Conversation::GetInstance().Snapshot();
    if (ai_drawn_turn_ != snapshot.turn) {
        ai_drawn_turn_ = snapshot.turn;
        ai_text_page_ = 0;
        ai_show_transcript_ = voice_note_mode_;
    }
    const char* states[] = {"按住说话 · 松开发送", "正在开启麦克风", "正在聆听 · 松手结束",
                            "正在识别", "AI 正在思考", "AI 正在回答", "已完成", "暂时无法完成"};
    const bool listening = snapshot.state == xiaozhi::TurnState::Listening;
    const bool busy = snapshot.state == xiaozhi::TurnState::Connecting || listening ||
                      snapshot.state == xiaozhi::TurnState::Transcribing ||
                      snapshot.state == xiaozhi::TurnState::Thinking || snapshot.state == xiaozhi::TurnState::Speaking;
    if(busy) {
        StrokeRoundRect(344,72,104,48,24,1);DrawTextCentered(344,72,104,48,"停止",ui_font_small);
    }else{
        const char* buttons[]={"输入","历史","新建"};const int xs[]={188,276,364};
        for(int i=0;i<3;++i){StrokeRoundRect(xs[i],72,84,48,12,1);DrawTextCentered(xs[i],72,84,48,buttons[i],ui_font_small);}
    }
    if (listening) FillCircle(40, 157, 6, true);
    else StrokeCircle(40, 157, 6, 1);
    DrawProductLabelLocked(60, 140, 388, states[static_cast<unsigned>(snapshot.state)], ui_font_status);
    const char* message = snapshot.message.empty() ? "按住机身 AI 键，说出此刻的想法。" : snapshot.message.c_str();
    char hint1[160], hint2[160];
    FitTextLines(message, ui_font_small, kUiContentWidth,
                 hint1, sizeof(hint1), hint2, sizeof(hint2));
    DrawText(kUiInset, 188, hint1, ui_font_small);
    DrawText(kUiInset, 216, hint2, ui_font_small);
    DrawProductLabelLocked(32,244,416,chat::History::Instance().Snapshot().message.c_str(),ui_font_small);

    FillRect(kUiInset, 272, kUiContentWidth, 1, true);
    DrawText(kUiInset, 296, ai_show_transcript_ ? "原文 / 切换" : "AI 回答 / 切换", ui_font_small);
    const std::string& body = ai_show_transcript_ ? snapshot.transcript : snapshot.answer;
    auto lines = raw_font::Wrap(body, kUiContentWidth, [](uint32_t cp) {
        return raw_font::Lookup(ui_font_body, cp).advance;
    });
    ai_page_count_ = std::max(1, (static_cast<int>(lines.size()) + kAiLinesPerPage - 1) / kAiLinesPerPage);
    ai_text_page_ = std::clamp(ai_text_page_, 0, ai_page_count_ - 1);
    char progress[24];
    std::snprintf(progress, sizeof(progress), "%02d / %02d", ai_text_page_ + 1, ai_page_count_);
    DrawText(448 - TextWidth(progress, ui_font_small), 296, progress, ui_font_small);
    if (body.empty()) {
        if (!snapshot.transcript.empty() && !ai_show_transcript_) {
            DrawText(kUiInset, 352, "你刚刚说", ui_font_small);
            char line1[192], line2[192];
            FitTextLines(snapshot.transcript.c_str(), ui_font_body, kUiContentWidth,
                         line1, sizeof(line1), line2, sizeof(line2));
            DrawText(kUiInset, 392, line1, ui_font_body);
            DrawText(kUiInset, 430, line2, ui_font_body);
        } else {
            DrawText(kUiInset, 376, listening ? "正在听你说…" : "让想法，随时留下。", ui_font_body);
            DrawText(kUiInset, 436, "每轮问答自动保存到本地历史", ui_font_small);
            DrawText(kUiInset, 468, "可整理灵感、提炼待办或翻译", ui_font_small);
        }
    } else {
        for (int i = 0; i < kAiLinesPerPage; ++i) {
            const int line = ai_text_page_ * kAiLinesPerPage + i;
            if (line >= static_cast<int>(lines.size())) break;
            DrawText(kUiInset, 340 + i * 36, lines[line].c_str(), ui_font_body);
        }
    }
    if (snapshot.truncated) DrawText(kUiInset, 586, "内容较长，已保留前段", ui_font_small);
    if(!busy && !snapshot.transcript.empty() && !snapshot.truncated) {
        const char* labels[]={"整理灵感","待办草稿","翻译英文","存为笔记"};
        for(int i=0;i<4;++i) {
            if(i==3 && snapshot.state!=xiaozhi::TurnState::Done)continue;
            const int x=kAiActionX[i%2],y=kAiActionY[i/2];
            StrokeRoundRect(x,y,kAiActionW,kAiActionH,24,1);
            const lucide::Id icons[]={lucide::Id::Sparkles,lucide::Id::ListTodo,lucide::Id::Languages,lucide::Id::NotebookPen};
            DrawProductIconLocked(icons[i],x+12,y+12,24,true);
            DrawTextCentered(x+38,y,kAiActionW-42,kAiActionH,labels[i],ui_font_small);
        }
    } else {
        DrawProductLabelLocked(32,632,416,busy ? "回答完成后可整理、翻译和存笔记" : "先按住机身 AI 键，说出你的想法",ui_font_small);
        DrawProductLabelLocked(32,684,416,"待办仅生成草稿，不自动创建提醒",ui_font_small);
    }
    DrawProductControlRailLocked(busy ? "AI 键松手结束 / HOME 首页" : "HOME 首页 / PREV 上页 / NEXT 下页");
}

void RawDisplay::DrawProductQuickNoteLocked() {
    std::memset(portrait_fb_, kWhite, portrait_size_);
    DrawProductStatusBarLocked();
    DrawProductHeadingLocked("我的胶囊", "NOTE");
    DrawText(kUiInset, 192, "按住 AI 键，说出你的想法。", ui_font_body);
    DrawText(kUiInset, 244, "松手后识别原文，自动保存到本机。", ui_font_small);
    DrawText(kUiInset, 284, "这里保留最近一次成功保存的胶囊。", ui_font_small);
    DrawText(kUiInset, 360, "灵感 / 待办 / 翻译", ui_font_status);
    StrokeRoundRect(kUiInset, 504, kUiContentWidth, 64, 24, 1);
    DrawTextCentered(kUiInset, 504, kUiContentWidth, 64, "打开最近胶囊", ui_font_body);
    DrawProductControlRailLocked(nullptr);
}



void RawDisplay::DrawProductTodayListLocked() {
    std::memset(portrait_fb_, kWhite, portrait_size_);
    DrawProductStatusBarLocked();
    DrawProductHeadingLocked("日历", "日程");
    const time_t now = time(nullptr);
    if (!reminders::ValidClock(now)) {
        DrawText(32, 200, "联网校时后显示日历", ui_font_body);
        DrawProductControlRailLocked("HOME 首页");
        return;
    }
    struct tm today{}; localtime_r(&now, &today);
    struct tm month = today;
    month.tm_mday = 1; month.tm_mon += calendar_month_;
    month.tm_hour = month.tm_min = month.tm_sec = 0;
    const time_t first = mktime(&month);
    struct tm following = month; ++following.tm_mon;
    const int days = static_cast<int>((mktime(&following) - first) / 86400);
    calendar_day_ = std::clamp(calendar_day_ ? calendar_day_ : today.tm_mday, 1, days);
    char label[48];
    std::snprintf(label, sizeof(label), "%d 年 %02d 月", month.tm_year + 1900, month.tm_mon + 1);
    DrawTextCentered(104, 136, 272, 48, label, ui_font_status);
    for (int i = 0; i < 2; ++i) {
        StrokeRoundRect(i ? 384 : 32, 136, 64, 48, 12, 1);
        DrawTextCentered(i ? 384 : 32, 136, 64, 48, i ? ">" : "<", ui_font_body);
    }
    const char* weekdays[] = {"一", "二", "三", "四", "五", "六", "日"};
    for (int i = 0; i < 7; ++i) DrawTextCentered(44 + i * 56, 192, 56, 32, weekdays[i], ui_font_small);
    const int lead = (month.tm_wday + 6) % 7;
    const auto items = reminders::Service::Instance().List();
    for (int day = 1; day <= days; ++day) {
        const int cell = lead + day - 1;
        const int x = 44 + cell % 7 * 56, y = 232 + cell / 7 * 44;
        const bool selected = day == calendar_day_;
        if (selected) FillRoundRect(x + 4, y, 48, 40, 12, true);
        else if (calendar_month_ == 0 && day == today.tm_mday) StrokeRoundRect(x + 4, y, 48, 40, 12, 1);
        char number[16]; std::snprintf(number, sizeof(number), "%d", day);
        DrawTextInk(x + (56 - TextWidth(number, ui_font_small))/2, y + 5, number, ui_font_small, !selected);
        for (const auto& item : items) {
            if (item.kind != "event") continue;
            const int64_t begin = first + (day - 1) * 86400;
            const auto occurrence = reminders::OccurrenceOnDay(item, begin);
            if (occurrence >= begin && occurrence < begin + 86400) {
                FillCircle(x + 28, y + 35, 2, !selected);
                break;
            }
        }
    }
    FillRect(32, 508, 416, 1, true);
    std::snprintf(label, sizeof(label), "%d 月 %d 日", month.tm_mon + 1, calendar_day_);
    DrawText(32, 516, label, ui_font_small);
    std::vector<std::pair<int64_t, reminders::Item>> events;
    const auto begin = first + (calendar_day_ - 1) * 86400;
    for (const auto& item : items) {
        if (item.kind != "event") continue;
        const auto next = reminders::OccurrenceOnDay(item, begin);
        if (next >= begin && next < begin + 86400) events.emplace_back(next, item);
    }
    std::sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const int pages = std::max(1, (static_cast<int>(events.size()) + 1)/2);
    calendar_events_page_ %= pages;
    for (int row = 0; row < 2; ++row) {
        const int index = calendar_events_page_ * 2 + row;
        if (index >= static_cast<int>(events.size())) break;
        const auto& event = events[index];
        const int y = 556 + row * 60;
        DrawText(32, y + 4, reminders::LocalTime(event.first).substr(11,5).c_str(), ui_font_small);
        DrawProductLabelLocked(128, y, 320, event.second.title.c_str(), ui_font_body);
    }
    if (events.empty()) DrawText(32, 564, "这一天没有日程", ui_font_body);
    StrokeRoundRect(32, 688, 200, 48, 12, 1);
    DrawTextCentered(32, 688, 200, 48, "语音添加", ui_font_small);
    if (pages > 1) {
        std::snprintf(label, sizeof(label), "更多 %d/%d", calendar_events_page_ + 1, pages);
        StrokeRoundRect(248, 688, 200, 48, 12, 1);
        DrawTextCentered(248, 688, 200, 48, label, ui_font_small);
    }
    DrawProductControlRailLocked("点日期查看 / PREV、NEXT 翻月");
}

void RawDisplay::DrawProductCardBoxLocked() {
    std::memset(portrait_fb_, kWhite, portrait_size_);
    DrawProductStatusBarLocked();
    DrawProductHeadingLocked("卡片盒", "CARDS");
    const auto snapshot = dashboard::DashboardData::GetInstance().GetSnapshot();
    int row = 0;
    auto draw_card = [&](const char* title, const char* detail) {
        const int y = kUiBodyY + row * kUiCardPitch;
        StrokeRoundRect(kUiInset, y, kUiContentWidth, kUiCardHeight, 16, 1);
        DrawProductLabelLocked(kUiInset + 20, y + 6, kUiContentWidth - 64, title, ui_font_body);
        DrawProductLabelLocked(kUiInset + 20, y + 46, kUiContentWidth - 64, detail, ui_font_small);
        DrawProductChevronLocked(kPortraitW - kUiInset - 28, y + 32);
        ++row;
    };
    for (size_t i = 0; i < dashboard::kCustomCardCount && row < 3; ++i) {
        if (snapshot.custom[i].enabled) draw_card(snapshot.custom[i].title, snapshot.custom[i].value);
    }
    for (size_t i = 0; i < dashboard::kAiSummaryCount && row < 3; ++i) {
        if (snapshot.ai_summary[i][0]) draw_card("AI 摘要", snapshot.ai_summary[i]);
    }
    if (row == 0) DrawText(kUiInset, 200, "还没有保存的卡片", ui_font_body);
    DrawProductControlRailLocked(nullptr);
}

bool RawDisplay::SelectCardSnapshotLocked(int selected) {
    const auto snapshot=dashboard::DashboardData::GetInstance().GetSnapshot();int row=0;
    for(const auto& card:snapshot.custom) if(card.enabled && row++==selected){selected_card_title_=card.title;selected_card_text_=card.value;return true;}
    for(const auto& summary:snapshot.ai_summary) if(summary[0] && row++==selected){selected_card_title_="AI 摘要";selected_card_text_=summary;return true;}
    return false;
}

void RawDisplay::DrawProductCardDetailLocked() {
    std::memset(portrait_fb_,kWhite,portrait_size_);DrawProductStatusBarLocked();
    DrawProductHeadingLocked("卡片详情","");
    DrawProductLabelLocked(32,168,416,selected_card_title_.empty()?"未选择卡片":selected_card_title_.c_str(),ui_font_body);
    const auto page=raw_font::Paginate(selected_card_text_,416,0,10,[](uint32_t cp){return raw_font::Lookup(ui_font_body,cp).advance;});
    for(size_t i=0;i<page.lines.size();++i)DrawText(32,228+i*40,page.lines[i].c_str(),ui_font_body);
    DrawProductLabelLocked(32,648,416,"来源 / 设备当前快照，非历史归档",ui_font_small);
    DrawProductControlRailLocked(selected_card_text_.empty()?"PREV 返回卡片列表":"NEXT 留屏 / PREV 返回卡片列表");
}

void RawDisplay::DrawProductKeepLocked() {
    std::memset(portrait_fb_,kWhite,portrait_size_);DrawText(32,32,"MIAO / INK",ui_font_small);
    DrawProductLabelLocked(32,160,416,selected_card_title_.empty()?"留屏":selected_card_title_.c_str(),ui_font_title);
    FillRect(32,244,48,2,true);
    const auto page=raw_font::Paginate(selected_card_text_,416,0,9,[](uint32_t cp){return raw_font::Lookup(ui_font_body,cp).advance;});
    for(size_t i=0;i<page.lines.size();++i)DrawText(32,288+i*38,page.lines[i].c_str(),ui_font_body);
    DrawProductControlRailLocked("点击屏幕或按键返回首页");
}

void RawDisplay::DrawProductWorkbenchLocked() {
    std::memset(portrait_fb_, kWhite, portrait_size_);
    DrawProductStatusBarLocked();
    DrawProductHeadingLocked("设备状态", "INFO");
    const auto snapshot = dashboard::DashboardData::GetInstance().GetSnapshot();
    DrawText(32, 152, "网络", ui_font_small);
    DrawProductLabelLocked(32, 188, 416, snapshot.network, ui_font_body);
    DrawText(32, 272, "天气", ui_font_small);
    char text[96];
    if (snapshot.weather.valid) std::snprintf(text,sizeof(text),"%d°  %s",snapshot.weather.temperature_c,snapshot.weather.condition);
    else std::snprintf(text,sizeof(text),"等待更新");
    DrawProductLabelLocked(32, 308, 416, text, ui_font_body);
    dashboard::FormatWeatherStatus(snapshot.weather,text,sizeof(text));
    DrawProductLabelLocked(32, 352, 416, text, ui_font_small);
    DrawText(32, 436, "额度", ui_font_small);
    if (snapshot.quota.valid) std::snprintf(text,sizeof(text),"5 小时 %d%%  /  本周 %d%%",std::clamp<int>(snapshot.quota.five_hour_remaining,0,100),std::clamp<int>(snapshot.quota.weekly_remaining,0,100));
    else std::snprintf(text,sizeof(text),"--");
    DrawProductLabelLocked(32, 472, 416, text, ui_font_status);
    dashboard::FormatQuotaStatus(snapshot.quota,text,sizeof(text));
    DrawProductLabelLocked(32, 520, 416, text, ui_font_small);
    DrawProductControlRailLocked("PREV 返回设备选项");
}

void RawDisplay::DrawProductSettingsLocked() {
    std::memset(portrait_fb_,kWhite,portrait_size_);DrawProductStatusBarLocked();DrawProductHeadingLocked("系统信息","INFO");
    const auto dashboard=dashboard::DashboardData::GetInstance().GetSnapshot();
    const auto system=device::QuickControls::Instance().Snapshot();
    const char* items[]={"界面字体","网络连接","固件版本","显示方式"};
    const char* values[]={"HarmonyOS Sans SC",dashboard.network,system.version.empty()?"未知":system.version.c_str(),"竖屏 / 纯黑白 / 无灰阶"};
    for(int i=0;i<4;++i){DrawText(32,152+i*120,items[i],ui_font_small);DrawProductLabelLocked(32,192+i*120,416,values[i],ui_font_body);}
    DrawProductControlRailLocked("字体按实际字号显示 / 顶部下拉控制栏");
}

void RawDisplay::DrawProductConfirmationLocked() {
    // Retained only as a legacy enum value; no menu exposes a fake task action.
    DrawProductQuickNoteLocked();
}

void RawDisplay::DrawProductMoreLocked() {
    std::memset(portrait_fb_, kWhite, portrait_size_);
    DrawProductStatusBarLocked();
    DrawProductHeadingLocked("设备设置", "设置");
    static constexpr const char* items[] = {"Wi-Fi", "设备状态", "刷新数据", "系统信息", "控制中心"};
    static constexpr const char* details[] = {"扫描网络、输入密码", "网络、天气与额度", "重新获取天气与额度", "字体、版本与连接", "音量、蓝牙、铃声与震动"};
    static constexpr lucide::Id icons[]={lucide::Id::Settings2,lucide::Id::Info,lucide::Id::RefreshCw,lucide::Id::Settings2,lucide::Id::Monitor};
    for (int i=0;i<5;++i) DrawProductIconRowLocked(kUiBodyY+i*kUiRowPitch,icons[i],items[i],details[i],navigation_index_==i);
    DrawProductControlRailLocked("PREV 返回首页 / NEXT 选择项目");
}

void RawDisplay::DrawProductScreenLocked() {
    // Mark the revision before drawing every page. Recording it afterwards
    // could swallow a provider update that arrives while pixels are drawn.
    last_dashboard_revision_ = dashboard::DashboardData::GetInstance().Revision();
    last_conversation_revision_ = xiaozhi::Conversation::GetInstance().Revision();
    last_recorder_revision_ = xiaozhi::AudioSession::GetInstance().RecorderState().revision;
    last_notes_revision_ = notes::DeviceStore().Revision();
    last_wifi_revision_ = network::WifiSetup::Instance().Revision();
    last_writer_revision_ = notes::Writer::Instance().Snapshot().revision;
    last_quick_revision_ = device::QuickControls::Instance().Revision();
    last_reader_revision_ = reader::Service::Instance().Revision();
    last_history_revision_ = chat::History::Instance().Revision();
    AdvanceFormsLocked();
    if (reminder_alert_.active) {
        password_reveal_=false;quick_controls_open_.store(false);
        DrawReminderAlertLocked();
        return;
    }
    if (discard_pending_) {DrawProductDiscardLocked();return;}
    if (quick_controls_open_.load()) {DrawProductQuickControlsLocked();return;}
    switch (product_page_) {
        case ProductPage::Home: DrawProductHomeLocked(); break;
        case ProductPage::AiResult: DrawProductAiLocked(false); break;
        case ProductPage::AiSteps: DrawProductAiLocked(true); break;
        case ProductPage::QuickNote: DrawProductQuickNoteLocked(); break;
        case ProductPage::Reader: DrawProductReaderLocked(); break;
        case ProductPage::TodayList: DrawProductTodayListLocked(); break;
        case ProductPage::CardBox: DrawProductCardBoxLocked(); break;
        case ProductPage::CardDetail: DrawProductCardDetailLocked(); break;
        case ProductPage::Keep: DrawProductKeepLocked(); break;
        case ProductPage::Apps: DrawProductAppsLocked(); break;
        case ProductPage::Workbench: DrawProductWorkbenchLocked(); break;
        case ProductPage::Settings: DrawProductSettingsLocked(); break;
        case ProductPage::Confirmation: DrawProductConfirmationLocked(); break;
        case ProductPage::More: DrawProductMoreLocked(); break;
        case ProductPage::Alarm: DrawProductAlarmLocked(); break;
        case ProductPage::Recorder: DrawProductRecorderLocked(); break;
        case ProductPage::ChatList: DrawProductHistoryLocked(false); break;
        case ProductPage::ChatDetail: DrawProductHistoryLocked(true); break;
        case ProductPage::Notes: DrawProductNotesLocked(false); break;
        case ProductPage::NoteDetail: DrawProductNotesLocked(true); break;
        case ProductPage::WifiList: DrawProductWifiLocked(false); break;
        case ProductPage::WifiCredentials: DrawProductWifiLocked(true); break;
        case ProductPage::TextEntry: DrawProductTextEntryLocked(); break;
        case ProductPage::NoteCompose: DrawProductNoteComposeLocked(); break;
    }
}

void RawDisplay::DrawHomeScreenLocked() {
    DrawProductScreenLocked();
}

void RawDisplay::UpdateGlassBinaryLocked(int x, int y, int w, int h) {
    if (!glass_nonwhite_fb_ || !glass_black_fb_ || !panel_fb_ || x < 0 || y < 0 || w <= 0 || h <= 0) return;
    const int stride = kPanelW / 8;
    const int first = x / 8;
    const int bytes = (w + 7) / 8;
    for (int row = 0; row < h; ++row) {
        const size_t offset = static_cast<size_t>(y + row) * stride + first;
        for (int col = 0; col < bytes; ++col) {
            const uint8_t black = static_cast<uint8_t>(~panel_fb_[offset + col]);
            glass_nonwhite_fb_[offset + col] = black;
            glass_black_fb_[offset + col] = black;
        }
    }
    paper_page_glass_uncertain_ = false;
}

bool RawDisplay::RecoverPanelForBinaryLocked() {
    if (!panel_custom_waveform_active_) return true;
    if (panel_ == nullptr) return false;
    ESP_LOGI(TAG, "recover SSD1677 after custom waveform");
    if (epaper_panel_recover(panel_) != ESP_OK) {
        ESP_LOGW(TAG, "SSD1677 custom-waveform recovery failed");
        // Treat both software histories as unknown.  The next caller must
        // retry recovery instead of issuing a binary waveform against RAM
        // planes that still contain Paper Mono/gray selectors.
        panel_history_valid_ = false;
        window_baseline_valid_ = false;
        return false;
    }
    panel_custom_waveform_active_ = false;
    panel_history_valid_ = false;
    window_baseline_valid_ = false;
    fast_refresh_count_ = 0;
    refresh_changed_bytes_ = 0;
    return true;
}

bool RawDisplay::FlushPartialLocked(int x, int y, int w, int h, bool incremental_du) {
    if (panel_region_fb_ == nullptr || !panel_history_valid_ || w <= 0 || h <= 0 ||
        (x & 7) != 0 || (w & 7) != 0 || x < 0 || y < 0 || x + w > kPanelW || y + h > kPanelH) {
        FlushLocked();
        return false;
    }
    const bool animation_wait = animation_running_;
    if ((animation_wait ? epaper_panel_wait_busy_timeout(panel_, 3000)
                        : epaper_panel_wait_busy(panel_)) != ESP_OK) {
        if (animation_wait) animation_running_ = false;
        return false;
    }
    const int stride = kPanelW / 8;
    const int row_bytes = w / 8;

    // Seed both SSD1677 RAM roles with the last committed frame once, because
    // a previous activation may have consumed/swapped them.
    const bool seed_baseline = !window_baseline_valid_;
    window_baseline_valid_ = false;
    if (seed_baseline) {
        epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_CURRENT);
        if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_prev_fb_) != ESP_OK) return false;
        epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_PREVIOUS);
        if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_prev_fb_) != ESP_OK) return false;
    }

    for (int row = 0; row < h; ++row) {
        std::memcpy(panel_region_fb_ + row * row_bytes,
                    panel_fb_ + (y + row) * stride + x / 8, row_bytes);
    }
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_CURRENT);
    if (esp_lcd_panel_draw_bitmap(panel_, x, y, x + w, y + h, panel_region_fb_) != ESP_OK) return false;
    if (!incremental_du) {
        epaper::CopyPreviousWithOutlineCleanup(panel_region_fb_, panel_prev_fb_, panel_fb_,
                                              kPanelW, kPanelH, x, y, w, h);
    } else {
        for (int row = 0; row < h; ++row) {
            std::memcpy(panel_region_fb_ + row * row_bytes,
                        panel_prev_fb_ + (y + row) * stride + x / 8, row_bytes);
        }
    }
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_PREVIOUS);
    if (esp_lcd_panel_draw_bitmap(panel_, x, y, x + w, y + h, panel_region_fb_) != ESP_OK) return false;
    // Normal UI updates must reload the panel's temperature-selected OTP
    // partial waveform. The resident 0x0C path belongs to custom-LUT tests.
    epaper_panel_set_refresh_mode(panel_, incremental_du ? SSD1677_EPAPER_REFRESH_DU
                                                       : SSD1677_EPAPER_REFRESH_PARTIAL);
    if (epaper_panel_refresh_screen(panel_) != ESP_OK) {
        if (animation_wait) animation_running_ = false;
        panel_history_valid_ = false;
        return false;
    }
    // MASTER_ACTIVATION may return before BUSY rises. Wait for the complete
    // waveform before synchronizing either RAM plane or the software history.
    const esp_err_t wait_err = epaper_panel_wait_refresh_timeout(panel_, animation_wait ? 3000 : 15000);
    if (wait_err != ESP_OK) {
        if (animation_wait) animation_running_ = false;
        ESP_LOGW(TAG, "partial refresh wait failed: %s", esp_err_to_name(wait_err));
        panel_history_valid_ = false;
        /* A failed experimental waveform must not poison the next button
         * press.  Reset/reinitialize the controller and force a known B/W
         * baseline before another differential window is attempted. */
        if (epaper_panel_recover(panel_) == ESP_OK) {
            panel_custom_waveform_active_ = false;
            panel_history_valid_ = false;
            window_baseline_valid_ = false;
            fast_refresh_count_ = 0;
            refresh_changed_bytes_ = 0;
        } else {
            ESP_LOGW(TAG, "partial refresh recovery failed");
        }
        return false;
    }

    // Commit this rectangle and make both controller roles equal to the new
    // target before the next window, matching Paper Mono's displayWindow().
    panel_history_valid_ = false;
    for (int row = 0; row < h; ++row) {
        std::memcpy(panel_region_fb_ + row * row_bytes,
                    panel_fb_ + (y + row) * stride + x / 8, row_bytes);
    }
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_CURRENT);
    if (esp_lcd_panel_draw_bitmap(panel_, x, y, x + w, y + h,
                                  panel_region_fb_) != ESP_OK) return false;
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_PREVIOUS);
    if (esp_lcd_panel_draw_bitmap(panel_, x, y, x + w, y + h,
                                  panel_region_fb_) != ESP_OK) return false;
    for (int row = 0; row < h; ++row) {
        std::memcpy(panel_prev_fb_ + (y + row) * stride + x / 8,
                    panel_fb_ + (y + row) * stride + x / 8, row_bytes);
    }
    UpdateGlassBinaryLocked(x, y, w, h);
    panel_history_valid_ = true;
    window_baseline_valid_ = true;
    return true;
}

bool RawDisplay::FlushBlackPulseLocked() {
    const bool had_history = panel_history_valid_;
    panel_history_valid_ = false;
    window_baseline_valid_ = false;
    if (epaper_panel_wait_busy(panel_) != ESP_OK) return false;

    // Use the existing PSRAM history buffer as scratch while history is
    // invalid. The panel driver copies PSRAM into its DMA bounce buffer.
    // Unknown glass (boot/recovery) needs a white->black drive everywhere.
    if (!had_history) std::memset(panel_prev_fb_, kWhite, panel_size_);
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_PREVIOUS);
    if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_prev_fb_) != ESP_OK) return false;
    std::memset(panel_prev_fb_, 0, panel_size_);
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_CURRENT);
    if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_prev_fb_) != ESP_OK) return false;
    epaper_panel_set_refresh_mode(panel_, SSD1677_EPAPER_REFRESH_PARTIAL);
    if (epaper_panel_refresh_screen(panel_) != ESP_OK ||
        epaper_panel_wait_refresh_timeout(panel_, 15000) != ESP_OK) return false;

    // One visible black pulse, then the target. Synchronize both RAM roles
    // after each waveform, since the controller may swap them on activation.
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_CURRENT);
    if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_prev_fb_) != ESP_OK) return false;
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_PREVIOUS);
    if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_prev_fb_) != ESP_OK) return false;
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_CURRENT);
    if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_fb_) != ESP_OK) return false;
    if (epaper_panel_refresh_screen(panel_) != ESP_OK ||
        epaper_panel_wait_refresh_timeout(panel_, 15000) != ESP_OK) return false;

    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_CURRENT);
    if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_fb_) != ESP_OK) return false;
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_PREVIOUS);
    if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_fb_) != ESP_OK) return false;
    std::memcpy(panel_prev_fb_, panel_fb_, panel_size_);
    UpdateGlassBinaryLocked(0, 0, kPanelW, kPanelH);
    panel_history_valid_ = true;
    window_baseline_valid_ = true;
    fast_refresh_count_ = 0;
    refresh_changed_bytes_ = 0;
    return true;
}

void RawDisplay::FlushLocked() {
    if (portrait_fb_ == nullptr || panel_fb_ == nullptr || panel_prev_fb_ == nullptr) return;
    if (!RecoverPanelForBinaryLocked()) return;

    for (int py = 0; py < kPortraitH; ++py) for (int px = 0; px < kPortraitW; ++px) {
        const bool white = (portrait_fb_[static_cast<size_t>(py) * (kPortraitW / 8) + (px >> 3)] & (0x80 >> (px & 7))) != 0;
        const int sx = py, sy = kPortraitW - 1 - px;
        uint8_t& b = panel_fb_[static_cast<size_t>(sy) * (kPanelW / 8) + (sx >> 3)];
        if (white) b |= static_cast<uint8_t>(0x80 >> (sx & 7)); else b &= static_cast<uint8_t>(~(0x80 >> (sx & 7)));
    }

    const auto damage = epaper::FindBinaryDamage(panel_prev_fb_, panel_fb_, kPanelW, kPanelH,
                                                epaper::kBinaryOutlineRadius);
    if (panel_history_valid_ && damage.changed_bytes == 0) return;
    const bool gc = panel_region_fb_ == nullptr ||
                    epaper::NeedsBinaryCleanup(panel_history_valid_, fast_refresh_count_,
                                                refresh_changed_bytes_, panel_size_);
    const int64_t started_us = esp_timer_get_time();
    if (!gc) {
        // One temperature-selected differential activation per changed frame.
        if (!FlushPartialLocked(damage.x, damage.y, damage.width, damage.height)) return;
        ++fast_refresh_count_;
        refresh_changed_bytes_ += damage.changed_bytes;
    } else {
        // FULL/FULL_FAST contain multiple optical inversions on this panel.
        // Two standard partial phases give one black pulse and then restore.
        if (!FlushBlackPulseLocked()) {
            ESP_LOGW(TAG, "black-pulse cleanup failed; baseline invalid");
            if (epaper_panel_recover(panel_) != ESP_OK) {
                ESP_LOGW(TAG, "black-pulse controller recovery failed");
            }
            return;
        }
    }
    ESP_LOGI(TAG, "refresh=%s window=%d,%d,%d,%d changed=%u partials=%lu phases=%d elapsed_ms=%lu",
             gc ? "black-pulse" : "partial", gc ? 0 : damage.x, gc ? 0 : damage.y,
             gc ? kPanelW : damage.width, gc ? kPanelH : damage.height,
             static_cast<unsigned>(damage.changed_bytes), static_cast<unsigned long>(fast_refresh_count_), gc ? 2 : 1,
             static_cast<unsigned long>((esp_timer_get_time() - started_us) / 1000));
}

void RawDisplay::FlushGray4Locked(const uint8_t* lsb, const uint8_t* msb,
                                  bool invert_planes, bool swap_planes,
                                  const uint8_t* lut, size_t lut_size) {
    if (!lsb || !msb || !panel_fb_ || !panel_prev_fb_) return;
    if (epaper_panel_wait_busy(panel_) != ESP_OK) return;
    if (!lut || epaper_panel_write_custom_lut(panel_, lut, lut_size) != ESP_OK) return;

    // SSD1677 uses the B/W RAM as the low bit and the RED RAM as the high bit.
    // The X4-family absolute grayscale LUT expects the host planes inverted;
    // the test pattern below is built in that public (black=00, white=11)
    // convention and complemented here before the panel write.
    for (size_t i = 0; i < panel_size_; ++i) {
        const uint8_t a = swap_planes ? msb[i] : lsb[i];
        const uint8_t b = swap_planes ? lsb[i] : msb[i];
        panel_fb_[i] = invert_planes ? static_cast<uint8_t>(~a) : a;
        panel_prev_fb_[i] = invert_planes ? static_cast<uint8_t>(~b) : b;
    }
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_CURRENT);
    if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_fb_) != ESP_OK) return;
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_PREVIOUS);
    if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_prev_fb_) != ESP_OK) return;
    epaper_panel_set_refresh_mode(panel_, SSD1677_EPAPER_REFRESH_GRAY4);
    if (epaper_panel_refresh_screen(panel_) != ESP_OK) return;
    if (epaper_panel_wait_refresh_timeout(panel_, 15000) != ESP_OK) {
        ESP_LOGW(TAG, "gray4 refresh wait failed");
        panel_history_valid_ = false;
        window_baseline_valid_ = false;
        // A failed custom waveform may leave the controller BUSY or with
        // selector RAM roles half-written.  Recover before another binary
        // test is allowed to use the panel.
        const esp_err_t recover_err = epaper_panel_recover(panel_);
        panel_custom_waveform_active_ = recover_err != ESP_OK;
        return;
    }
    // These RAM contents are gray planes, not a valid B/W differential pair.
    panel_history_valid_ = false;
    window_baseline_valid_ = false;
    panel_custom_waveform_active_ = true;
    // q24 marks non-white and q26 marks the black endpoint. For the current
    // diagnostic mapping, level 0 is white, level 1 is light gray, and the
    // MSB marks the dark/black endpoint (a future absolute LUT can refine it).
    if (glass_nonwhite_fb_ && glass_black_fb_) {
        for (size_t i = 0; i < panel_size_; ++i) {
            glass_nonwhite_fb_[i] = static_cast<uint8_t>(lsb[i] | msb[i]);
            glass_black_fb_[i] = msb[i];
        }
    }
    fast_refresh_count_ = 0;
    refresh_changed_bytes_ = 0;
}

void RawDisplay::FlushWipeTestLocked(int strip_width) {
    if (!panel_fb_ || !panel_prev_fb_ || !panel_region_fb_) return;
    // Wipe is a binary differential diagnostic.  A preceding gray/Paper
    // Mono test leaves selector planes and a custom LUT resident, so restore
    // the controller before seeding the white baseline for this test.
    if (!RecoverPanelForBinaryLocked()) return;
    if (epaper_panel_wait_busy(panel_) != ESP_OK) return;

    // Establish a white old frame, then reveal a black target from left to
    // right. Each strip is one ordinary differential partial update.
    std::memset(panel_fb_, kWhite, panel_size_);
    std::memset(panel_prev_fb_, kWhite, panel_size_);
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_CURRENT);
    if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_fb_) != ESP_OK) return;
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_PREVIOUS);
    if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_prev_fb_) != ESP_OK) return;
    epaper_panel_set_refresh_mode(panel_, SSD1677_EPAPER_REFRESH_FULL);
    if (epaper_panel_refresh_screen(panel_) != ESP_OK ||
        epaper_panel_wait_refresh_timeout(panel_, 15000) != ESP_OK) return;

    panel_history_valid_ = true;
    const int kStripW = std::clamp(strip_width & ~7, 8, kPanelW);
    const int stride = kPanelW / 8;
    for (int x = 0; x < kPanelW; x += kStripW) {
        const int w = std::min(kStripW, kPanelW - x);
        for (int y = 0; y < kPanelH; ++y) {
            for (int xx = x; xx < x + w; ++xx) {
                panel_fb_[static_cast<size_t>(y) * stride + (xx >> 3)] &=
                    static_cast<uint8_t>(~(0x80u >> (xx & 7)));
            }
        }
        FlushPartialLocked(x, 0, (w + 7) & ~7, kPanelH);
    }
    panel_history_valid_ = false;
    window_baseline_valid_ = false;
    UpdateGlassBinaryLocked(0, 0, kPanelW, kPanelH);
}

void RawDisplay::RunAnimationTestLocked(bool incremental_du) {
    animation_frames_ = 0;
    animation_total_us_ = 0;
    animation_mode_du_ = incremental_du;
    std::fill(animation_frame_us_, animation_frame_us_ + 12, 0);
    if (!panel_fb_ || !panel_prev_fb_ || !panel_region_fb_ || !panel_history_valid_) {
        ESP_LOGW(TAG, "animation requires a valid B/W panel history; run TEST_CONSOLE first");
        return;
    }
    for (int frame = 0; frame < 10; ++frame) RunAnimationFrameLocked(incremental_du, frame);
}

void RawDisplay::RunAnimationFrameLocked(bool incremental_du, int frame) {
    if (!panel_fb_ || !panel_prev_fb_ || !panel_region_fb_ || frame < 0 || frame >= 10) return;
    constexpr int kBlockW = 64;
    constexpr int kBlockH = 160;
    constexpr int kStartX = 16;
    constexpr int kY = 160;
    constexpr int kStepX = 64;
    constexpr int kWindowW = kBlockW * 2;
    const int stride = kPanelW / 8;
    const int x = kStartX + frame * kStepX;
    const int old_x = frame == 0 ? x : x - kStepX;
    const int window_x = frame == 0 ? x : old_x;
    const int window_w = frame == 0 ? kBlockW : kWindowW;
    for (int row = kY; row < kY + kBlockH; ++row) {
        if (frame != 0) {
            for (int xx = old_x; xx < old_x + kBlockW; ++xx) {
                panel_fb_[static_cast<size_t>(row) * stride + (xx >> 3)] |=
                    static_cast<uint8_t>(0x80u >> (xx & 7));
            }
        }
        for (int xx = x; xx < x + kBlockW; ++xx) {
            panel_fb_[static_cast<size_t>(row) * stride + (xx >> 3)] &=
                static_cast<uint8_t>(~(0x80u >> (xx & 7)));
        }
    }
    const int64_t start_us = esp_timer_get_time();
    FlushPartialLocked(window_x, kY, window_w, kBlockH, incremental_du);
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    animation_total_us_ += elapsed_us;
    if (animation_frames_ < 12) animation_frame_us_[animation_frames_] = elapsed_us;
    ++animation_frames_;
}

void RawDisplay::RunAnimationTest(bool incremental_du) {
    if (!Lock(3000)) {
        ESP_LOGW(TAG, "animation could not lock display for setup");
        animation_running_ = false;
        return;
    }
    {
        screen_test_mode_ = false;
        test_console_mode_ = true;
        animation_frames_ = 0;
        animation_total_us_ = 0;
        animation_mode_du_ = incremental_du;
        std::fill(animation_frame_us_, animation_frame_us_ + 12, 0);
        if (!panel_fb_ || !panel_prev_fb_ || !panel_region_fb_ || !portrait_fb_) {
            ESP_LOGW(TAG, "animation buffers are unavailable");
            Unlock();
            animation_running_ = false;
            return;
        }
        // A gray/Paper Mono test deliberately invalidates the binary diff
        // history.  Rebuild the button console here so the next DU/FC tap can
        // be run directly without requiring a separate TEST_CONSOLE command.
        if (!panel_history_valid_) {
            std::memset(portrait_fb_, kWhite, portrait_size_);
            DrawTestConsoleLocked();
            FlushLocked();
        }
        if (!panel_history_valid_) {
            ESP_LOGW(TAG, "animation could not establish a B/W panel history");
            Unlock();
            animation_running_ = false;
            return;
        }
    }
    Unlock();
    // Keep the display lock scoped to one window. A complete animation can
    // take seconds on the glass; holding it across all frames starves UI and
    // makes watchdog resets look like panel failures.
    for (int frame = 0; frame < 10 && animation_running_; ++frame) {
        if (!Lock(3000)) {
            ESP_LOGW(TAG, "animation could not lock display at frame %d", frame);
            animation_running_ = false;
            break;
        }
        RunAnimationFrameLocked(incremental_du, frame);
        Unlock();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void RawDisplay::RunPaperMonoTestLocked() {
    animation_frames_ = 0;
    animation_total_us_ = 0;
    std::fill(animation_frame_us_, animation_frame_us_ + 12, 0);
    if (!panel_fb_ || !panel_prev_fb_ || !panel_region_fb_ ||
        !glass_nonwhite_fb_ || !glass_black_fb_) {
        ESP_LOGW(TAG, "Paper Mono test buffers are unavailable");
        return;
    }

    screen_test_mode_ = false;
    test_console_mode_ = true;

    // Establish a known white glass state before the selector LUT. This keeps
    // the first custom activation interpretable even when the previous test
    // ended in a binary black frame.
    std::memset(portrait_fb_, kWhite, portrait_size_);
    panel_history_valid_ = false;
    window_baseline_valid_ = false;
    FlushLocked();
    if (epaper_panel_wait_busy_timeout(panel_, 8000) != ESP_OK) return;

    // Full three-level frame: white / Paper Mono middle gray / black. The
    // target q24/q26 masks are retained in the software glass model while the
    // two controller planes are replaced by selector masks for the first
    // custom activation.
    std::memset(panel_fb_, kWhite, panel_size_);       // B/W target (1=white)
    std::memset(panel_prev_fb_, 0, panel_size_);      // gray mask
    const int stride = kPanelW / 8;
    const int gray_y = kPanelH / 3;
    const int black_y = (kPanelH * 2) / 3;
    for (int y = 0; y < kPanelH; ++y) {
        if (y >= gray_y && y < black_y) {
            std::memset(panel_prev_fb_ + static_cast<size_t>(y) * stride, 0xFF, stride);
        } else if (y >= black_y) {
            std::memset(panel_fb_ + static_cast<size_t>(y) * stride, 0x00, stride);
        }
    }

    for (size_t i = 0; i < panel_size_; ++i) {
        const uint8_t bw = panel_fb_[i];
        const uint8_t gray = panel_prev_fb_[i];
        const uint8_t q24 = static_cast<uint8_t>(gray | ~bw);
        const uint8_t q26 = static_cast<uint8_t>(~bw & ~gray);
        glass_nonwhite_fb_[i] = q24;
        glass_black_fb_[i] = q26;
        // Corrective first pass: drive every pixel to its target class.
        panel_fb_[i] = static_cast<uint8_t>(~(q24 ^ q26));
        panel_prev_fb_[i] = q24;
    }

    uint8_t lut[112]{};
    (void)BuildPaperMonoTriLut(lut);
    if (epaper_panel_write_custom_lut(panel_, lut, sizeof(lut)) != ESP_OK) {
        ESP_LOGW(TAG, "Paper Mono LUT write failed");
        return;
    }
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_CURRENT);
    if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_fb_) != ESP_OK) return;
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_PREVIOUS);
    if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_prev_fb_) != ESP_OK) return;

    int64_t start_us = esp_timer_get_time();
    epaper_panel_set_refresh_mode(panel_, SSD1677_EPAPER_REFRESH_GRAY4);
    if (epaper_panel_refresh_screen(panel_) != ESP_OK ||
        epaper_panel_wait_refresh_timeout(panel_, 8000) != ESP_OK) {
        ESP_LOGW(TAG, "Paper Mono cold activation failed");
        return;
    }
    animation_frame_us_[0] = esp_timer_get_time() - start_us;
    animation_total_us_ = animation_frame_us_[0];
    animation_frames_ = 1;

    // Seed both RAM roles with the target selectors. Every following strip can
    // then use the resident 0x0C activation without reloading the LUT.
    for (size_t i = 0; i < panel_size_; ++i) panel_fb_[i] = glass_nonwhite_fb_[i];
    for (size_t i = 0; i < panel_size_; ++i) panel_prev_fb_[i] = glass_black_fb_[i];
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_CURRENT);
    if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_fb_) != ESP_OK) return;
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_PREVIOUS);
    if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_prev_fb_) != ESP_OK) return;

    // Move a full-height class boundary from left to right. This is a
    // deliberate Paper Mono window experiment: the LUT remains resident and
    // only one 96px window is driven per activation, so the user can compare
    // its visual motion with the B/W DU/FC buttons.
    constexpr int kStripW = 96;
    constexpr int kStripCount = kPanelW / kStripW;
    const int row_bytes = kStripW / 8;
    for (int step = 0; step < kStripCount && animation_running_; ++step) {
        const int x = step * kStripW;
        const uint8_t target24 = (step % 3 == 0) ? 0x00 : 0xFF;
        const uint8_t target26 = (step % 3 == 2) ? 0xFF : 0x00;
        for (int row = 0; row < kPanelH; ++row) {
            const size_t offset = static_cast<size_t>(row) * stride + x / 8;
            uint8_t* dst = panel_region_fb_ + static_cast<size_t>(row) * row_bytes;
            for (int col = 0; col < row_bytes; ++col) {
                const uint8_t old24 = glass_nonwhite_fb_[offset + col];
                const uint8_t old26 = glass_black_fb_[offset + col];
                const uint8_t changed = static_cast<uint8_t>(
                    (old24 ^ target24) | (old26 ^ target26));
                dst[col] = static_cast<uint8_t>(changed & ~(target24 ^ target26));
            }
        }
        epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_CURRENT);
        if (esp_lcd_panel_draw_bitmap(panel_, x, 0, x + kStripW, kPanelH,
                                      panel_region_fb_) != ESP_OK) break;
        for (int row = 0; row < kPanelH; ++row) {
            const size_t offset = static_cast<size_t>(row) * stride + x / 8;
            uint8_t* dst = panel_region_fb_ + static_cast<size_t>(row) * row_bytes;
            for (int col = 0; col < row_bytes; ++col) {
                const uint8_t old24 = glass_nonwhite_fb_[offset + col];
                const uint8_t old26 = glass_black_fb_[offset + col];
                const uint8_t changed = static_cast<uint8_t>(
                    (old24 ^ target24) | (old26 ^ target26));
                dst[col] = static_cast<uint8_t>(changed & target24);
            }
        }
        epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_PREVIOUS);
        if (esp_lcd_panel_draw_bitmap(panel_, x, 0, x + kStripW, kPanelH,
                                      panel_region_fb_) != ESP_OK) break;

        start_us = esp_timer_get_time();
        epaper_panel_set_refresh_mode(panel_, SSD1677_EPAPER_REFRESH_PARTIAL_WARM);
        if (epaper_panel_refresh_screen(panel_) != ESP_OK ||
            epaper_panel_wait_refresh_timeout(panel_, 5000) != ESP_OK) break;

        for (int row = 0; row < kPanelH; ++row) {
            const size_t offset = static_cast<size_t>(row) * stride + x / 8;
            for (int col = 0; col < row_bytes; ++col) {
                glass_nonwhite_fb_[offset + col] = target24;
                glass_black_fb_[offset + col] = target26;
            }
        }
        // Re-seed the addressed window with target selectors after the
        // activation, matching Paper Mono's post-window baseline step.
        std::memset(panel_region_fb_, target24, static_cast<size_t>(row_bytes) * kPanelH);
        epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_CURRENT);
        if (esp_lcd_panel_draw_bitmap(panel_, x, 0, x + kStripW, kPanelH,
                                      panel_region_fb_) != ESP_OK) break;
        std::memset(panel_region_fb_, target26, static_cast<size_t>(row_bytes) * kPanelH);
        epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_PREVIOUS);
        if (esp_lcd_panel_draw_bitmap(panel_, x, 0, x + kStripW, kPanelH,
                                      panel_region_fb_) != ESP_OK) break;

        const int64_t elapsed = esp_timer_get_time() - start_us;
        if (animation_frames_ < 12) animation_frame_us_[animation_frames_] = elapsed;
        animation_total_us_ += elapsed;
        ++animation_frames_;
    }
    panel_history_valid_ = false;
    window_baseline_valid_ = false;
    fast_refresh_count_ = 0;
    refresh_changed_bytes_ = 0;
    panel_custom_waveform_active_ = true;
}

void RawDisplay::DrawPaperMonoPageLocked(bool page_b, int origin_x) {
    if (panel_fb_ == nullptr || panel_prev_fb_ == nullptr) return;

    // The page renderer writes the two Paper Mono selector masks directly.
    // Keeping the layout in a separate helper lets the page-turn test render
    // page A, commit it, and then render page B without introducing another
    // full-size framebuffer.
    std::memset(panel_fb_, 0, panel_size_);       // q24: non-white classes
    std::memset(panel_prev_fb_, 0, panel_size_);  // q26: black endpoint
    paper_page_draw_origin_x_ = origin_x;

    constexpr int kLeft = 4;
    constexpr int kRight = kPaperPageWidth - 4;
    constexpr int kPageWidth = kRight - kLeft;
    FillPaperMonoClassRectLocked(kLeft, 16, kPageWidth, 4, 2);
    FillPaperMonoClassRectLocked(kLeft, 780, kPageWidth, 4, 2);
    FillPaperMonoClassRectLocked(kLeft, 16, 4, 768, 2);
    FillPaperMonoClassRectLocked(kRight - 4, 16, 4, 768, 2);
    FillPaperMonoClassRectLocked(12, 112, 376, 3, 1);
    FillPaperMonoClassRectLocked(12, 286, 376, 3, 1);
    FillPaperMonoClassRectLocked(12, 448, 376, 3, 1);

    DrawPaperMonoTextLocked(16, 30, page_b ? "PAGE B / 3-LEVEL" : "PAGE A / 3-LEVEL",
                            ui_font_title, 2);
    DrawPaperMonoTextLocked(16, 82, page_b ? "RASTER PAGE / NEW" : "RASTER PAGE / OLD",
                            ui_font_status, 1);

    // Swap the two raster samples between pages. Their coverage edges remain
    // genuine 2bpp font coverage, so the transition tests both solid classes
    // and the middle tone used by antialiased glyphs.
    DrawPaperMonoTextLocked(16, 132, page_b ? "三灰度文字" : "三灰度字体",
                            ui_font_h1, 2);
    DrawPaperMonoTextLocked(16, 198, page_b ? "三灰度字体" : "三灰度文字",
                            ui_font_h1, 1);
    DrawPaperMonoTextLocked(16, 254, page_b ? "NEW FRAME / BLACK EDGE" :
                            "OLD FRAME / BLACK EDGE", ui_font_title, 2);

    // A gray card gives the same black/gray contrast context on both pages,
    // while changing the copy makes a page replacement obvious at a glance.
    FillPaperMonoClassRectLocked(12, 304, 376, 126, 1);
    DrawPaperMonoTextLocked(28, 322, page_b ? "PAGE B CONTENT" : "PAGE A CONTENT",
                            ui_font_title, 2);
    DrawPaperMonoTextLocked(28, 372, page_b ? "NEXT FRAME" : "CURRENT FRAME",
                            ui_font_body, 2);

    // Three solid references make the result easy to grade by eye. Page B
    // rotates their order so every broad horizontal region has a real target
    // change, rather than testing only a few glyph pixels.
    const uint8_t swatches_a[3] = {2, 1, 0};
    const uint8_t swatches_b[3] = {0, 2, 1};
    const char* swatch_labels[3] = {"BLACK", "GRAY", "WHITE"};
    for (int i = 0; i < 3; ++i) {
        const int x = 18 + i * 126;
        const uint8_t level = page_b ? swatches_b[i] : swatches_a[i];
        FillPaperMonoClassRectLocked(x, 466, 112, 54, level);
        // The outline stays black even around the white class.
        FillPaperMonoClassRectLocked(x, 466, 112, 3, 2);
        FillPaperMonoClassRectLocked(x, 517, 112, 3, 2);
        FillPaperMonoClassRectLocked(x, 466, 3, 54, 2);
        FillPaperMonoClassRectLocked(x + 109, 466, 3, 54, 2);
        DrawPaperMonoTextLocked(x + 8, 530, swatch_labels[i], ui_font_small, 2);
    }

    DrawPaperMonoTextLocked(16, 580, page_b ? "PAGE B / NEW FRAME" : "PAGE A / OLD FRAME",
                            ui_font_status, 2);
    DrawPaperMonoTextLocked(16, 608, "WINDOW MAP  LEFT -> RIGHT", ui_font_small, 1);

    // Six mapped swatches retain different classes between pages. The actual
    // left-to-right wave uses six 80-pixel logical screen windows below.
    const uint8_t map_a[6] = {2, 1, 0, 2, 1, 0};
    const uint8_t map_b[6] = {0, 2, 1, 0, 2, 1};
    for (int i = 0; i < 6; ++i) {
        const int x = 8 + i * 64;
        FillPaperMonoClassRectLocked(x, 642, 64, 70, page_b ? map_b[i] : map_a[i]);
        FillPaperMonoClassRectLocked(x, 642, 64, 3, 2);
        FillPaperMonoClassRectLocked(x, 709, 64, 3, 2);
        FillPaperMonoClassRectLocked(x, 642, 3, 70, 2);
        FillPaperMonoClassRectLocked(x + 61, 642, 3, 70, 2);
    }
    DrawPaperMonoTextLocked(16, 728, page_b ? "TARGET B / WARM 0x0C" :
                            "TARGET A / COLD 0xCC", ui_font_small, 2);
    DrawPaperMonoTextLocked(16, 754, "PAPER PAGE TEST", ui_font_small, 1);
    paper_page_draw_origin_x_ = -1;
}

void RawDisplay::RunPaperMonoTextTestLocked() {
    animation_frames_ = 0;
    animation_total_us_ = 0;
    std::fill(animation_frame_us_, animation_frame_us_ + 12, 0);
    if (!portrait_fb_ || !panel_fb_ || !panel_prev_fb_ || !panel_region_fb_ ||
        !glass_nonwhite_fb_ || !glass_black_fb_) {
        ESP_LOGW(TAG, "Paper Mono text buffers are unavailable");
        return;
    }

    screen_test_mode_ = false;
    test_console_mode_ = true;

    // Leave the previous grayscale/animation state through the normal binary
    // recovery boundary, then establish a white optical baseline. The text
    // page is a complete three-class image, so it must never diff against
    // selector RAM left by the preceding test.
    std::memset(portrait_fb_, kWhite, portrait_size_);
    panel_history_valid_ = false;
    window_baseline_valid_ = false;
    FlushLocked();
    if (!panel_history_valid_ || epaper_panel_wait_busy_timeout(panel_, 8000) != ESP_OK) {
        ESP_LOGW(TAG, "Paper Mono text baseline failed");
        return;
    }

    // panel_fb_ = q24 (non-white selector), panel_prev_fb_ = q26 (black
    // endpoint selector). Both zero means white; q24=1/q26=0 is gray; both
    // one is black. Render the page straight into these landscape masks using
    // the portrait-to-panel transform in SetPaperMonoClassPixelLocked().
    std::memset(panel_fb_, 0, panel_size_);
    std::memset(panel_prev_fb_, 0, panel_size_);

    constexpr int kLeft = 20;
    constexpr int kRight = kPortraitW - 20;
    constexpr int kPageWidth = kRight - kLeft;
    // Four-pixel black frame, with a gray rule inside to make the middle tone
    // visible even if the glyph edges are too small to inspect unaided.
    FillPaperMonoClassRectLocked(kLeft, 14, kPageWidth, 4, 2);
    FillPaperMonoClassRectLocked(kLeft, 778, kPageWidth, 4, 2);
    FillPaperMonoClassRectLocked(kLeft, 14, 4, 768, 2);
    FillPaperMonoClassRectLocked(kRight - 4, 14, 4, 768, 2);
    FillPaperMonoClassRectLocked(28, 112, 424, 3, 1);
    FillPaperMonoClassRectLocked(28, 286, 424, 3, 1);
    FillPaperMonoClassRectLocked(28, 468, 424, 3, 1);

    DrawPaperMonoTextLocked(32, 30, "PAPER MONO 3L TEXT", ui_font_title, 2);
    DrawPaperMonoTextLocked(32, 82, "RASTER 2BPP -> 3 LEVEL", ui_font_status, 1);

    // The first two samples isolate the rasterizer: black ink carries its
    // antialiased fringe as Paper Mono gray, while gray ink keeps only the
    // denser coverage. This makes the source 2bpp coverage visible without
    // using a checkerboard or temporal dithering pattern.
    DrawPaperMonoTextLocked(32, 132, "三灰度字体", ui_font_h1, 2);
    DrawPaperMonoTextLocked(32, 198, "三灰度文字", ui_font_h1, 1);
    DrawPaperMonoTextLocked(32, 254, "BLACK / GRAY EDGE", ui_font_title, 2);

    // A middle-tone paper strip gives a second contrast context for the same
    // rasterized glyphs. Black text should remain legible on it; the gray
    // sample below intentionally approaches the background tone.
    FillPaperMonoClassRectLocked(28, 304, 424, 126, 1);
    DrawPaperMonoTextLocked(44, 322, "BLACK ON GRAY", ui_font_title, 2);
    DrawPaperMonoTextLocked(44, 372, "黑字 灰字", ui_font_body, 2);

    // Solid swatches are the optical reference for the text edges.
    FillPaperMonoClassRectLocked(34, 486, 122, 48, 2);
    FillPaperMonoClassRectLocked(179, 486, 122, 48, 1);
    // The white swatch is left untouched and outlined in black.
    FillPaperMonoClassRectLocked(324, 486, 122, 48, 0);
    FillPaperMonoClassRectLocked(34, 486, 122, 3, 2);
    FillPaperMonoClassRectLocked(34, 531, 122, 3, 2);
    FillPaperMonoClassRectLocked(34, 486, 3, 48, 2);
    FillPaperMonoClassRectLocked(153, 486, 3, 48, 2);
    FillPaperMonoClassRectLocked(179, 486, 122, 3, 2);
    FillPaperMonoClassRectLocked(179, 531, 122, 3, 2);
    FillPaperMonoClassRectLocked(179, 486, 3, 48, 2);
    FillPaperMonoClassRectLocked(298, 486, 3, 48, 2);
    FillPaperMonoClassRectLocked(324, 486, 122, 3, 2);
    FillPaperMonoClassRectLocked(324, 531, 122, 3, 2);
    FillPaperMonoClassRectLocked(324, 486, 3, 48, 2);
    FillPaperMonoClassRectLocked(443, 486, 3, 48, 2);
    DrawPaperMonoTextLocked(50, 546, "BLACK", ui_font_small, 2);
    DrawPaperMonoTextLocked(199, 546, "GRAY", ui_font_small, 2);
    DrawPaperMonoTextLocked(343, 546, "WHITE", ui_font_small, 2);

    DrawPaperMonoTextLocked(32, 596, "COVERAGE 0 1 2 3 -> W G B", ui_font_status, 2);
    DrawPaperMonoTextLocked(32, 636, "文字边 / RASTER EDGE", ui_font_body, 2);
    DrawPaperMonoTextLocked(32, 688, (test_variant_ & 1U) ? "MAP B: CRISP" : "MAP A: SOFT",
                            ui_font_small, 1);
    DrawPaperMonoTextLocked(32, 716, "PAPER TEXT TEST", ui_font_small, 2);

    // Preserve the target selector masks as the software glass model before
    // converting the working arrays into the first corrective activation.
    std::memcpy(glass_nonwhite_fb_, panel_fb_, panel_size_);
    std::memcpy(glass_black_fb_, panel_prev_fb_, panel_size_);
    for (size_t i = 0; i < panel_size_; ++i) {
        const uint8_t q24 = glass_nonwhite_fb_[i];
        const uint8_t q26 = glass_black_fb_[i];
        panel_fb_[i] = static_cast<uint8_t>(~(q24 ^ q26));
        panel_prev_fb_[i] = q24;
    }

    uint8_t lut[112]{};
    (void)BuildPaperMonoTriLut(lut);
    if (epaper_panel_write_custom_lut(panel_, lut, sizeof(lut)) != ESP_OK) {
        ESP_LOGW(TAG, "Paper Mono text LUT write failed");
        return;
    }
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_CURRENT);
    if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_fb_) != ESP_OK) return;
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_PREVIOUS);
    if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_prev_fb_) != ESP_OK) return;

    const int64_t start_us = esp_timer_get_time();
    epaper_panel_set_refresh_mode(panel_, SSD1677_EPAPER_REFRESH_GRAY4);
    if (epaper_panel_refresh_screen(panel_) != ESP_OK ||
        epaper_panel_wait_refresh_timeout(panel_, 8000) != ESP_OK) {
        ESP_LOGW(TAG, "Paper Mono text activation failed");
        (void)epaper_panel_recover(panel_);
        panel_history_valid_ = false;
        window_baseline_valid_ = false;
        return;
    }

    animation_frame_us_[0] = esp_timer_get_time() - start_us;
    animation_total_us_ = animation_frame_us_[0];
    animation_frames_ = 1;

    // Re-seed both controller roles with the target selector masks. This is
    // the same post-activation boundary used by the Paper Mono window path and
    // prevents a later binary test from mistaking gray RAM for old/new B/W.
    std::memcpy(panel_fb_, glass_nonwhite_fb_, panel_size_);
    std::memcpy(panel_prev_fb_, glass_black_fb_, panel_size_);
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_CURRENT);
    if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_fb_) != ESP_OK) return;
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_PREVIOUS);
    if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_prev_fb_) != ESP_OK) return;

    panel_history_valid_ = false;
    window_baseline_valid_ = false;
    fast_refresh_count_ = 0;
    refresh_changed_bytes_ = 0;
    panel_custom_waveform_active_ = true;
}

void RawDisplay::RunPaperMonoPageTestLocked() {
    animation_frames_ = 0;
    animation_total_us_ = 0;
    std::fill(animation_frame_us_, animation_frame_us_ + 12, 0);
    if (!portrait_fb_ || !panel_fb_ || !panel_prev_fb_ || !panel_region_fb_ ||
        !glass_nonwhite_fb_ || !glass_black_fb_) {
        ESP_LOGW(TAG, "Paper Mono page buffers are unavailable");
        return;
    }

    screen_test_mode_ = false;
    test_console_mode_ = true;

    // Keep the image already on the glass as the old frame. A full binary
    // white FlushLocked() here was the page-wide white flash reported by the
    // user; only a genuinely unknown glass history needs a corrective first
    // selector activation, and even that does not insert a separate clear.
    const bool force_first = paper_page_glass_uncertain_ ||
                             (!panel_history_valid_ && !panel_custom_waveform_active_);
    if (epaper_panel_wait_busy_timeout(panel_, 8000) != ESP_OK) {
        ESP_LOGW(TAG, "Paper Mono page BUSY before first frame");
        return;
    }

    // Page A occupies logical x=[0,400), page B x=[80,480). Their coverage
    // masks are distinct from q24/q26 ink classes, so a white area still
    // belongs to its page. Keep A's target q masks intact until the waveform
    // succeeds; advancing the glass model first would poison a retry.
    DrawPaperMonoPageLocked(false, 0);

    uint8_t lut[112]{};
    // Entry 0 stays idle during the turn. The ordinary Paper Mono background
    // top-up would otherwise touch every unchanged pixel in each window.
    (void)BuildPaperMonoTriLut(lut, false);
    if (epaper_panel_write_custom_lut(panel_, lut, sizeof(lut)) != ESP_OK) {
        ESP_LOGW(TAG, "Paper Mono page LUT write failed");
        return;
    }
    panel_custom_waveform_active_ = true;

    for (size_t i = 0; i < panel_size_; ++i) {
        const auto selectors = ComposePaperPageByte(
            glass_nonwhite_fb_[i], glass_black_fb_[i], panel_fb_[i], panel_prev_fb_[i],
            0xFFu, 0xFFu, force_first);
        panel_region_fb_[i] = selectors.current;
    }
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_CURRENT);
    if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_region_fb_) != ESP_OK) {
        ESP_LOGW(TAG, "Paper Mono page current plane write failed");
        return;
    }
    for (size_t i = 0; i < panel_size_; ++i) {
        const auto selectors = ComposePaperPageByte(
            glass_nonwhite_fb_[i], glass_black_fb_[i], panel_fb_[i], panel_prev_fb_[i],
            0xFFu, 0xFFu, force_first);
        panel_region_fb_[i] = selectors.previous;
    }
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_PREVIOUS);
    if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_region_fb_) != ESP_OK) {
        ESP_LOGW(TAG, "Paper Mono page previous plane write failed");
        return;
    }

    const int64_t cold_start_us = esp_timer_get_time();
    epaper_panel_set_refresh_mode(panel_, SSD1677_EPAPER_REFRESH_GRAY4);
    if (epaper_panel_refresh_screen(panel_) != ESP_OK ||
        epaper_panel_wait_refresh_timeout(panel_, 8000) != ESP_OK) {
        ESP_LOGW(TAG, "Paper Mono page cold activation failed");
        const esp_err_t recover_err = epaper_panel_recover(panel_);
        panel_custom_waveform_active_ = recover_err != ESP_OK;
        paper_page_glass_uncertain_ = true;
        panel_history_valid_ = false;
        window_baseline_valid_ = false;
        return;
    }
    std::memcpy(glass_nonwhite_fb_, panel_fb_, panel_size_);
    std::memcpy(glass_black_fb_, panel_prev_fb_, panel_size_);
    paper_page_glass_uncertain_ = false;
    panel_custom_waveform_active_ = true;
    animation_frame_us_[0] = esp_timer_get_time() - cold_start_us;
    animation_total_us_ = animation_frame_us_[0];
    animation_frames_ = 1;
    ESP_LOGI(TAG, "Paper page frame 0 page=A mode=diff_0xCC forced=%d ms=%lu",
             force_first ? 1 : 0, static_cast<unsigned long>(animation_frame_us_[0] / 1000));

    // After the absolute activation, seed both controller roles with page A's
    // q masks. A subsequent warm window then has the exact old/new selector
    // base expected by the Paper Mono driver.
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_CURRENT);
    if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_fb_) != ESP_OK) {
        ESP_LOGW(TAG, "Paper Mono page A current seed failed");
        return;
    }
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_PREVIOUS);
    if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_prev_fb_) != ESP_OK) {
        ESP_LOGW(TAG, "Paper Mono page A previous seed failed");
        return;
    }

    // Render page B only in the host-side q masks. The six full-height
    // logical columns are addressed in descending controller-Y order because
    // FlushLocked's portrait transform maps logical x to panel y=479-x.
    DrawPaperMonoPageLocked(true, kPaperPageBOriginX);
    constexpr int kStripW = 80;
    constexpr int kStripCount = kPortraitW / kStripW;
    const int stride = kPanelW / 8;
    const int row_bytes = kPanelW / 8;  // every window spans the full panel X
    bool failed = false;

    for (int step = 0; step < kStripCount && animation_running_; ++step) {
        const int logical_x = step * kStripW;
        const int panel_y = kPanelH - logical_x - kStripW;
        bool changed_any = false;
        uint32_t white_clear_pixels = 0;
        uint32_t enter_pixels = 0;
        uint32_t direct_pixels = 0;

        // A 1-bpp page-coverage mask is derived from each page rectangle, not
        // from its ink: old-only ink clears to white, new-only ink enters, and
        // ink shared by both frames changes gray/black directly. In the three
        // cases the selected white waveform is confined to old-only ink.
        for (int row = 0; row < kStripW; ++row) {
            const size_t offset = static_cast<size_t>(panel_y + row) * stride;
            uint8_t* dst = panel_region_fb_ + static_cast<size_t>(row) * row_bytes;
            const int portrait_x = kPortraitW - 1 - (panel_y + row);
            for (int col = 0; col < row_bytes; ++col) {
                const auto selectors = ComposePaperPageByte(
                    glass_nonwhite_fb_[offset + col], glass_black_fb_[offset + col],
                    panel_fb_[offset + col], panel_prev_fb_[offset + col],
                    PaperPageCoverageByte(portrait_x, col, 0),
                    PaperPageCoverageByte(portrait_x, col, kPaperPageBOriginX));
                const uint8_t driven = static_cast<uint8_t>(selectors.clear |
                                                              selectors.enter | selectors.direct);
                if (driven != 0) changed_any = true;
                white_clear_pixels += PaperPagePopcount8(selectors.clear);
                enter_pixels += PaperPagePopcount8(selectors.enter);
                direct_pixels += PaperPagePopcount8(selectors.direct);
                dst[col] = selectors.current;
            }
        }
        epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_CURRENT);
        if (esp_lcd_panel_draw_bitmap(panel_, 0, panel_y, kPanelW,
                                      panel_y + kStripW, panel_region_fb_) != ESP_OK) {
            ESP_LOGW(TAG, "Paper page step %d current plane write failed", step);
            failed = true;
            break;
        }

        // Previous selector uses the same three disjoint masks and target
        // classes; entry 0 in this page LUT is idle for unchanged pixels.
        for (int row = 0; row < kStripW; ++row) {
            const size_t offset = static_cast<size_t>(panel_y + row) * stride;
            uint8_t* dst = panel_region_fb_ + static_cast<size_t>(row) * row_bytes;
            const int portrait_x = kPortraitW - 1 - (panel_y + row);
            for (int col = 0; col < row_bytes; ++col) {
                const auto selectors = ComposePaperPageByte(
                    glass_nonwhite_fb_[offset + col], glass_black_fb_[offset + col],
                    panel_fb_[offset + col], panel_prev_fb_[offset + col],
                    PaperPageCoverageByte(portrait_x, col, 0),
                    PaperPageCoverageByte(portrait_x, col, kPaperPageBOriginX));
                dst[col] = selectors.previous;
            }
        }
        epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_PREVIOUS);
        if (esp_lcd_panel_draw_bitmap(panel_, 0, panel_y, kPanelW,
                                      panel_y + kStripW, panel_region_fb_) != ESP_OK) {
            ESP_LOGW(TAG, "Paper page step %d previous plane write failed", step);
            failed = true;
            break;
        }

        const int64_t start_us = esp_timer_get_time();
        epaper_panel_set_refresh_mode(panel_, SSD1677_EPAPER_REFRESH_PARTIAL_WARM);
        if (epaper_panel_refresh_screen(panel_) != ESP_OK ||
            epaper_panel_wait_refresh_timeout(panel_, 5000) != ESP_OK) {
            ESP_LOGW(TAG, "Paper page step %d warm activation failed", step);
            const esp_err_t recover_err = epaper_panel_recover(panel_);
            panel_custom_waveform_active_ = recover_err != ESP_OK;
            paper_page_glass_uncertain_ = true;
            panel_history_valid_ = false;
            window_baseline_valid_ = false;
            failed = true;
            break;
        }

        // Commit the target q masks to the software glass model for this
        // window. The rest of the page still represents page A until its turn.
        for (int row = 0; row < kStripW; ++row) {
            const size_t offset = static_cast<size_t>(panel_y + row) * stride;
            std::memcpy(glass_nonwhite_fb_ + offset, panel_fb_ + offset, row_bytes);
            std::memcpy(glass_black_fb_ + offset, panel_prev_fb_ + offset, row_bytes);
        }

        // Re-seed the addressed window with the target selectors. This mirrors
        // PaperMonoDriver::displayWindow() and keeps the controller's two RAM
        // roles aligned for the next 0x0C activation.
        for (int row = 0; row < kStripW; ++row) {
            const size_t offset = static_cast<size_t>(panel_y + row) * stride;
            std::memcpy(panel_region_fb_ + static_cast<size_t>(row) * row_bytes,
                        panel_fb_ + offset, row_bytes);
        }
        epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_CURRENT);
        if (esp_lcd_panel_draw_bitmap(panel_, 0, panel_y, kPanelW,
                                      panel_y + kStripW, panel_region_fb_) != ESP_OK) {
            ESP_LOGW(TAG, "Paper page step %d current seed failed", step);
            failed = true;
            break;
        }
        for (int row = 0; row < kStripW; ++row) {
            const size_t offset = static_cast<size_t>(panel_y + row) * stride;
            std::memcpy(panel_region_fb_ + static_cast<size_t>(row) * row_bytes,
                        panel_prev_fb_ + offset, row_bytes);
        }
        epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_PREVIOUS);
        if (esp_lcd_panel_draw_bitmap(panel_, 0, panel_y, kPanelW,
                                      panel_y + kStripW, panel_region_fb_) != ESP_OK) {
            ESP_LOGW(TAG, "Paper page step %d previous seed failed", step);
            failed = true;
            break;
        }

        const int64_t elapsed = esp_timer_get_time() - start_us;
        if (animation_frames_ < 12) animation_frame_us_[animation_frames_] = elapsed;
        animation_total_us_ += elapsed;
        ++animation_frames_;
        ESP_LOGI(TAG,
                 "Paper page frame %d logical_x=%d panel_y=%d changed=%d clear=%lu enter=%lu direct=%lu ms=%lu",
                 step + 1, logical_x, panel_y, changed_any ? 1 : 0,
                 static_cast<unsigned long>(white_clear_pixels),
                 static_cast<unsigned long>(enter_pixels),
                 static_cast<unsigned long>(direct_pixels),
                 static_cast<unsigned long>(elapsed / 1000));
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (failed) return;
    panel_history_valid_ = false;
    window_baseline_valid_ = false;
    fast_refresh_count_ = 0;
    refresh_changed_bytes_ = 0;
    panel_custom_waveform_active_ = true;
}

void RawDisplay::RunWavefrontTestLocked() {
    animation_frames_ = 0;
    animation_total_us_ = 0;
    std::fill(animation_frame_us_, animation_frame_us_ + 12, 0);
    if (!portrait_fb_ || !panel_fb_ || !panel_prev_fb_ || !panel_history_valid_) {
        ESP_LOGW(TAG, "wavefront requires a valid B/W panel history; run TEST_CONSOLE first");
        return;
    }

    // Establish a known optical baseline, then issue one absolute vertical
    // four-level frame with the single-byte timing experiment above. A camera
    // may show the final vertical bands, but any temporal movement is the
    // controller's gate scan and is not programmable as an X-axis wavefront.
    std::memset(portrait_fb_, kWhite, portrait_size_);
    panel_history_valid_ = false;
    FlushLocked();
    std::memset(panel_fb_, 0, panel_size_);
    std::memset(panel_prev_fb_, 0, panel_size_);
    const int stride = kPanelW / 8;
    for (int x = 0; x < kPanelW; ++x) {
        const uint8_t level = static_cast<uint8_t>(x / (kPanelW / 4));
        const uint8_t mask = static_cast<uint8_t>(0x80u >> (x & 7));
        for (int y = 0; y < kPanelH; ++y) {
            if (level & 1) panel_fb_[static_cast<size_t>(y) * stride + (x >> 3)] |= mask;
            if (level & 2) panel_prev_fb_[static_cast<size_t>(y) * stride + (x >> 3)] |= mask;
        }
    }
    animation_mode_du_ = false;
    const int64_t start_us = esp_timer_get_time();
    FlushGray4Locked(panel_fb_, panel_prev_fb_, true, false,
                     kGray4WavefrontLut, sizeof(kGray4WavefrontLut));
    animation_frame_us_[0] = esp_timer_get_time() - start_us;
    animation_total_us_ = animation_frame_us_[0];
    animation_frames_ = 1;
}

bool RawDisplay::StartAnimationTest(bool incremental_du) {
    if (animation_running_) return false;
    animation_mode_du_ = incremental_du;
    animation_wavefront_ = false;
    animation_papermono_ = false;
    animation_paper_text_ = false;
    animation_paper_page_ = false;
    animation_running_ = true;
    if (xTaskCreatePinnedToCore(&RawDisplay::AnimationTaskEntry, "epd_anim", 8192,
                                this, 2, &animation_task_, 0) != pdPASS) {
        animation_running_ = false;
        animation_task_ = nullptr;
        ESP_LOGW(TAG, "animation task creation failed");
        return false;
    }
    return true;
}

bool RawDisplay::StartWavefrontTest() {
    if (animation_running_) return false;
    animation_mode_du_ = false;
    animation_wavefront_ = true;
    animation_papermono_ = false;
    animation_paper_text_ = false;
    animation_paper_page_ = false;
    animation_running_ = true;
    if (xTaskCreatePinnedToCore(&RawDisplay::AnimationTaskEntry, "epd_wave", 8192,
                                this, 2, &animation_task_, 0) != pdPASS) {
        animation_running_ = false;
        animation_wavefront_ = false;
        animation_task_ = nullptr;
        ESP_LOGW(TAG, "wavefront task creation failed");
        return false;
    }
    return true;
}

bool RawDisplay::StartPaperMonoTest() {
    if (animation_running_) return false;
    animation_mode_du_ = false;
    animation_wavefront_ = false;
    animation_papermono_ = true;
    animation_paper_text_ = false;
    animation_paper_page_ = false;
    animation_running_ = true;
    if (xTaskCreatePinnedToCore(&RawDisplay::AnimationTaskEntry, "epd_pmono", 8192,
                                this, 2, &animation_task_, 0) != pdPASS) {
        animation_running_ = false;
        animation_papermono_ = false;
        animation_paper_text_ = false;
        animation_task_ = nullptr;
        ESP_LOGW(TAG, "Paper Mono task creation failed");
        return false;
    }
    return true;
}

bool RawDisplay::StartPaperMonoTextTest() {
    if (animation_running_) return false;
    animation_mode_du_ = false;
    animation_wavefront_ = false;
    animation_papermono_ = false;
    animation_paper_text_ = true;
    animation_paper_page_ = false;
    animation_running_ = true;
    if (xTaskCreatePinnedToCore(&RawDisplay::AnimationTaskEntry, "epd_ptext", 8192,
                                this, 2, &animation_task_, 0) != pdPASS) {
        animation_running_ = false;
        animation_paper_text_ = false;
        animation_task_ = nullptr;
        ESP_LOGW(TAG, "Paper Mono text task creation failed");
        return false;
    }
    return true;
}

bool RawDisplay::StartPaperMonoPageTest() {
    if (animation_running_) return false;
    animation_mode_du_ = false;
    animation_wavefront_ = false;
    animation_papermono_ = false;
    animation_paper_text_ = false;
    animation_paper_page_ = true;
    animation_running_ = true;
    if (xTaskCreatePinnedToCore(&RawDisplay::AnimationTaskEntry, "epd_ppage", 8192,
                                this, 2, &animation_task_, 0) != pdPASS) {
        animation_running_ = false;
        animation_paper_page_ = false;
        animation_task_ = nullptr;
        ESP_LOGW(TAG, "Paper Mono page task creation failed");
        return false;
    }
    return true;
}

void RawDisplay::AnimationTaskEntry(void* arg) {
    auto* self = static_cast<RawDisplay*>(arg);
    if (self != nullptr) self->AnimationTask();
    vTaskDelete(nullptr);
}

void RawDisplay::AnimationTask() {
    const bool incremental_du = animation_mode_du_;
    const bool wavefront = animation_wavefront_;
    const bool papermono = animation_papermono_;
    const bool paper_text = animation_paper_text_;
    const bool paper_page = animation_paper_page_;
    if (wavefront || papermono || paper_text || paper_page) {
        DisplayLockGuard lock(this);
        screen_test_mode_ = false;
        test_console_mode_ = true;
        if (papermono) RunPaperMonoTestLocked();
        else if (paper_text) RunPaperMonoTextTestLocked();
        else if (paper_page) RunPaperMonoPageTestLocked();
        else RunWavefrontTestLocked();
    } else {
        RunAnimationTest(incremental_du);
    }
    SendAnimationResultToSerial();
    animation_running_ = false;
    animation_task_ = nullptr;
}

void RawDisplay::SendAnimationResultToSerial() const {
    // Keep this diagnostic path deliberately small and 32-bit.  The ESP-IDF
    // nano printf implementation used by this target has a fragile varargs
    // path for mixed %lld/%s arguments; a completed animation must never
    // reboot merely while reporting its timing.
    char response[128];
    const uint32_t frames = std::min<uint32_t>(animation_frames_, 12);
    const uint32_t total_ms = animation_total_us_ > 0
                                  ? static_cast<uint32_t>(animation_total_us_ / 1000)
                                  : 0;
    const uint32_t avg_ms = frames != 0 ? total_ms / frames : 0;
    const int used = std::snprintf(
        response, sizeof(response), "@@%s_ACK frames=%lu total_ms=%lu avg_ms=%lu mode=%s\n",
        animation_paper_page_ ? "PAPER_PAGE" :
            (animation_paper_text_ ? "PAPER_TEXT" :
             (animation_papermono_ ? "PAPER_MONO" :
              (animation_wavefront_ ? "WAVEFRONT" : "ANIM"))),
        static_cast<unsigned long>(frames), static_cast<unsigned long>(total_ms),
        static_cast<unsigned long>(avg_ms),
        animation_paper_page_ ? "page_3l_masked" :
            (animation_paper_text_ ? "raster_3l" :
             (animation_papermono_ ? "tri_warm" :
              (animation_wavefront_ ? "wavefront" : (animation_mode_du_ ? "du" : "fc")))));
    if (used <= 0) return;
    const int fd = ::open("/dev/secondary", O_WRONLY | O_NONBLOCK);
    if (fd >= 0) {
        (void)SerialWriteAll(fd, response,
                             static_cast<size_t>(std::min(used, static_cast<int>(sizeof(response) - 1))));
        ::close(fd);
    }
}

void RawDisplay::ShowGray4TestPattern() {
    DisplayLockGuard lock(this);
    if (!portrait_fb_ || !panel_fb_ || !panel_prev_fb_) return;
    screen_test_mode_ = false;
    test_console_mode_ = true;

    // Start from a known optical state. This diagnostic clean is intentional:
    // it tells us whether the gray waveform works on the glass, rather than
    // measuring residual pixels from the button console.
    std::memset(portrait_fb_, kWhite, portrait_size_);
    panel_history_valid_ = false;
    FlushLocked();

    std::memset(panel_fb_, 0, panel_size_);       // public LSB plane
    std::memset(panel_prev_fb_, 0, panel_size_);  // public MSB plane
    const int stride = kPanelW / 8;
    for (int y = 0; y < kPanelH; ++y) {
        const uint8_t level = static_cast<uint8_t>(y / (kPanelH / 4));
        for (int x = 0; x < kPanelW; ++x) {
            const uint8_t mask = static_cast<uint8_t>(0x80u >> (x & 7));
            if (level & 1) panel_fb_[static_cast<size_t>(y) * stride + (x >> 3)] |= mask;
            if (level & 2) panel_prev_fb_[static_cast<size_t>(y) * stride + (x >> 3)] |= mask;
        }
    }
    const bool quality = test_variant_ >= 2;
    const bool invert = true;
    const bool swap = (test_variant_ & 1) != 0;
    FlushGray4Locked(panel_fb_, panel_prev_fb_, invert, swap,
                     quality ? kGray4QualityLut : kGray4Lut,
                     quality ? sizeof(kGray4QualityLut) : sizeof(kGray4Lut));
}

void RawDisplay::ShowWipeTestPattern() {
    DisplayLockGuard lock(this);
    screen_test_mode_ = false;
    test_console_mode_ = true;
    FlushWipeTestLocked(kWipeStripWidths[test_variant_ % kWipeStripWidthCount]);
}

void RawDisplay::UpdateStatusBar(bool update_all) {
    if(quick_controls_open_.load()) device::QuickControls::Instance().Refresh();
    DisplayLockGuard lock(this);
    if (!portrait_fb_) return;
    if (test_console_mode_) return;
    if (screen_test_mode_) {
        ++screen_test_tick_;
        if ((screen_test_tick_ % 3) == 0) {
            DrawTestPatternVariantLocked(screen_test_tick_ / 3);
            // Portrait (x=190..382,y=350..398) maps to panel
            // x=350..398, y=97..289 with the fixed 270-degree rotation.
            if ((screen_test_tick_ / 3) & 1) {
                FlushPartialLocked(344, 96, 56, 200);
            } else {
                FlushLocked();
            }
        }
        return;
    }
    bool discharging = false;
    Board::GetInstance().GetBatteryLevel(battery_percent_, charging_, discharging);
    time_t now = time(nullptr); struct tm tmv{}; localtime_r(&now, &tmv);
    const uint32_t dashboard_revision = dashboard::DashboardData::GetInstance().Revision();
    const int64_t now_ms = esp_timer_get_time() / 1000;
    const bool notification_expired = notification_text_[0] != '\0' &&
                                      notification_deadline_ms_ > 0 &&
                                      notification_deadline_ms_ <= now_ms;
    if (notification_expired) {
        notification_text_[0] = '\0';
        notification_deadline_ms_ = 0;
    }
    if (!update_all && tmv.tm_min == last_minute_ && battery_percent_ == last_drawn_battery_ &&
        charging_ == last_drawn_charging_ && dashboard_revision == last_dashboard_revision_ &&
        xiaozhi::Conversation::GetInstance().Revision() == last_conversation_revision_ &&
        xiaozhi::AudioSession::GetInstance().RecorderState().revision == last_recorder_revision_ && !notification_expired && notes::DeviceStore().Revision()==last_notes_revision_ &&
        network::WifiSetup::Instance().Revision()==last_wifi_revision_ &&
        notes::Writer::Instance().Snapshot().revision==last_writer_revision_ &&
        device::QuickControls::Instance().Revision()==last_quick_revision_ &&
        reader::Service::Instance().Revision()==last_reader_revision_ &&
        chat::History::Instance().Revision()==last_history_revision_) return;
    last_minute_ = tmv.tm_min;
    last_drawn_battery_ = battery_percent_;
    last_drawn_charging_ = charging_;
    DrawHomeScreenLocked();
    FlushLocked();
}

void RawDisplay::SetStatus(const char* status) {
    DisplayLockGuard lock(this);
    char next_status[sizeof(status_text_)];
    CopyDisplayText(next_status, sizeof(next_status), status);
    if (std::strcmp(status_text_, next_status) == 0) return;
    std::memcpy(status_text_, next_status, sizeof(status_text_));
    if (!portrait_fb_ || screen_test_mode_ || test_console_mode_) return;
    DrawHomeScreenLocked();
    FlushLocked();
}

void RawDisplay::ShowNotification(const char* notification, int duration_ms) {
    DisplayLockGuard lock(this);
    CopyDisplayText(notification_text_, sizeof(notification_text_), notification);
    const int64_t now_ms = esp_timer_get_time() / 1000;
    const int safe_duration_ms = std::clamp(duration_ms, 0, 60 * 1000);
    notification_deadline_ms_ = notification_text_[0] == '\0'
                                    ? 0
                                    : now_ms + static_cast<int64_t>(safe_duration_ms);
    if (!portrait_fb_ || screen_test_mode_ || test_console_mode_) return;
    DrawHomeScreenLocked();
    FlushLocked();
}

void RawDisplay::SetPowerSaveMode(bool on) {
    bool changed = false;
    {
        DisplayLockGuard lock(this);
        if (power_save_ == on || (on && form_active_.load())) return;
        power_save_ = on;
        changed = true;
        if (portrait_fb_ && !screen_test_mode_ && !test_console_mode_) {
            DrawHomeScreenLocked();
            FlushLocked();
        }
    }
    // Network power-save calls can invoke callbacks that touch the display;
    // keep them outside the framebuffer mutex.
    if (changed) Board::GetInstance().SetPowerSaveMode(on);
}

void RawDisplay::ShowPoweredOffScreen() {
    DisplayLockGuard lock(this);
    if (!portrait_fb_) return;
    std::memset(portrait_fb_, kWhite, portrait_size_);
    FlushLocked();
}
