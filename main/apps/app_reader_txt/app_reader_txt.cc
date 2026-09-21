#include "app_reader_txt.h"

#include "fontpack_lvgl.h"
#include "hal/common/SdCardManager.hpp"
#include "hal/common/sd_paths.h"
#include "haptic_feedback.h"
#include "lv_adapter_display.h"
#include "reader_text.h"
#include "screen_common.h"
#include "vk_key_handler.h"

#include <dirent.h>
#include <esp_log.h>
#include <lvgl.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace {

constexpr const char* TAG = "AppReaderTxt";
constexpr const char* kScreenId = "reader_txt";
constexpr size_t kMaxBooks = 24;
constexpr size_t kMaxName = 96;
constexpr size_t kMaxPath = 256;
constexpr size_t kMaxFileBytes = 1024 * 1024;
constexpr size_t kMaxHistory = 128;
// A consistent safe inset keeps content clear of the panel's rounded corners.
constexpr lv_coord_t kBodyPad = 24;
constexpr lv_coord_t kCardRadius = 10;

struct Book {
    char path[kMaxPath]{};
    char name[kMaxName]{};
};

Book s_books[kMaxBooks];
size_t s_book_count = 0;
char s_selected_path[kMaxPath]{};
char* s_text = nullptr;
size_t s_text_size = 0;
size_t s_text_offset = 0;
size_t s_history[kMaxHistory]{};
size_t s_history_count = 0;
lv_obj_t* s_scr = nullptr;
lv_obj_t* s_body = nullptr;
lv_obj_t* s_content = nullptr;
lv_obj_t* s_status = nullptr;
lv_obj_t* s_progress = nullptr;
lv_coord_t s_content_height = 0;
bool s_reader_mode = false;
reader_layout_page_t s_page{};

const lv_font_t* UiFont() {
    const lv_font_t* font = fontpack_lv_font_ui();
    return font != nullptr ? font : LV_FONT_DEFAULT;
}

const lv_font_t* SmallFont() {
    const lv_font_t* font = fontpack_lv_font_get(18, 2);
    return font != nullptr ? font : UiFont();
}

const lv_font_t* TitleFont() {
    const lv_font_t* font = fontpack_lv_font_get(30, 2);
    return font != nullptr ? font : UiFont();
}

void FreeText() {
    free(s_text);
    s_text = nullptr;
    s_text_size = 0;
    s_text_offset = 0;
    s_history_count = 0;
}

bool HasTxtExtension(const char* name) {
    if (name == nullptr) return false;
    const char* dot = std::strrchr(name, '.');
    if (dot == nullptr) return false;
    return strcasecmp(dot, ".txt") == 0;
}

void ScanDirectory(const char* dir_path) {
    DIR* dir = opendir(dir_path);
    if (dir == nullptr) return;
    while (s_book_count < kMaxBooks) {
        struct dirent* ent = readdir(dir);
        if (ent == nullptr) break;
        if (!HasTxtExtension(ent->d_name)) continue;
        const size_t dir_len = std::strlen(dir_path);
        const size_t name_len = std::strlen(ent->d_name);
        if (dir_len + 1U + name_len >= kMaxPath) continue;
        Book& book = s_books[s_book_count];
        std::memcpy(book.path, dir_path, dir_len);
        book.path[dir_len] = '/';
        std::memcpy(book.path + dir_len + 1U, ent->d_name, name_len + 1U);
        const size_t display_len = std::min(name_len, kMaxName - 1U);
        std::memcpy(book.name, ent->d_name, display_len);
        book.name[display_len] = '\0';
        ++s_book_count;
    }
    closedir(dir);
}

void SetStatus(const char* text) {
    if (s_status != nullptr && lv_obj_is_valid(s_status) && text != nullptr) {
        lv_label_set_text(s_status, text);
    }
}

