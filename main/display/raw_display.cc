#include "raw_display.h"

#include "board.h"
#include "dashboard/dashboard_data.h"
#include "dashboard/dashboard_service.h"
#include "esp_lcd_ssd1677_commands.h"
#include "esp_lcd_panel_ssd1677.h"
#include "xiaozhi/xiaozhi_client.h"
#include "driver/usb_serial_jtag.h"
#include "hal/usb_serial_jtag_ll.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
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
// Match EegoRead's proven cadence: DU for normal frames, with an occasional
// GC pass to re-establish charge/history and clear accumulated ghosting.
constexpr uint32_t kFullRefreshEvery = 8;
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

// Product UI geometry from the shared portrait specification.  The legacy
// dashboard below keeps its 32 px editorial grid so old screenshots and
// serial diagnostics remain reproducible; new product pages use this 16 px
// grid and a fixed 2x2 action footer.
constexpr int kUiInset = 16;
constexpr int kUiContentWidth = kPortraitW - kUiInset * 2;
constexpr int kUiStatusHeight = 40;
constexpr int kUiTitleY = 56;
constexpr int kUiBodyY = 120;
constexpr int kUiBodyHeight = 504;
constexpr int kUiFooterY = 640;
constexpr int kUiFooterButtonWidth = 216;
constexpr int kUiFooterButtonHeight = 64;
constexpr int kUiFooterGap = 16;
constexpr int kUiFooterX[2] = {kUiInset, kUiInset + kUiFooterButtonWidth + kUiFooterGap};
constexpr int kUiFooterRowY[2] = {kUiFooterY, kUiFooterY + kUiFooterButtonHeight + kUiFooterGap};
constexpr int kUiListRowHeight = 64;
constexpr int kUiListRowGap = 8;
constexpr int kUiCardStroke = 2;

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
    if (fd < 0 || data == nullptr) return false;
    size_t offset = 0;
    int stalled = 0;
    while (offset < size && stalled < 2000) {
        const ssize_t written = ::write(fd, data + offset, size - offset);
        if (written > 0) {
            offset += static_cast<size_t>(written);
            stalled = 0;
            continue;
        }
        if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return false;
        }
        ++stalled;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return offset == size;
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
            const int x = std::clamp(static_cast<int>(point.x), 0, kPortraitW - 1);
            const int y = std::clamp(static_cast<int>(point.y), 0, kPortraitH - 1);
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
            if (tap) HandleHomeTap(x, y);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void RawDisplay::HandleHomeTap(int x, int y) {
    enum class Action { None, Gray4, PaperMono, PaperText, AnimDu, AnimFc, PaperPage };
    Action action = Action::None;
    bool ai_tap = false;
    bool refresh_tap = false;
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
            auto footer_index = [&]() {
                for (int i = 0; i < 4; ++i) {
                    const int row = i / 2;
                    const int column = i % 2;
                    if (in_rect(kUiFooterX[column], kUiFooterRowY[row],
                                kUiFooterButtonWidth, kUiFooterButtonHeight)) return i;
                }
                return -1;
            };

            ProductPage next_page = product_page_;
            switch (product_page_) {
                case ProductPage::Home: {
                    const int footer = footer_index();
                    if (footer == 0) next_page = ProductPage::QuickNote;
                    else if (footer == 1) next_page = ProductPage::Keep;
                    else if (footer == 2) next_page = ProductPage::Apps;
                    else if (footer == 3) {
                        next_page = ProductPage::AiResult;
                        ai_tap = true;
                    } else if (in_rect(kUiInset, kUiBodyY, kUiContentWidth, 248)) {
                        next_page = ProductPage::AiResult;
                    } else if (in_rect(kUiInset, 384, kUiContentWidth, 112)) {
                        next_page = ProductPage::TodayList;
                    } else if (in_rect(kUiInset, 512, kUiContentWidth, 112)) {
                        next_page = ProductPage::Reader;
                    }
                    break;
                }
                case ProductPage::Apps: {
                    const int row = (y - kUiBodyY) / (kUiListRowHeight + kUiListRowGap);
                    if (x >= kUiInset && x < kUiInset + kUiContentWidth &&
                        row >= 0 && row < 6 &&
                        y < kUiBodyY + 6 * (kUiListRowHeight + kUiListRowGap) - kUiListRowGap) {
                        static constexpr ProductPage kAppPages[] = {
                            ProductPage::Reader, ProductPage::AiResult, ProductPage::QuickNote,
                            ProductPage::CardBox, ProductPage::Settings, ProductPage::More,
                        };
                        next_page = kAppPages[row];
                    } else {
                        const int footer = footer_index();
                        if (footer == 0 || footer == 3) next_page = ProductPage::Home;
                        else if (footer == 1) next_page = ProductPage::CardBox;
                        else if (footer == 2) next_page = ProductPage::Settings;
                    }
                    break;
                }
                case ProductPage::AiResult:
                    if (footer_index() == 0) next_page = ProductPage::CardDetail;
                    else if (footer_index() == 1) next_page = ProductPage::Keep;
                    else if (footer_index() == 2) next_page = ProductPage::AiSteps;
                    else if (footer_index() == 3) next_page = ProductPage::Home;
                    else if (in_rect(kUiInset, kUiBodyY, kUiContentWidth, 248))
                        next_page = ProductPage::AiSteps;
                    break;
                case ProductPage::AiSteps:
                    if (footer_index() == 0) next_page = ProductPage::AiResult;
                    else if (footer_index() == 1) next_page = ProductPage::CardDetail;
                    else if (footer_index() == 2) next_page = ProductPage::Keep;
                    else if (footer_index() == 3) next_page = ProductPage::Home;
                    break;
                case ProductPage::QuickNote: {
                    const int footer = footer_index();
                    if (footer == 0) {
                        quick_note_state_ = quick_note_state_ < 1 ? 1 :
                                            quick_note_state_ == 1 ? 2 : quick_note_state_;
                        redraw = true;
                    } else if (footer == 1) {
                        quick_note_state_ = std::max<uint8_t>(quick_note_state_, 3);
                        redraw = true;
                    } else if (footer == 2) next_page = ProductPage::Confirmation;
                    else if (footer == 3) next_page = ProductPage::Home;
                    break;
                }
                case ProductPage::Reader: {
                    const int footer = footer_index();
                    if (footer == 0) {
                        if (reader_page_ > 0) --reader_page_;
                        redraw = true;
                    } else if (footer == 1) {
                        if (reader_page_ < 2) ++reader_page_;
                        redraw = true;
                    } else if (footer == 2) next_page = ProductPage::TodayList;
                    else if (footer == 3) next_page = ProductPage::Home;
                    break;
                }
                case ProductPage::TodayList:
                    if (y >= kUiBodyY && y < kUiBodyY + 3 * 76 && x >= kUiInset &&
                        x < kUiInset + kUiContentWidth) next_page = ProductPage::CardDetail;
                    else if (footer_index() == 0 || footer_index() == 3) next_page = ProductPage::Home;
                    else if (footer_index() == 2) next_page = ProductPage::CardBox;
                    break;
                case ProductPage::CardBox:
                    if (y >= kUiBodyY && y < kUiBodyY + 3 * 88 && x >= kUiInset &&
                        x < kUiInset + kUiContentWidth) next_page = ProductPage::CardDetail;
                    else if (footer_index() == 0 || footer_index() == 3) next_page = ProductPage::Home;
                    else if (footer_index() == 1) next_page = ProductPage::Keep;
                    break;
                case ProductPage::CardDetail:
                    if (footer_index() == 0) next_page = ProductPage::Home;
                    else if (footer_index() == 1) next_page = ProductPage::Keep;
                    else if (footer_index() == 2) next_page = ProductPage::QuickNote;
                    else if (footer_index() == 3) next_page = ProductPage::More;
                    break;
                case ProductPage::Keep:
                    // Any tap wakes the user from the independent snapshot.
                    next_page = ProductPage::Home;
                    break;
                case ProductPage::Workbench:
                    if (footer_index() == 0 || footer_index() == 3) next_page = ProductPage::Home;
                    else if (footer_index() == 1) refresh_tap = true;
                    else if (footer_index() == 2) next_page = ProductPage::Settings;
                    break;
                case ProductPage::Settings:
                    if (footer_index() == 0 || footer_index() == 3) next_page = ProductPage::Home;
                    else if (footer_index() == 1) {
                        // Orientation switching is deferred to the next phase;
                        // keep the user on the settings page with an explicit
                        // status message rather than applying stale hitboxes.
                        redraw = true;
                    } else if (footer_index() == 2) next_page = ProductPage::Workbench;
                    break;
                case ProductPage::Confirmation:
                    if (footer_index() == 0) next_page = ProductPage::QuickNote;
                    else if (footer_index() == 1) next_page = ProductPage::TodayList;
                    else if (footer_index() == 2) next_page = ProductPage::Keep;
                    else if (footer_index() == 3) next_page = ProductPage::Home;
                    break;
                case ProductPage::More:
                    if (y >= kUiBodyY && y < kUiBodyY + 4 * 82 && x >= kUiInset &&
                        x < kUiInset + kUiContentWidth) {
                        const int row = (y - kUiBodyY) / 82;
                        if (row == 0) next_page = ProductPage::Workbench;
                        else if (row == 1) {
                            test_console_mode_ = true;
                            DrawTestConsoleLocked();
                            FlushLocked();
                            return;
                        } else if (row == 2) refresh_tap = true;
                        else next_page = ProductPage::Settings;
                    } else if (footer_index() == 0 || footer_index() == 3) next_page = ProductPage::Home;
                    else if (footer_index() == 1) next_page = ProductPage::Workbench;
                    else if (footer_index() == 2) next_page = ProductPage::Settings;
                    break;
            }
            if (next_page != product_page_) {
                product_page_ = next_page;
                redraw = true;
            }
            if (redraw) {
                DrawHomeScreenLocked();
                FlushLocked();
            }
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
    if (ai_tap) {
        if (!ai_listening_) {
            if (xiaozhi::Client::GetInstance().ListenStart()) {
                ai_listening_ = true;
                ShowNotification("小智开始聆听", 2000);
            } else {
                ShowNotification("小智尚未连接", 2500);
            }
        } else {
            const bool accepted = xiaozhi::Client::GetInstance().ListenStop();
            ai_listening_ = false;
            ShowNotification(accepted ? "已发送结束聆听" : "小智连接已断开", 2000);
        }
        return;
    }
    if (refresh_tap) {
        dashboard::DashboardService::GetInstance().RefreshNow();
        ShowNotification("正在刷新天气与额度", 2000);
    }
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
    char command[96] = {};
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
        // Product pages reserve the top 40 px for the status bar.  Treat all
        // other in-page taps as candidates and let HandleHomeTap apply the
        // page-specific hit map; this keeps serial touch injection in sync
        // with the physical touch task as new pages are added.
        return x >= kUiInset && x < kUiInset + kUiContentWidth && y >= kUiTitleY &&
               y < kPortraitH;
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
        if (actionable) HandleHomeTap(x, y);
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
        if (tap) HandleHomeTap(x, y);
        send_touch_ack("up", x, y, dx, tap, false);
        serial_touch_down = false;
        serial_touch_start_us = 0;
    };

    ESP_LOGI(TAG, "serial input ready (FRAME?; TOUCH TAP/CLICK/DOWN/MOVE/UP)");
    while (!frame_dump_stop_) {
        if (fd < 0) {
            fd = ::open("/dev/secondary", O_WRONLY | O_NONBLOCK);
            if (fd < 0) {
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
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
                                case ProductPage::TodayList: page_name = "today_list"; break;
                                case ProductPage::CardBox: page_name = "card_box"; break;
                                case ProductPage::CardDetail: page_name = "card_detail"; break;
                                case ProductPage::Keep: page_name = "keep"; break;
                                case ProductPage::Apps: page_name = "apps"; break;
                                case ProductPage::Workbench: page_name = "workbench"; break;
                                case ProductPage::Settings: page_name = "settings"; break;
                                case ProductPage::Confirmation: page_name = "confirmation"; break;
                                case ProductPage::More: page_name = "more"; break;
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
                        SetPowerSaveMode(false);
                        send_touch_ack("back", -1, -1, 0, true, false);
                    } else if (strncasecmp(command, "TOUCH TAP ", 10) == 0 ||
                               strncasecmp(command, "TOUCH CLICK ", 12) == 0) {
                        int x = 0;
                        int y = 0;
                        char extra = '\0';
                        const size_t prefix = strncasecmp(command, "TOUCH TAP ", 10) == 0 ? 10U : 12U;
                        const int parsed = std::sscanf(command + prefix, "%d %d %c", &x, &y, &extra);
                        if (parsed != 2 || x < 0 || x >= kPortraitW || y < 0 || y >= kPortraitH) {
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
                        if (parsed != 2 || x < 0 || x >= kPortraitW || y < 0 || y >= kPortraitH) {
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
                            "PAPER_PAGE_TEST? | HOME?\n";
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
    if (text == nullptr || text[0] == '\0' || font.bitmap == nullptr || font.glyphs == nullptr ||
        font.codepoints == nullptr) return;
    int cursor = x;
    const char* p = text;
    while (*p != '\0') {
        const uint32_t codepoint = Utf8Next(&p);
        const ui_glyph_t* glyph = FindGlyph(font, codepoint);
        if (glyph == nullptr) {
            cursor += std::max(1, static_cast<int>(font.height / 2));
            continue;
        }
        const int stride = font.bits_per_pixel == 1
                               ? (static_cast<int>(glyph->width) + 7) / 8
                               : (static_cast<int>(glyph->width) + 3) / 4;
        for (int yy = 0; yy < font.height; ++yy) {
            const uint8_t* row = font.bitmap + glyph->offset + yy * stride;
            for (int xx = 0; xx < glyph->width; ++xx) {
                uint8_t level = 0;
                if (font.bits_per_pixel == 1) {
                    level = static_cast<uint8_t>((row[xx >> 3] >> (7 - (xx & 7))) & 0x01U);
                } else if (font.bits_per_pixel == 2) {
                    const uint8_t packed = row[xx >> 2];
                    level = static_cast<uint8_t>((packed >> (6 - 2 * (xx & 3))) & 0x03U);
                }
                if ((font.bits_per_pixel == 1 && level != 0U) ||
                    (font.bits_per_pixel == 2 && level >= 2U)) {
                    SetPixel(cursor + xx, y + yy, black);
                }
            }
        }
        cursor += static_cast<int>(glyph->width) + 1;
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
        const ui_glyph_t* glyph = FindGlyph(font, codepoint);
        width += glyph != nullptr ? static_cast<int>(glyph->width) + 1
                                  : std::max(1, static_cast<int>(font.height / 2));
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
    while (*p != '\0') {
        const char* start = p;
        const uint32_t codepoint = Utf8Next(&p);
        const ui_glyph_t* glyph = FindGlyph(font, codepoint);
        const int advance = glyph != nullptr ? static_cast<int>(glyph->width) + 1
                                             : std::max(1, static_cast<int>(font.height / 2));
        if (width + advance > max_width) {
            const char* ellipsis = "…";
            const int ellipsis_width = TextWidth(ellipsis, font);
            if (ellipsis_width > 0 && width + ellipsis_width <= max_width &&
                written + std::strlen(ellipsis) + 1 < out_size) {
                std::memcpy(out + written, ellipsis, std::strlen(ellipsis));
                written += std::strlen(ellipsis);
            }
            break;
        }
        const size_t bytes = static_cast<size_t>(p - start);
        if (written + bytes + 1 >= out_size) break;
        std::memcpy(out + written, start, bytes);
        written += bytes;
        width += advance;
    }
    out[written] = '\0';
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
        const ui_glyph_t* unit_glyph = FindGlyph(font, codepoint);
        const int advance = unit_glyph != nullptr
                                ? static_cast<int>(unit_glyph->width) + 1
                                : std::max(1, static_cast<int>(font.height / 2));
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
    DrawText(32, 756, "PAGE TURN 3L: masked white clear", ui_font_small);
    DrawText(32, 780, "VERSION advances with TEST_VERSION_NEXT?", ui_font_small);
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
    quick_note_state_ = 0;
    reader_page_ = 0;
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
    DrawText(70, kScheduleHeaderY - 2, "今日日程", ui_font_body);
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

void RawDisplay::DrawProductButtonLocked(int x, int y, int width, int height,
                                         const char* label, bool filled) {
    if (label == nullptr || label[0] == '\0' || width <= 0 || height <= 0) return;
    if (filled) FillRect(x, y, width, height, true);
    else StrokeRect(x, y, width, height, kUiCardStroke);

    const ui_font_t& font = TextWidth(label, ui_font_body) <= width - 24
                                ? ui_font_body
                                : ui_font_status;
    const int text_width = TextWidth(label, font);
    const int draw_x = x + std::max(12, (width - text_width) / 2);
    const int draw_y = y + std::max(0, (height - static_cast<int>(font.height)) / 2);
    DrawTextInk(draw_x, draw_y, label, font, !filled);
}

void RawDisplay::DrawProductStatusBarLocked(const char* section) {
    time_t now = time(nullptr);
    struct tm tmv{};
    localtime_r(&now, &tmv);

    char clock_text[8];
    std::snprintf(clock_text, sizeof(clock_text), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    DrawText(16, 5, clock_text, ui_font_status);

    const char* weekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
    char date_text[24];
    std::snprintf(date_text, sizeof(date_text), "%02d.%02d 周%s", tmv.tm_mon + 1,
                  tmv.tm_mday, weekdays[std::clamp(tmv.tm_wday, 0, 6)]);
    DrawText(112, 8, date_text, ui_font_small);

    char network_text[32];
    const dashboard::Snapshot snapshot = dashboard::DashboardData::GetInstance().GetSnapshot();
    CopyDisplayText(network_text, sizeof(network_text),
                    status_text_[0] != '\0' ? status_text_ : snapshot.network);
    char network_fit[20];
    FitText(network_text, ui_font_small, 96, network_fit, sizeof(network_fit));
    DrawText(205, 8, network_fit, ui_font_small);

    char section_fit[32];
    FitText(section != nullptr ? section : "首页", ui_font_small, 92,
            section_fit, sizeof(section_fit));
    DrawTextCentered(304, 8, 96, 24, section_fit, ui_font_small);
    DrawBattery(416, 9, battery_percent_, charging_);
    FillRect(kUiInset, kUiStatusHeight - 1, kUiContentWidth, 1, true);
}

void RawDisplay::DrawProductFooterLocked(const char* first, const char* second,
                                         const char* third, const char* fourth) {
    const char* labels[4] = {first, second, third, fourth};
    for (int i = 0; i < 4; ++i) {
        const int row = i / 2;
        const int column = i % 2;
        DrawProductButtonLocked(kUiFooterX[column], kUiFooterRowY[row],
                                kUiFooterButtonWidth, kUiFooterButtonHeight,
                                labels[i]);
    }
}

void RawDisplay::DrawProductHomeLocked() {
    std::memset(portrait_fb_, kWhite, portrait_size_);
    const dashboard::Snapshot snapshot = dashboard::DashboardData::GetInstance().GetSnapshot();
    last_dashboard_revision_ = snapshot.revision;

    if (power_save_) {
        const char* text = "休眠中";
        DrawText((kPortraitW - TextWidth(text, ui_font_title)) / 2, 350, text, ui_font_title);
        return;
    }

    const int64_t now_ms = esp_timer_get_time() / 1000;
    const bool notification_active = notification_text_[0] != '\0' &&
                                     notification_deadline_ms_ > now_ms;
    DrawProductStatusBarLocked(notification_active ? notification_text_ : "首页");

    char title[64];
    FitText("今天，先做重要的事", ui_font_title, kUiContentWidth, title, sizeof(title));
    DrawText(kUiInset, kUiTitleY + 2, title, ui_font_title);

    // Main focus card.  A black field makes the first action visually
    // dominant while retaining enough white margin for the paper-like grid.
    FillRect(kUiInset, kUiBodyY, kUiContentWidth, 248, true);
    DrawTextInk(kUiInset + 24, kUiBodyY + 20, "AI 今日重点", ui_font_status, false);
    FillRect(kUiInset + 24, kUiBodyY + 58, kUiContentWidth - 48, 1, false);
    const char* focus_source = snapshot.ai_count > 0 && snapshot.ai_summary[0][0] != '\0'
                                   ? snapshot.ai_summary[0]
                                   : "暂无重点，点击开始询问小智";
    char focus_line1[96];
    char focus_line2[96];
    FitTextLines(focus_source, ui_font_body, kUiContentWidth - 48,
                 focus_line1, sizeof(focus_line1), focus_line2, sizeof(focus_line2));
    auto draw_center_ink = [this](int x, int y, int width, const char* text,
                                  const ui_font_t& font) {
        if (text == nullptr || text[0] == '\0') return;
        const int text_width = TextWidth(text, font);
        DrawTextInk(x + std::max(0, (width - text_width) / 2), y, text, font, false);
    };
    draw_center_ink(kUiInset + 24, kUiBodyY + 84, kUiContentWidth - 48,
                    focus_line1, ui_font_body);
    if (focus_line2[0] != '\0') {
        draw_center_ink(kUiInset + 24, kUiBodyY + 126, kUiContentWidth - 48,
                        focus_line2, ui_font_body);
    }
    char focus_status[48];
    CopyDisplayText(focus_status, sizeof(focus_status), snapshot.ai_status);
    char focus_status_fit[48];
    FitText(focus_status, ui_font_small, kUiContentWidth - 48,
            focus_status_fit, sizeof(focus_status_fit));
    DrawTextInk(kUiInset + 24, kUiBodyY + 203, focus_status_fit, ui_font_small, false);

    // The following two cards are intentionally equal in height.  They are
    // separate targets so a tap never depends on the text length in the card.
    StrokeRect(kUiInset, 384, kUiContentWidth, 112, kUiCardStroke);
    DrawText(kUiInset + 16, 397, "下一件", ui_font_small);
    if (snapshot.schedule_count > 0) {
        DrawText(kUiInset + 16, 425, snapshot.schedule[0].time, ui_font_status);
        char next_title[56];
        FitText(snapshot.schedule[0].title, ui_font_body, 278, next_title, sizeof(next_title));
        DrawText(kUiInset + 118, 421, next_title, ui_font_body);
        char next_detail[72];
        FitText(snapshot.schedule[0].detail, ui_font_small, 300,
                next_detail, sizeof(next_detail));
        DrawText(kUiInset + 118, 462, next_detail, ui_font_small);
    } else {
        DrawText(kUiInset + 16, 430, "暂无安排", ui_font_body);
    }

    StrokeRect(kUiInset, 512, kUiContentWidth, 112, kUiCardStroke);
    DrawText(kUiInset + 16, 525, "继续阅读", ui_font_small);
    const dashboard::CustomCard& reading = snapshot.custom[1];
    char reading_title_source[40];
    CopyDisplayText(reading_title_source, sizeof(reading_title_source),
                    reading.enabled && reading.title[0] != '\0' ? reading.title : "打开书库");
    char reading_title[40];
    FitText(reading_title_source, ui_font_body, 260, reading_title, sizeof(reading_title));
    DrawText(kUiInset + 16, 552, reading_title, ui_font_body);
    char reading_value[40];
    CopyDisplayText(reading_value, sizeof(reading_value),
                    reading.enabled && reading.value[0] != '\0' ? reading.value : "暂无阅读记录");
    FitText(reading_value, ui_font_small, 260, reading_value, sizeof(reading_value));
    DrawText(kUiInset + 16, 590, reading_value, ui_font_small);
    DrawText(kUiInset + 336, 560, ">", ui_font_title);

    DrawProductFooterLocked("随手记", "留屏", "应用", "问 AI");
}

void RawDisplay::DrawProductAppsLocked() {
    std::memset(portrait_fb_, kWhite, portrait_size_);
    DrawProductStatusBarLocked("应用");
    DrawText(kUiInset, kUiTitleY + 2, "应用目录", ui_font_title);

    static constexpr const char* kNames[] = {"阅读", "AI 助手", "随手记", "卡片盒", "设置", "更多"};
    static constexpr const char* kDetails[] = {
        "从 SD 卡继续阅读", "提问、总结与整理", "录音并保存原文", "浏览已保存内容",
        "布局、网络与省电", "工作台和设备工具",
    };
    for (int i = 0; i < 6; ++i) {
        const int y = kUiBodyY + i * (kUiListRowHeight + kUiListRowGap);
        StrokeRect(kUiInset, y, kUiContentWidth, kUiListRowHeight, kUiCardStroke);
        char index[8];
        std::snprintf(index, sizeof(index), "%02d", i + 1);
        DrawText(kUiInset + 16, y + 20, index, ui_font_small);
        DrawText(kUiInset + 66, y + 12, kNames[i], ui_font_body);
        DrawText(kUiInset + 66, y + 43, kDetails[i], ui_font_small);
        DrawText(kUiInset + kUiContentWidth - 36, y + 14, ">", ui_font_title);
    }
    DrawProductFooterLocked("返回", "卡片盒", "设置", "更多");
}

void RawDisplay::DrawProductAiLocked(bool details) {
    std::memset(portrait_fb_, kWhite, portrait_size_);
    DrawProductStatusBarLocked(details ? "AI 详情" : "AI 结果");
    DrawText(kUiInset, kUiTitleY + 2, details ? "下一步" : "AI 结果", ui_font_title);

    const dashboard::Snapshot snapshot = dashboard::DashboardData::GetInstance().GetSnapshot();
    if (!details) {
        StrokeRect(kUiInset, kUiBodyY, kUiContentWidth, 248, kUiCardStroke);
        DrawText(kUiInset + 20, kUiBodyY + 18, "摘要", ui_font_small);
        const char* summary = snapshot.ai_count > 0 && snapshot.ai_summary[0][0] != '\0'
                                  ? snapshot.ai_summary[0]
                                  : "等待小智返回结果";
        char line1[96];
        char line2[96];
        FitTextLines(summary, ui_font_body, kUiContentWidth - 40,
                     line1, sizeof(line1), line2, sizeof(line2));
        DrawText(kUiInset + 20, kUiBodyY + 62, line1, ui_font_body);
        DrawText(kUiInset + 20, kUiBodyY + 104, line2, ui_font_body);
        DrawText(kUiInset + 20, kUiBodyY + 176, "结论", ui_font_small);
        const char* conclusion = snapshot.ai_count > 1 ? snapshot.ai_summary[1] : "点击下一步查看执行建议";
        char conclusion_fit[96];
        FitText(conclusion, ui_font_status, kUiContentWidth - 40,
                conclusion_fit, sizeof(conclusion_fit));
        DrawText(kUiInset + 20, kUiBodyY + 205, conclusion_fit, ui_font_status);
        DrawProductFooterLocked("存卡片", "留屏", "下一步", "返回");
    } else {
        static constexpr const char* kSteps[] = {
            "1 先完成当前最重要的一件事",
            "2 把结果写入卡片盒",
            "3 再安排下一次提醒",
        };
        for (int i = 0; i < 3; ++i) {
            const int y = kUiBodyY + i * 86;
            StrokeRect(kUiInset, y, kUiContentWidth, 72, kUiCardStroke);
            DrawText(kUiInset + 20, y + 20, kSteps[i], ui_font_body);
        }
        DrawText(kUiInset + 16, 390, "流式结果按完整段落更新屏幕", ui_font_small);
        DrawProductFooterLocked("返回结果", "存卡片", "留屏", "首页");
    }
}

void RawDisplay::DrawProductQuickNoteLocked() {
    std::memset(portrait_fb_, kWhite, portrait_size_);
    DrawProductStatusBarLocked("随手记");
    DrawText(kUiInset, kUiTitleY + 2, "随手记", ui_font_title);
    StrokeRect(kUiInset, kUiBodyY, kUiContentWidth, 300, kUiCardStroke);
    static constexpr const char* kStateNames[] = {"待开始", "录音中", "原文已保存", "AI 整理完成", "草稿待确认"};
    static constexpr const char* kStateDetails[] = {
        "按左下按钮开始录音", "再次点击结束录音并保存原文", "保存成功，等待 AI 整理",
        "请检查标题和内容", "确认后才会创建任务",
    };
    const uint8_t state = std::min<uint8_t>(quick_note_state_, 4);
    DrawText(kUiInset + 24, kUiBodyY + 28, kStateNames[state], ui_font_body);
    DrawText(kUiInset + 24, kUiBodyY + 84, kStateDetails[state], ui_font_status);
    DrawText(kUiInset + 24, kUiBodyY + 152, "今天的想法会先保存为原文", ui_font_small);
    if (state >= 3) {
        DrawText(kUiInset + 24, kUiBodyY + 198, "标题：整理今天的工作重点", ui_font_status);
        DrawText(kUiInset + 24, kUiBodyY + 238, "内容：完成后再交给小智安排下一步", ui_font_small);
    }
    DrawProductFooterLocked("开始录音", "AI 整理", "确认任务", "返回");
}

void RawDisplay::DrawProductReaderLocked() {
    std::memset(portrait_fb_, kWhite, portrait_size_);
    DrawProductStatusBarLocked("阅读");
    char page_text[24];
    std::snprintf(page_text, sizeof(page_text), "阅读  %u / 3",
                  static_cast<unsigned>(reader_page_ + 1));
    DrawText(kUiInset, kUiTitleY + 2, page_text, ui_font_title);
    StrokeRect(kUiInset, kUiBodyY, kUiContentWidth, kUiBodyHeight, kUiCardStroke);

    static constexpr const char* kLines[3][10] = {
        {"第一章  从一件小事开始", "", "今天只做一件重要的事。", "把目标写下来，再把它拆成", "可以马上完成的小步。", "", "完成一小步以后，停下来", "看看下一步是否仍然清楚。", "", ""},
        {"第二章  保持连续", "", "稳定的节奏比偶尔的冲刺", "更容易留下真正的进展。", "把注意力放回当前一页，", "让工具安静地服务于阅读。", "", "", "", ""},
        {"第三章  记录结果", "", "在一天结束以前记下结果，", "明天就不必从头寻找方向。", "一张卡片足够承接一个想法。", "", "", "", "", ""},
    };
    const uint8_t page = std::min<uint8_t>(reader_page_, 2);
    for (int i = 0; i < 10; ++i) {
        if (kLines[page][i][0] == '\0') continue;
        DrawText(kUiInset + 24, kUiBodyY + 22 + i * 42, kLines[page][i],
                 i == 0 ? ui_font_status : ui_font_body);
    }
    DrawProductFooterLocked("上一页", "下一页", "目录", "返回");
}

void RawDisplay::DrawProductTodayListLocked() {
    std::memset(portrait_fb_, kWhite, portrait_size_);
    DrawProductStatusBarLocked("今日清单");
    DrawText(kUiInset, kUiTitleY + 2, "今日清单", ui_font_title);
    const dashboard::Snapshot snapshot = dashboard::DashboardData::GetInstance().GetSnapshot();
    const int count = std::min<int>(snapshot.schedule_count, 3);
    for (int i = 0; i < count; ++i) {
        const int y = kUiBodyY + i * 76;
        StrokeRect(kUiInset, y, kUiContentWidth, 64, kUiCardStroke);
        DrawText(kUiInset + 18, y + 17, snapshot.schedule[i].time, ui_font_status);
        char title[56];
        FitText(snapshot.schedule[i].title, ui_font_body, 270, title, sizeof(title));
        DrawText(kUiInset + 112, y + 12, title, ui_font_body);
        DrawText(kUiInset + 112, y + 42, snapshot.schedule[i].done ? "已完成" : "待处理",
                 ui_font_small);
        if (snapshot.schedule[i].done) FillCircle(kUiInset + 414, y + 31, 7, true);
        else StrokeCircle(kUiInset + 414, y + 31, 7, 2);
    }
    if (count == 0) DrawText(kUiInset + 24, kUiBodyY + 24, "今天还没有安排", ui_font_body);
    DrawProductFooterLocked("返回", "完成", "卡片盒", "首页");
}

void RawDisplay::DrawProductCardBoxLocked() {
    std::memset(portrait_fb_, kWhite, portrait_size_);
    DrawProductStatusBarLocked("卡片盒");
    DrawText(kUiInset, kUiTitleY + 2, "卡片盒", ui_font_title);
    const dashboard::Snapshot snapshot = dashboard::DashboardData::GetInstance().GetSnapshot();
    int row = 0;
    for (size_t i = 0; i < dashboard::kCustomCardCount && row < 3; ++i) {
        if (!snapshot.custom[i].enabled) continue;
        const int y = kUiBodyY + row * 88;
        StrokeRect(kUiInset, y, kUiContentWidth, 76, kUiCardStroke);
        DrawText(kUiInset + 18, y + 10, snapshot.custom[i].title, ui_font_body);
        DrawText(kUiInset + 18, y + 46, snapshot.custom[i].value, ui_font_small);
        DrawText(kUiInset + 408, y + 16, ">", ui_font_title);
        ++row;
    }
    for (size_t i = 0; i < dashboard::kAiSummaryCount && row < 3; ++i) {
        if (snapshot.ai_summary[i][0] == '\0') continue;
        const int y = kUiBodyY + row * 88;
        StrokeRect(kUiInset, y, kUiContentWidth, 76, kUiCardStroke);
        DrawText(kUiInset + 18, y + 10, "AI 摘要", ui_font_small);
        char summary[80];
        FitText(snapshot.ai_summary[i], ui_font_status, 350, summary, sizeof(summary));
        DrawText(kUiInset + 18, y + 38, summary, ui_font_status);
        ++row;
    }
    if (row == 0) DrawText(kUiInset + 24, kUiBodyY + 24, "还没有保存的卡片", ui_font_body);
    DrawProductFooterLocked("返回", "打开", "留屏", "首页");
}

void RawDisplay::DrawProductCardDetailLocked() {
    std::memset(portrait_fb_, kWhite, portrait_size_);
    DrawProductStatusBarLocked("卡片详情");
    DrawText(kUiInset, kUiTitleY + 2, "卡片详情", ui_font_title);
    StrokeRect(kUiInset, kUiBodyY, kUiContentWidth, 360, kUiCardStroke);
    DrawText(kUiInset + 24, kUiBodyY + 24, "整理今天的工作重点", ui_font_body);
    DrawText(kUiInset + 24, kUiBodyY + 80, "来源：小智 AI 结果", ui_font_small);
    DrawText(kUiInset + 24, kUiBodyY + 126, "内容", ui_font_small);
    DrawText(kUiInset + 24, kUiBodyY + 166, "今天先完成最重要的一件事，", ui_font_status);
    DrawText(kUiInset + 24, kUiBodyY + 202, "完成后再安排下一步。", ui_font_status);
    DrawText(kUiInset + 24, kUiBodyY + 274, "创建时间：刚刚", ui_font_small);
    DrawProductFooterLocked("返回", "留屏", "编辑", "更多");
}

void RawDisplay::DrawProductKeepLocked() {
    std::memset(portrait_fb_, kWhite, portrait_size_);
    // Keep-screen deliberately has no status bar, battery, network or footer.
    DrawText(kUiInset, 56, "留屏", ui_font_title);
    FillRect(kUiInset, 112, kUiContentWidth, 1, true);
    DrawText(kUiInset + 24, 144, "整理今天的工作重点", ui_font_body);
    DrawText(kUiInset + 24, 204, "今天先完成最重要的一件事，", ui_font_status);
    DrawText(kUiInset + 24, 244, "完成后再安排下一步。", ui_font_status);
    DrawText(kUiInset + 24, 344, "来源：小智 AI 结果", ui_font_small);
    DrawText(kUiInset + 24, 380, "快照时间：进入留屏时", ui_font_small);
    DrawText(kUiInset + 24, 460, "设备将在唤醒后返回原页面", ui_font_small);
}

void RawDisplay::DrawProductWorkbenchLocked() {
    std::memset(portrait_fb_, kWhite, portrait_size_);
    DrawProductStatusBarLocked("工作台");
    DrawText(kUiInset, kUiTitleY + 2, "工作台", ui_font_title);
    static constexpr const char* kTools[] = {"同步天气与额度", "刷新 AI 摘要", "显示测试页", "查看设备状态"};
    for (int i = 0; i < 4; ++i) {
        const int y = kUiBodyY + i * 82;
        StrokeRect(kUiInset, y, kUiContentWidth, 68, kUiCardStroke);
        DrawText(kUiInset + 20, y + 18, kTools[i], ui_font_body);
        DrawText(kUiInset + 408, y + 13, ">", ui_font_title);
    }
    DrawProductFooterLocked("返回", "刷新", "设置", "首页");
}

void RawDisplay::DrawProductSettingsLocked() {
    std::memset(portrait_fb_, kWhite, portrait_size_);
    DrawProductStatusBarLocked("设置");
    DrawText(kUiInset, kUiTitleY + 2, "设置", ui_font_title);
    static constexpr const char* kItems[] = {"布局方向", "网络连接", "省电策略", "关于设备"};
    static constexpr const char* kValues[] = {"竖屏（默认）", "离线", "自动", "RAW SSD1677"};
    for (int i = 0; i < 4; ++i) {
        const int y = kUiBodyY + i * 82;
        StrokeRect(kUiInset, y, kUiContentWidth, 68, kUiCardStroke);
        DrawText(kUiInset + 20, y + 10, kItems[i], ui_font_body);
        DrawText(kUiInset + 20, y + 41, kValues[i], ui_font_small);
        DrawText(kUiInset + 408, y + 13, ">", ui_font_title);
    }
    DrawProductFooterLocked("返回", "切换方向", "工作台", "首页");
}

void RawDisplay::DrawProductConfirmationLocked() {
    std::memset(portrait_fb_, kWhite, portrait_size_);
    DrawProductStatusBarLocked("确认");
    DrawText(kUiInset, kUiTitleY + 2, "请确认", ui_font_title);
    StrokeRect(kUiInset, kUiBodyY, kUiContentWidth, 240, kUiCardStroke);
    DrawText(kUiInset + 24, kUiBodyY + 28, "任务尚未创建", ui_font_body);
    DrawText(kUiInset + 24, kUiBodyY + 92, "确认草稿后才会加入今日清单。", ui_font_status);
    DrawText(kUiInset + 24, kUiBodyY + 146, "保存原文和 AI 整理结果不会自动创建任务。", ui_font_small);
    DrawProductFooterLocked("返回编辑", "确认创建", "留屏", "首页");
}

void RawDisplay::DrawProductMoreLocked() {
    std::memset(portrait_fb_, kWhite, portrait_size_);
    DrawProductStatusBarLocked("更多");
    DrawText(kUiInset, kUiTitleY + 2, "更多", ui_font_title);
    static constexpr const char* kItems[] = {"工作台", "设备测试", "刷新数据", "关于本项目"};
    for (int i = 0; i < 4; ++i) {
        const int y = kUiBodyY + i * 82;
        StrokeRect(kUiInset, y, kUiContentWidth, 68, kUiCardStroke);
        DrawText(kUiInset + 20, y + 18, kItems[i], ui_font_body);
        DrawText(kUiInset + 408, y + 13, ">", ui_font_title);
    }
    DrawProductFooterLocked("返回", "工作台", "设置", "首页");
}

void RawDisplay::DrawProductScreenLocked() {
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
    return true;
}

void RawDisplay::FlushPartialLocked(int x, int y, int w, int h, bool incremental_du) {
    if (panel_region_fb_ == nullptr || !panel_history_valid_ || w <= 0 || h <= 0 ||
        (x & 7) != 0 || (w & 7) != 0 || x < 0 || y < 0 || x + w > kPanelW || y + h > kPanelH) {
        FlushLocked();
        return;
    }
    const bool animation_wait = animation_running_;
    if ((animation_wait ? epaper_panel_wait_busy_timeout(panel_, 3000)
                        : epaper_panel_wait_busy(panel_)) != ESP_OK) {
        if (animation_wait) animation_running_ = false;
        return;
    }
    const int stride = kPanelW / 8;
    const int row_bytes = w / 8;

    // Paper Mono seeds both SSD1677 RAM roles with the last committed frame
    // once, because a previous activation may have consumed/swapped them.
    const bool seed_baseline = !window_baseline_valid_;
    if (seed_baseline) {
        epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_CURRENT);
        if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_prev_fb_) != ESP_OK) return;
        epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_PREVIOUS);
        if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_prev_fb_) != ESP_OK) return;
        window_baseline_valid_ = true;
    }

    for (int row = 0; row < h; ++row) {
        std::memcpy(panel_region_fb_ + row * row_bytes,
                    panel_fb_ + (y + row) * stride + x / 8, row_bytes);
    }
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_CURRENT);
    if (esp_lcd_panel_draw_bitmap(panel_, x, y, x + w, y + h, panel_region_fb_) != ESP_OK) return;
    for (int row = 0; row < h; ++row) {
        std::memcpy(panel_region_fb_ + row * row_bytes,
                    panel_prev_fb_ + (y + row) * stride + x / 8, row_bytes);
    }
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_PREVIOUS);
    if (esp_lcd_panel_draw_bitmap(panel_, x, y, x + w, y + h, panel_region_fb_) != ESP_OK) return;
    epaper_panel_set_refresh_mode(
        panel_, incremental_du ? SSD1677_EPAPER_REFRESH_DU
                               : (seed_baseline ? SSD1677_EPAPER_REFRESH_PARTIAL
                                                : SSD1677_EPAPER_REFRESH_PARTIAL_WARM));
    if (epaper_panel_refresh_screen(panel_) != ESP_OK) {
        if (animation_wait) animation_running_ = false;
        return;
    }
    const esp_err_t wait_err = animation_wait ? epaper_panel_wait_refresh_timeout(panel_, 3000)
                                              : epaper_panel_wait_busy(panel_);
    if (wait_err != ESP_OK) {
        if (animation_wait) animation_running_ = false;
        ESP_LOGW(TAG, "partial refresh wait failed: %s", esp_err_to_name(wait_err));
        /* A failed experimental waveform must not poison the next button
         * press.  Reset/reinitialize the controller and force a known B/W
         * baseline before another differential window is attempted. */
        if (epaper_panel_recover(panel_) == ESP_OK) {
            panel_custom_waveform_active_ = false;
            panel_history_valid_ = false;
            window_baseline_valid_ = false;
            fast_refresh_count_ = 0;
        } else {
            ESP_LOGW(TAG, "partial refresh recovery failed");
        }
        return;
    }

    // Commit this rectangle and make both controller roles equal to the new
    // target before the next window, matching Paper Mono's displayWindow().
    for (int row = 0; row < h; ++row) {
        std::memcpy(panel_prev_fb_ + (y + row) * stride + x / 8,
                    panel_fb_ + (y + row) * stride + x / 8, row_bytes);
    }
    UpdateGlassBinaryLocked(x, y, w, h);
    for (int row = 0; row < h; ++row) {
        std::memcpy(panel_region_fb_ + row * row_bytes,
                    panel_fb_ + (y + row) * stride + x / 8, row_bytes);
    }
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_CURRENT);
    if (esp_lcd_panel_draw_bitmap(panel_, x, y, x + w, y + h,
                                  panel_region_fb_) != ESP_OK) return;
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_PREVIOUS);
    if (esp_lcd_panel_draw_bitmap(panel_, x, y, x + w, y + h,
                                  panel_region_fb_) != ESP_OK) return;
    window_baseline_valid_ = true;
}