int GlyphAdvance(uint32_t codepoint, void* /*context*/) {
    lv_font_glyph_dsc_t dsc{};
    const lv_font_t* font = UiFont();
    if (font != nullptr && lv_font_get_glyph_dsc(font, &dsc, codepoint, 0)) {
        return dsc.adv_w;
    }
    return static_cast<int>(font != nullptr ? font->line_height / 2 : 16);
}

reader_layout_config_t MakeLayoutConfig() {
    reader_layout_config_t config{};
    config.viewport_width_px = static_cast<uint16_t>(LV_HOR_RES - kBodyPad * 2);
    config.viewport_height_px = static_cast<uint16_t>(s_content_height > 0 ? s_content_height : LV_VER_RES - 180);
    config.line_height_px = static_cast<uint16_t>(UiFont()->line_height + 6);
    config.paragraph_spacing_px = 8;
    config.indent_width_px = static_cast<uint16_t>(UiFont()->line_height * 2);
    config.fallback_advance_px = static_cast<uint16_t>(UiFont()->line_height / 2);
    const uint16_t usable_height = config.viewport_height_px > 8
                                       ? config.viewport_height_px - 8
                                       : config.viewport_height_px;
    const uint16_t line_height = config.line_height_px == 0 ? 1 : config.line_height_px;
    const uint16_t fit_lines = usable_height / line_height;
    config.max_lines = static_cast<uint8_t>(std::max<uint16_t>(1, std::min<uint16_t>(READER_LAYOUT_MAX_LINES, fit_lines)));
    config.kinsoku = true;
    config.indent_mode = READER_INDENT_NORMALIZE_EXISTING;
    config.glyph_advance = GlyphAdvance;
    return config;
}

bool StartsParagraphAtOffset() {
    return s_text_offset == 0 ||
           (s_text_offset <= s_text_size && s_text[s_text_offset - 1] == '\n');
}

bool LoadTextFile(const char* path) {
    FreeText();
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return false;
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return false;
    }
    long length = std::ftell(file);
    if (length <= 0 || static_cast<size_t>(length) > kMaxFileBytes) {
        std::fclose(file);
        return false;
    }
    std::rewind(file);
    uint8_t* raw = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(length)));
    if (raw == nullptr || std::fread(raw, 1, static_cast<size_t>(length), file) != static_cast<size_t>(length)) {
        free(raw);
        std::fclose(file);
        return false;
    }
    std::fclose(file);

    const reader_encoding_t encoding = reader_text_detect_encoding(raw, static_cast<size_t>(length));
    // Valid UTF-8 stays the same size; malformed bytes may become U+FFFD (3 bytes).
    const size_t out_cap = encoding == READER_ENCODING_UTF8
                               ? static_cast<size_t>(length) * 3U + 1U
                               : static_cast<size_t>(length) * 2U + 1U;
    char* decoded = static_cast<char*>(std::malloc(out_cap));
    if (decoded == nullptr) {
        free(raw);
        return false;
    }
    size_t in_pos = 0;
    size_t out_pos = 0;
    while (in_pos < static_cast<size_t>(length) && out_pos + 4 < out_cap) {
        uint32_t cp = 0;
        size_t consumed = 0;
        const int ok = reader_text_decode_one(encoding, raw + in_pos,
                                              static_cast<size_t>(length) - in_pos,
                                              &cp, &consumed);
        if (ok == 0 || consumed == 0) break;
        char utf8[4];
        const size_t n = reader_text_encode_utf8(cp, utf8);
        if (n == 0 || out_pos + n >= out_cap) break;
        std::memcpy(decoded + out_pos, utf8, n);
        out_pos += n;
        in_pos += consumed;
    }
    decoded[out_pos] = '\0';
    free(raw);
    s_text = decoded;
    s_text_size = out_pos;
    ESP_LOGI(TAG, "loaded TXT %s (%u bytes, encoding=%d, utf8=%u)", path,
             static_cast<unsigned>(length), static_cast<int>(encoding),
             static_cast<unsigned>(s_text_size));
    return s_text_size != 0;
}