void RawDisplay::FlushLocked() {
    if (portrait_fb_ == nullptr || panel_fb_ == nullptr || panel_prev_fb_ == nullptr) return;

    if (!RecoverPanelForBinaryLocked()) return;

    // EPD updates are asynchronous.  Drain the previous waveform before
    // touching either VRAM plane; otherwise draw_bitmap rejects the write
    // with ESP_ERR_NOT_FINISHED and stale data gets refreshed repeatedly.
    if (epaper_panel_wait_busy(panel_) != ESP_OK) {
        ESP_LOGW(TAG, "wait BUSY before refresh failed");
        panel_history_valid_ = false;
        window_baseline_valid_ = false;
        return;
    }

    for (int py = 0; py < kPortraitH; ++py) for (int px = 0; px < kPortraitW; ++px) {
        const bool white = (portrait_fb_[static_cast<size_t>(py) * (kPortraitW / 8) + (px >> 3)] & (0x80 >> (px & 7))) != 0;
        const int sx = py, sy = kPortraitW - 1 - px;
        uint8_t& b = panel_fb_[static_cast<size_t>(sy) * (kPanelW / 8) + (sx >> 3)];
        if (white) b |= static_cast<uint8_t>(0x80 >> (sx & 7)); else b &= static_cast<uint8_t>(~(0x80 >> (sx & 7)));
    }

    // Match EegoRead's unchanged-frame guard.  A redundant waveform is still
    // visible on e-paper (and needlessly consumes panel lifetime), even when
    // the rendered status bar did not change.
    if (panel_history_valid_ && std::memcmp(panel_prev_fb_, panel_fb_, panel_size_) == 0) {
        return;
    }

    // Keep the SSD1677 ordering from the validated bring-up image: current
    // plane first, then the auxiliary plane.  Unlike EegoRead's UC8279C,
    // SSD1677's full waveform bypasses RED, so do not reinterpret that plane
    // as a DTM1 register.
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_CURRENT);
    if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_fb_) != ESP_OK) {
        ESP_LOGW(TAG, "write current framebuffer failed");
        return;
    }
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_PREVIOUS);
    if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_prev_fb_) != ESP_OK) {
        ESP_LOGW(TAG, "write auxiliary framebuffer failed");
        return;
    }

    const bool gc = !panel_history_valid_ || fast_refresh_count_ >= kFullRefreshEvery;
    /*
     * SSD1677's OTP FULL_FAST (0xD7) is not a one-flash waveform: it runs
     * several white/black phases and is visibly worse than a full-screen
     * differential update.  Use the controller's PARTIAL waveform for the
     * normal fast path instead.  With PREVIOUS and CURRENT already loaded
     * for the whole frame, this is the same single-pulse strategy used by
     * the LVGL adapter.  Keep the occasional OTP FULL pass for ghost cleanup.
     */
    epaper_panel_set_refresh_mode(panel_, gc ? SSD1677_EPAPER_REFRESH_FULL
                                             : SSD1677_EPAPER_REFRESH_PARTIAL);
    if (epaper_panel_refresh_screen(panel_) != ESP_OK) {
        ESP_LOGW(TAG, "EPD refresh start failed");
        panel_history_valid_ = false;
        window_baseline_valid_ = false;
        return;
    }
    // Keep the software and controller histories in lockstep, and do not
    // start a second waveform while BUSY is asserted.
    if (epaper_panel_wait_refresh_timeout(panel_, 15000) != ESP_OK) {
        ESP_LOGW(TAG, "wait BUSY after refresh failed");
        panel_history_valid_ = false;
        window_baseline_valid_ = false;
        return;
    }
    std::memcpy(panel_prev_fb_, panel_fb_, panel_size_);
    UpdateGlassBinaryLocked(0, 0, kPanelW, kPanelH);
    panel_history_valid_ = true;
    window_baseline_valid_ = false;
    fast_refresh_count_ = gc ? 0 : fast_refresh_count_ + 1;
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
        !notification_expired) return;
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
        if (power_save_ == on) return;
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