void RenderPage() {
    if (!s_reader_mode || s_content == nullptr || !lv_obj_is_valid(s_content) || s_text == nullptr) return;
    const reader_layout_config_t config = MakeLayoutConfig();
    const bool ok = reader_layout_page(s_text + s_text_offset, s_text_size - s_text_offset,
                                       StartsParagraphAtOffset(), &config, &s_page);
    if (!ok || s_page.line_count == 0) {
        lv_label_set_text(s_content, s_text_offset == 0 ? "文本为空" : "已到末尾");
        SetStatus("TXT 阅读");
        return;
    }
    char* page_text = static_cast<char*>(std::malloc(s_page.line_count * READER_LAYOUT_MAX_LINE_BYTES + 1));
    if (page_text == nullptr) return;
    size_t out = 0;
    for (uint8_t i = 0; i < s_page.line_count; ++i) {
        const size_t n = std::strlen(s_page.lines[i].utf8);
        if (out + n + 2 >= s_page.line_count * READER_LAYOUT_MAX_LINE_BYTES + 1) break;
        std::memcpy(page_text + out, s_page.lines[i].utf8, n);
        out += n;
        if (i + 1 < s_page.line_count) page_text[out++] = '\n';
    }
    page_text[out] = '\0';
    lv_label_set_text(s_content, page_text);
    free(page_text);
    const size_t consumed = s_page.source_bytes_consumed;
    char status[96];
    std::snprintf(status, sizeof(status), "已读 %u%%",
                  static_cast<unsigned>(s_text_size == 0 ? 100 : (s_text_offset * 100U) / s_text_size));
    SetStatus(status);
    if (s_progress != nullptr && lv_obj_is_valid(s_progress)) {
        const lv_coord_t track_w = LV_HOR_RES - kBodyPad * 2;
        const lv_coord_t progress_w = s_text_size == 0
                                           ? 0
                                           : static_cast<lv_coord_t>((s_text_offset * track_w) / s_text_size);
        lv_obj_set_width(s_progress, progress_w > 2 ? progress_w : 2);
    }
    if (consumed == 0) {
        s_text_offset = s_text_size;
    }
}

void NextPage() {
    if (!s_reader_mode || s_text == nullptr) return;
    const reader_layout_config_t config = MakeLayoutConfig();
    if (!reader_layout_page(s_text + s_text_offset, s_text_size - s_text_offset,
                            StartsParagraphAtOffset(), &config, &s_page) || s_page.source_bytes_consumed == 0) {
        SetStatus("已到末尾");
        return;
    }
    if (s_history_count < kMaxHistory) s_history[s_history_count++] = s_text_offset;
    s_text_offset += s_page.source_bytes_consumed;
    if (s_text_offset > s_text_size) s_text_offset = s_text_size;
    RenderPage();
}

void PrevPage() {
    if (!s_reader_mode || s_history_count == 0) return;
    s_text_offset = s_history[--s_history_count];
    RenderPage();
}

bool OnKey(const char* key_name) {
    if (key_name == nullptr || !s_reader_mode) return false;
    if (std::strcmp(key_name, "vk_next") == 0) {
        NextPage();
        return true;
    }
    if (std::strcmp(key_name, "vk_prev") == 0 && s_history_count != 0) {
        PrevPage();
        return true;
    }
    return false;
}

void OnReaderTap(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_point_t p{};
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_indev_get_point(indev, &p);
    if (p.x > LV_HOR_RES / 2) NextPage(); else PrevPage();
    HapticPulseIfEnabled();
}

void OpenSelectedAsync(void* /*unused*/) {
    ScreenNavigateTo(AppReaderTxt::CreateReader);
}

void OnBookClicked(lv_event_t* e) {
    auto* book = static_cast<Book*>(lv_event_get_user_data(e));
    if (book == nullptr) return;
    std::snprintf(s_selected_path, sizeof(s_selected_path), "%s", book->path);
    lv_async_call(OpenSelectedAsync, nullptr);
}

void OnDeleted(lv_event_t* e) {
    if (lv_event_get_target(e) != s_scr) return;
    FreeText();
    s_scr = nullptr;
    s_body = nullptr;
    s_content = nullptr;
    s_status = nullptr;
    s_progress = nullptr;
    s_content_height = 0;
    s_reader_mode = false;
}

lv_obj_t* MakeRow(lv_obj_t* parent, const char* title, lv_event_cb_t cb, void* user_data,
                  size_t index) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 80);
    lv_obj_set_style_bg_color(row, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(row, lv_color_black(), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    HapticAttachClick(row);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t* marker = lv_obj_create(row);
    lv_obj_remove_style_all(marker);
    lv_obj_set_size(marker, 30, 30);
    lv_obj_set_style_bg_color(marker, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(marker, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(marker, kCardRadius, 0);
    lv_obj_align(marker, LV_ALIGN_LEFT_MID, 0, -4);
    lv_obj_clear_flag(marker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(marker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* number = lv_label_create(marker);
    char number_text[16];
    std::snprintf(number_text, sizeof(number_text), "%02u", static_cast<unsigned>(index + 1));
    lv_label_set_text(number, number_text);
    lv_obj_set_style_text_font(number, SmallFont(), 0);
    lv_obj_set_style_text_color(number, lv_color_white(), 0);
    lv_obj_center(number);
    lv_obj_t* label = lv_label_create(row);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_font(label, UiFont(), 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_width(label, LV_HOR_RES - kBodyPad * 2 - 78);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 44, 13);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* meta = lv_label_create(row);
    lv_label_set_text(meta, "TXT  /  本地文件");
    lv_obj_set_style_text_font(meta, SmallFont(), 0);
    lv_obj_set_style_text_color(meta, lv_color_black(), 0);
    lv_obj_align(meta, LV_ALIGN_BOTTOM_LEFT, 44, -13);
    lv_obj_clear_flag(meta, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* arrow = lv_label_create(row);
    lv_label_set_text(arrow, ">");
    lv_obj_set_style_text_font(arrow, UiFont(), 0);
    lv_obj_set_style_text_color(arrow, lv_color_black(), 0);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -2, -3);
    lv_obj_clear_flag(arrow, LV_OBJ_FLAG_CLICKABLE);
    return row;
}

}  // namespace

lv_obj_t* AppReaderTxt::Create() {
    FreeText();
    s_reader_mode = false;
    s_book_count = 0;
    s_selected_path[0] = '\0';
    ScreenSetIsHome(false);
    SdCardManager::GetInstance().Mount();
    ScanDirectory(SD_APP_ROOT);
    ScanDirectory(SD_MOUNT_POINT);

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_scr = scr;
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, UiFont(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    lv_obj_add_event_cb(scr, OnDeleted, LV_EVENT_DELETE, nullptr);
    EpdStatusBar bar = ScreenCreateStatusBar(scr);
    s_body = lv_obj_create(scr);
    lv_obj_remove_style_all(s_body);
    lv_obj_set_size(s_body, LV_HOR_RES, LV_VER_RES - bar.height - 44);
    lv_obj_align(s_body, LV_ALIGN_TOP_MID, 0, bar.height);
    lv_obj_set_style_pad_all(s_body, kBodyPad, 0);
    lv_obj_set_style_pad_row(s_body, 0, 0);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_body, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t* intro = lv_obj_create(s_body);
    lv_obj_remove_style_all(intro);
    lv_obj_set_width(intro, lv_pct(100));
    lv_obj_set_height(intro, 140);
    lv_obj_clear_flag(intro, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(intro, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* intro_overline = lv_label_create(intro);
    lv_label_set_text(intro_overline, "LOCAL / TXT");
    lv_obj_set_style_text_font(intro_overline, SmallFont(), 0);
    lv_obj_set_style_text_letter_space(intro_overline, 2, 0);
    lv_obj_align(intro_overline, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t* intro_title = lv_label_create(intro);
    lv_label_set_text(intro_title, "把时间，留给阅读。");
    lv_obj_set_style_text_font(intro_title, TitleFont(), 0);
    lv_obj_set_style_text_color(intro_title, lv_color_black(), 0);
    lv_obj_set_width(intro_title, lv_pct(100));
    lv_obj_align(intro_title, LV_ALIGN_TOP_LEFT, 0, 38);
    lv_obj_t* intro_hint = lv_label_create(intro);
    char book_count[64];
    std::snprintf(book_count, sizeof(book_count), "%u 本书  /  轻点书名打开", static_cast<unsigned>(s_book_count));
    lv_label_set_text(intro_hint, book_count);
    lv_obj_set_style_text_font(intro_hint, SmallFont(), 0);
    lv_obj_set_style_text_color(intro_hint, lv_color_black(), 0);
    lv_obj_align(intro_hint, LV_ALIGN_BOTTOM_LEFT, 0, -16);
    lv_obj_t* rule = lv_obj_create(intro);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, LV_HOR_RES - kBodyPad * 2, 2);
    lv_obj_set_style_bg_color(rule, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_align(rule, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    if (s_book_count == 0) {
        lv_obj_t* empty = lv_label_create(s_body);
        lv_label_set_text(empty, "书架还空着\n\n将 TXT 文件放入 SD 卡的\nmetalio/e-ink/ 或根目录，\n再回来开始第一段阅读。");
        lv_obj_set_style_text_font(empty, UiFont(), 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_margin_top(empty, 36, 0);
        lv_obj_set_style_text_line_space(empty, 8, 0);
        lv_obj_set_width(empty, lv_pct(100));
    } else {
        for (size_t i = 0; i < s_book_count; ++i) MakeRow(s_body, s_books[i].name, OnBookClicked, &s_books[i], i);
    }
    lv_obj_t* footer = lv_label_create(scr);
    lv_label_set_text(footer, "离线阅读  /  只保留文字与留白");
    lv_obj_set_style_text_font(footer, SmallFont(), 0);
    lv_obj_set_style_text_color(footer, lv_color_black(), 0);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_LEFT, kBodyPad, -16);
    VkKey_AttachScreen(scr, kScreenId, VkKeyScreenDesc{AppReaderTxt::Create});
    lv_obj_invalidate(scr);
    if (auto* display = LVAdapterDisplay::Instance()) {
        display->RequestNextFullRefresh();
    }
    return scr;
}

lv_obj_t* AppReaderTxt::CreateReader() {
    s_reader_mode = true;
    ScreenSetIsHome(false);
    const bool loaded = s_selected_path[0] != '\0' && LoadTextFile(s_selected_path);
    lv_obj_t* scr = lv_obj_create(nullptr);
    s_scr = scr;
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, UiFont(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    lv_obj_add_event_cb(scr, OnDeleted, LV_EVENT_DELETE, nullptr);
    EpdStatusBar bar = ScreenCreateStatusBar(scr);
    lv_obj_t* overline = lv_label_create(scr);
    lv_label_set_text(overline, "NOW READING  /  TXT");
    lv_obj_set_style_text_font(overline, SmallFont(), 0);
    lv_obj_set_style_text_letter_space(overline, 1, 0);
    lv_obj_align(overline, LV_ALIGN_TOP_LEFT, kBodyPad, bar.height + 2);
    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, std::strrchr(s_selected_path, '/') != nullptr ? std::strrchr(s_selected_path, '/') + 1 : s_selected_path);
    lv_obj_set_style_text_font(title, UiFont(), 0);
    lv_obj_set_width(title, LV_HOR_RES - kBodyPad * 2);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, kBodyPad, bar.height + 28);
    lv_obj_t* top_rule = lv_obj_create(scr);
    lv_obj_remove_style_all(top_rule);
    lv_obj_set_size(top_rule, LV_HOR_RES - kBodyPad * 2, 2);
    lv_obj_set_style_bg_color(top_rule, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(top_rule, LV_OPA_COVER, 0);
    lv_obj_align(top_rule, LV_ALIGN_TOP_MID, 0, bar.height + 66);
    lv_obj_t* side_rule = lv_obj_create(scr);
    lv_obj_remove_style_all(side_rule);
    lv_obj_set_size(side_rule, 3, LV_VER_RES - bar.height - 150);
    lv_obj_set_style_bg_color(side_rule, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(side_rule, LV_OPA_COVER, 0);
    lv_obj_align(side_rule, LV_ALIGN_TOP_LEFT, kBodyPad - 10, bar.height + 86);
    s_content = lv_label_create(scr);
    lv_obj_set_width(s_content, LV_HOR_RES - kBodyPad * 2);
    s_content_height = LV_VER_RES - bar.height - 150;
    lv_obj_set_height(s_content, s_content_height);
    lv_obj_align(s_content, LV_ALIGN_TOP_LEFT, kBodyPad, bar.height + 86);
    lv_obj_set_style_text_font(s_content, UiFont(), 0);
    lv_obj_set_style_text_color(s_content, lv_color_black(), 0);
    lv_obj_set_style_text_line_space(s_content, 8, 0);
    lv_label_set_long_mode(s_content, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(s_content, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_content, OnReaderTap, LV_EVENT_CLICKED, nullptr);
    s_status = lv_label_create(scr);
    lv_obj_set_width(s_status, LV_HOR_RES / 2);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_LEFT, kBodyPad, -30);
    lv_obj_set_style_text_font(s_status, SmallFont(), 0);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_t* nav_hint = lv_label_create(scr);
    lv_label_set_text(nav_hint, "左翻  /  右翻");
    lv_obj_set_style_text_font(nav_hint, SmallFont(), 0);
    lv_obj_set_style_text_align(nav_hint, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(nav_hint, LV_HOR_RES / 2 - kBodyPad);
    lv_obj_align(nav_hint, LV_ALIGN_BOTTOM_RIGHT, -kBodyPad, -30);
    lv_obj_t* progress_track = lv_obj_create(scr);
    lv_obj_remove_style_all(progress_track);
    lv_obj_set_size(progress_track, LV_HOR_RES - kBodyPad * 2, 6);
    lv_obj_set_style_border_color(progress_track, lv_color_black(), 0);
    lv_obj_set_style_border_width(progress_track, 1, 0);
    lv_obj_set_style_radius(progress_track, 3, 0);
    lv_obj_align(progress_track, LV_ALIGN_BOTTOM_MID, 0, -12);
    s_progress = lv_obj_create(scr);
    lv_obj_remove_style_all(s_progress);
    lv_obj_set_size(s_progress, 2, 5);
    lv_obj_set_style_bg_color(s_progress, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_progress, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_progress, 3, 0);
    lv_obj_set_parent(s_progress, progress_track);
    lv_obj_align(s_progress, LV_ALIGN_LEFT_MID, 0, 0);
    if (loaded) {
        RenderPage();
    } else {
        lv_label_set_text(s_content, "TXT 打开失败\n仅支持最大 1 MB 的文本文件");
        SetStatus("读取失败");
    }
    VkKey_AttachScreen(scr, kScreenId, VkKeyScreenDesc{AppReaderTxt::Create, OnKey});
    lv_obj_invalidate(scr);
    if (auto* display = LVAdapterDisplay::Instance()) {
        display->RequestNextFullRefresh();
    }
    return scr;
}
