#include "reader_text.h"
#include "cp950_sparse_table.h"

#include <ctype.h>
#include <string.h>

/* FatFS 0.15 in ESP-IDF 6 exposes this converter from ffunicode.c. Keep the
 * declaration local so host tests can provide a tiny deterministic stub. */
extern uint16_t ff_oem2uni(uint16_t oem, uint16_t codepage);

enum {
    REPLACEMENT_CHARACTER = 0xfffd,
    IDEOGRAPHIC_SPACE = 0x3000,
    MAX_LAYOUT_UNITS = READER_LAYOUT_MAX_LINE_BYTES / 2,
    LEGACY_DETECT_BYTES = 8192,
};

typedef struct {
    uint32_t codepoint;
    size_t source_start;
    size_t source_bytes;
    uint16_t advance_px;
    uint8_t utf8_length;
    uint8_t style_flags;
    char utf8[4];
} layout_unit_t;

static bool utf8_sequence_valid(const uint8_t *data, size_t size,
                                size_t *sequence_bytes, uint32_t *codepoint)
{
    if (size == 0) {
        return false;
    }
    const uint8_t first = data[0];
    if (first < 0x80) {
        *sequence_bytes = 1;
        *codepoint = first;
        return true;
    }
    size_t needed;
    uint32_t value;
    uint32_t minimum;
    if (first >= 0xc2 && first <= 0xdf) {
        needed = 2;
        value = first & 0x1f;
        minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
        needed = 3;
        value = first & 0x0f;
        minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
        needed = 4;
        value = first & 0x07;
        minimum = 0x10000;
    } else {
        return false;
    }
    if (size < needed) {
        *sequence_bytes = 0;
        return false;
    }
    for (size_t index = 1; index < needed; ++index) {
        if ((data[index] & 0xc0) != 0x80) {
            return false;
        }
        value = (value << 6) | (data[index] & 0x3f);
    }
    if (value < minimum || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff)) {
        return false;
    }
    *sequence_bytes = needed;
    *codepoint = value;
    return true;
}

static uint16_t cp950_lookup(uint16_t encoded)
{
    size_t left = 0;
    size_t right = reader_cp950_table_count;
    while (left < right) {
        const size_t middle = left + (right - left) / 2U;
        const uint16_t candidate = reader_cp950_table[middle].encoded;
        if (candidate < encoded) {
            left = middle + 1U;
        } else {
            right = middle;
        }
    }
    return left < reader_cp950_table_count &&
                   reader_cp950_table[left].encoded == encoded
               ? reader_cp950_table[left].unicode
               : 0;
}

static bool legacy_trail_valid(reader_encoding_t encoding, uint8_t trail)
{
    if (encoding == READER_ENCODING_CP950) {
        return (trail >= 0x40U && trail <= 0x7eU) ||
               (trail >= 0xa1U && trail <= 0xfeU);
    }
    return trail >= 0x40U && trail <= 0xfeU && trail != 0x7fU;
}

static uint16_t legacy_lookup(reader_encoding_t encoding, uint8_t lead,
                              uint8_t trail)
{
    const uint16_t encoded = (uint16_t)(((uint16_t)lead << 8) | trail);
    return encoding == READER_ENCODING_CP950 ? cp950_lookup(encoded)
                                              : ff_oem2uni(encoded, 936);
}

static int legacy_codepoint_score(uint32_t codepoint)
{
    if (codepoint == REPLACEMENT_CHARACTER) {
        return -12;
    }
    if (codepoint == '\t' || codepoint == '\r' || codepoint == '\n' ||
        (codepoint >= 0x20U && codepoint <= 0x7eU)) {
        return 0;
    }
    if (codepoint < 0x20U || (codepoint >= 0x7fU && codepoint <= 0x9fU)) {
        return -4;
    }
    if (codepoint >= 0x3400U && codepoint <= 0x9fffU) {
        return 4;
    }
    if ((codepoint >= 0x3000U && codepoint <= 0x303fU) ||
        (codepoint >= 0xfe10U && codepoint <= 0xfe6fU) ||
        (codepoint >= 0xff00U && codepoint <= 0xffefU)) {
        return 2;
    }
    return 1;
}

static int score_legacy_encoding(reader_encoding_t encoding,
                                 const uint8_t *data, size_t size,
                                 size_t *valid_double_bytes)
{
    int score = 0;
    size_t doubles = 0;
    for (size_t offset = 0; offset < size;) {
        const uint8_t first = data[offset];
        if (first < 0x80U) {
            score += legacy_codepoint_score(first);
            ++offset;
            continue;
        }
        if (first < 0x81U || first > 0xfeU || offset + 1U >= size ||
            !legacy_trail_valid(encoding, data[offset + 1U])) {
            score += legacy_codepoint_score(REPLACEMENT_CHARACTER);
            ++offset;
            continue;
        }
        const uint16_t mapped =
            legacy_lookup(encoding, first, data[offset + 1U]);
        if (mapped == 0) {
            score += legacy_codepoint_score(REPLACEMENT_CHARACTER);
            ++offset;
            continue;
        }
        score += legacy_codepoint_score(mapped);
        ++doubles;
        offset += 2U;
    }
    if (valid_double_bytes != NULL) {
        *valid_double_bytes = doubles;
    }
    return score;
}

reader_encoding_t reader_text_detect_encoding(const uint8_t *data, size_t size)
{
    if (data == NULL || size == 0) {
        return READER_ENCODING_UTF8;
    }
    if (size >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf) {
        return READER_ENCODING_UTF8;
    }
    if (size >= 2 && data[0] == 0xff && data[1] == 0xfe) {
        return READER_ENCODING_UTF16_LE;
    }
    if (size >= 2 && data[0] == 0xfe && data[1] == 0xff) {
        return READER_ENCODING_UTF16_BE;
    }

    bool saw_multibyte = false;
    for (size_t offset = 0; offset < size;) {
        if (data[offset] < 0x80) {
            ++offset;
            continue;
        }
        size_t consumed = 0;
        uint32_t codepoint = 0;
        if (!utf8_sequence_valid(data + offset, size - offset, &consumed,
                                 &codepoint)) {
            const uint8_t first = data[offset];
            const size_t expected = first >= 0xc2 && first <= 0xdf
                                        ? 2U
                                    : first >= 0xe0 && first <= 0xef
                                        ? 3U
                                    : first >= 0xf0 && first <= 0xf4
                                        ? 4U
                                        : 0U;
            /* A partial tail is normal only when the probe genuinely ends
             * before the sequence's required byte count. */
            if (expected != 0 && size - offset < expected) {
                break;
            }
            const size_t sample_size = size > LEGACY_DETECT_BYTES
                                           ? LEGACY_DETECT_BYTES
                                           : size;
            size_t cp950_double_bytes = 0;
            const int gbk_score = score_legacy_encoding(
                READER_ENCODING_GBK, data, sample_size, NULL);
            const int cp950_score = score_legacy_encoding(
                READER_ENCODING_CP950, data, sample_size,
                &cp950_double_bytes);
            return cp950_double_bytes >= 4U &&
                           cp950_score >= gbk_score + 12
                       ? READER_ENCODING_CP950
                       : READER_ENCODING_GBK;
        }
        (void)codepoint;
        saw_multibyte = true;
        offset += consumed;
    }
    (void)saw_multibyte;
    return READER_ENCODING_UTF8;
}

size_t reader_text_encode_utf8(uint32_t codepoint, char out[4])
{
    if (codepoint <= 0x7f) {
        out[0] = (char)codepoint;
        return 1;
    }
    if (codepoint <= 0x7ff) {
        out[0] = (char)(0xc0 | (codepoint >> 6));
        out[1] = (char)(0x80 | (codepoint & 0x3f));
        return 2;
    }
    if (codepoint <= 0xffff) {
        out[0] = (char)(0xe0 | (codepoint >> 12));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        out[2] = (char)(0x80 | (codepoint & 0x3f));
        return 3;
    }
    if (codepoint <= 0x10ffff) {
        out[0] = (char)(0xf0 | (codepoint >> 18));
        out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3f));
        out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        out[3] = (char)(0x80 | (codepoint & 0x3f));
        return 4;
    }
    return reader_text_encode_utf8(REPLACEMENT_CHARACTER, out);
}

static int decode_utf16(const uint8_t *data, size_t size, bool little_endian,
                        uint32_t *codepoint, size_t *source_bytes)
{
    if (size < 2) {
        return 0;
    }
    const uint16_t first = little_endian
                               ? (uint16_t)(data[0] | ((uint16_t)data[1] << 8))
                               : (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
    if (first >= 0xd800 && first <= 0xdbff) {
        if (size < 4) {
            return 0;
        }
        const uint16_t second = little_endian
                                    ? (uint16_t)(data[2] | ((uint16_t)data[3] << 8))
                                    : (uint16_t)(((uint16_t)data[2] << 8) | data[3]);
        if (second >= 0xdc00 && second <= 0xdfff) {
            *codepoint = 0x10000 + (((uint32_t)first - 0xd800) << 10) +
                         ((uint32_t)second - 0xdc00);
            *source_bytes = 4;
            return 1;
        }
        *codepoint = REPLACEMENT_CHARACTER;
        *source_bytes = 2;
        return 1;
    }
    *codepoint = (first >= 0xdc00 && first <= 0xdfff)
                     ? REPLACEMENT_CHARACTER
                     : first;
    *source_bytes = 2;
    return 1;
}

int reader_text_decode_one(reader_encoding_t encoding, const uint8_t *data,
                           size_t size, uint32_t *codepoint,
                           size_t *source_bytes)
{
    if (data == NULL || codepoint == NULL || source_bytes == NULL || size == 0) {
        return 0;
    }
    *source_bytes = 0;
    if (encoding == READER_ENCODING_UTF16_LE ||
        encoding == READER_ENCODING_UTF16_BE) {
        return decode_utf16(data, size, encoding == READER_ENCODING_UTF16_LE,
                            codepoint, source_bytes);
    }
    if ((encoding == READER_ENCODING_GBK ||
         encoding == READER_ENCODING_CP950) &&
        data[0] >= 0x80U) {
        if (size < 2) {
            return 0;
        }
        const uint8_t trail = data[1];
        if (data[0] < 0x81U || data[0] > 0xfeU ||
            !legacy_trail_valid(encoding, trail)) {
            *codepoint = REPLACEMENT_CHARACTER;
            *source_bytes = 1;
            return 1;
        }
        const uint16_t mapped = legacy_lookup(encoding, data[0], trail);
        *codepoint = mapped != 0 ? mapped : REPLACEMENT_CHARACTER;
        *source_bytes = mapped != 0 ? 2U : 1U;
        return 1;
    }

    size_t consumed = 0;
    uint32_t decoded = 0;
    if (utf8_sequence_valid(data, size, &consumed, &decoded)) {
        *codepoint = decoded;
        *source_bytes = consumed;
        return 1;
    }
    if (consumed == 0 && data[0] >= 0xc2 && data[0] <= 0xf4) {
        const size_t expected = data[0] < 0xe0 ? 2 : data[0] < 0xf0 ? 3 : 4;
        if (size < expected) {
            return 0;
        }
    }
    *codepoint = REPLACEMENT_CHARACTER;
    *source_bytes = 1;
    return 1;
}

static bool utf8_prefix(const char *text, size_t size, const char *literal)
{
    const size_t literal_size = strlen(literal);
    return size >= literal_size && memcmp(text, literal, literal_size) == 0;
}

static size_t trim_left_utf8_space(const char *text, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        const unsigned char ch = (unsigned char)text[offset];
        if (ch == ' ' || ch == '\t') {
            ++offset;
        } else if (offset + 3 <= size &&
                   (unsigned char)text[offset] == 0xe3 &&
                   (unsigned char)text[offset + 1] == 0x80 &&
                   (unsigned char)text[offset + 2] == 0x80) {
            offset += 3;
        } else {
            break;
        }
    }
    return offset;
}

static size_t numeral_utf8_bytes(const char *text, size_t size)
{
    if (size == 0) {
        return 0;
    }
    if (text[0] >= '0' && text[0] <= '9') {
        return 1;
    }
    if (size >= 3 && (unsigned char)text[0] == 0xef &&
        (unsigned char)text[1] == 0xbc &&
        (unsigned char)text[2] >= 0x90 && (unsigned char)text[2] <= 0x99) {
        return 3;
    }
    static const char *const numerals[] = {
        "\xe9\x9b\xb6", "\xe3\x80\x87", "\xe4\xb8\x80", "\xe4\xba\x8c",
        "\xe4\xb8\x89", "\xe5\x9b\x9b", "\xe4\xba\x94", "\xe5\x85\xad",
        "\xe4\xb8\x83", "\xe5\x85\xab", "\xe4\xb9\x9d", "\xe5\x8d\x81",
        "\xe7\x99\xbe", "\xe5\x8d\x83", "\xe4\xb8\x87", "\xe4\xb8\xa4",
    };
    for (size_t index = 0; index < sizeof(numerals) / sizeof(numerals[0]); ++index) {
        if (size >= 3 && memcmp(text, numerals[index], 3) == 0) {
            return 3;
        }
    }
    return 0;
}

bool reader_text_is_chapter_heading(const char *utf8_line, size_t length)
{
    if (utf8_line == NULL) {
        return false;
    }
    size_t offset = trim_left_utf8_space(utf8_line, length);
    const char *text = utf8_line + offset;
    length -= offset;
    if (utf8_prefix(text, length, "Chapter ") || utf8_prefix(text, length, "CHAPTER ") ||
        utf8_prefix(text, length, "chapter ")) {
        const size_t chapter_length = 8;
        return length > chapter_length && text[chapter_length] >= '0' &&
               text[chapter_length] <= '9';
    }
    if (!utf8_prefix(text, length, "\xe7\xac\xac")) { /* U+7B2C */
        return false;
    }
    offset = 3;
    uint8_t numeral_count = 0;
    while (offset < length && numeral_count < 12) {
        const size_t bytes = numeral_utf8_bytes(text + offset, length - offset);
        if (bytes == 0) {
            break;
        }
        offset += bytes;
        ++numeral_count;
    }
    if (numeral_count == 0 || offset + 3 > length) {
        return false;
    }
    static const char *const units[] = {
        "\xe7\xab\xa0", "\xe5\x8d\xb7", "\xe8\x8a\x82", "\xe5\x9b\x9e",
        "\xe9\x9b\x86", "\xe9\x83\xa8", "\xe7\xaf\x87",
    };
    for (size_t index = 0; index < sizeof(units) / sizeof(units[0]); ++index) {
        if (memcmp(text + offset, units[index], 3) == 0) {
            return true;
        }
    }
    return false;
}

static bool line_has_leading_indent(const char *utf8_line, size_t length)
{
    return trim_left_utf8_space(utf8_line, length) > 0;
}

static uint16_t line_codepoint_count(const char *utf8_line, size_t length)
{
    size_t offset = trim_left_utf8_space(utf8_line, length);
    uint16_t count = 0;
    while (offset < length) {
        uint32_t codepoint = 0;
        size_t source_bytes = 0;
        if (reader_text_decode_one(READER_ENCODING_UTF8,
                                   (const uint8_t *)utf8_line + offset,
                                   length - offset, &codepoint,
                                   &source_bytes) != 1 ||
            source_bytes == 0) {
            break;
        }
        if (count != UINT16_MAX) {
            ++count;
        }
        offset += source_bytes;
    }
    return count;
}

void reader_indent_analysis_reset(reader_indent_analysis_t *analysis)
{
    if (analysis != NULL) {
        memset(analysis, 0, sizeof(*analysis));
    }
}

void reader_indent_analysis_observe(reader_indent_analysis_t *analysis,
                                    const char *utf8_line, size_t length,
                                    bool follows_blank_line)
{
    if (analysis == NULL || utf8_line == NULL || length == 0) {
        return;
    }
    if (trim_left_utf8_space(utf8_line, length) == length) {
        return;
    }
    if (analysis->paragraphs_seen != UINT16_MAX) {
        ++analysis->paragraphs_seen;
    }
    if (line_has_leading_indent(utf8_line, length) &&
        analysis->paragraphs_indented != UINT16_MAX) {
        ++analysis->paragraphs_indented;
    }
    if (follows_blank_line && analysis->paragraphs_blank_separated != UINT16_MAX) {
        ++analysis->paragraphs_blank_separated;
    }
    const uint16_t codepoints = line_codepoint_count(utf8_line, length);
    if (codepoints >= 24 && analysis->paragraphs_long != UINT16_MAX) {
        ++analysis->paragraphs_long;
    } else if (codepoints <= 12 && analysis->paragraphs_short != UINT16_MAX) {
        ++analysis->paragraphs_short;
    }
}

reader_indent_mode_t reader_indent_analysis_mode(const reader_indent_analysis_t *analysis)
{
    if (analysis == NULL || analysis->paragraphs_seen < 3) {
        return READER_INDENT_NONE;
    }
    /* A consistent source convention wins. This prevents adding indent to
     * poetry, code blocks, and books intentionally formatted flush-left. */
    if ((uint32_t)analysis->paragraphs_indented * 100u >=
        (uint32_t)analysis->paragraphs_seen * 55u) {
        /* A mostly indented book gets a uniform first-line indent. Existing
         * source spaces are consumed, and missing ones are supplied by the
         * layout stage so paragraph edges remain visually consistent. */
        return READER_INDENT_FORCE_TWO_EM;
    }
    /* Long, flush-left physical lines are the common form of plain-text
     * fiction. Short-line material is kept flush-left so poetry, lyrics and
     * source-like text are not mistaken for prose. */
    if ((uint32_t)analysis->paragraphs_short * 100u >=
            (uint32_t)analysis->paragraphs_seen * 65u &&
        (uint32_t)analysis->paragraphs_long * 100u <
            (uint32_t)analysis->paragraphs_seen * 20u) {
        return READER_INDENT_NONE;
    }
    if (analysis->paragraphs_long >= 3 &&
        ((uint32_t)analysis->paragraphs_long * 100u >=
             (uint32_t)analysis->paragraphs_seen * 30u ||
         (uint32_t)analysis->paragraphs_blank_separated * 100u >=
             (uint32_t)analysis->paragraphs_seen * 35u)) {
        return READER_INDENT_FORCE_TWO_EM;
    }
    return READER_INDENT_NONE;
}

static bool codepoint_is_source_space(uint32_t codepoint)
{
    return codepoint == ' ' || codepoint == '\t' || codepoint == IDEOGRAPHIC_SPACE;
}

static bool codepoint_is_ascii_word(uint32_t codepoint)
{
    return (codepoint >= 'A' && codepoint <= 'Z') ||
           (codepoint >= 'a' && codepoint <= 'z');
}

static bool codepoint_forbidden_line_start(uint32_t codepoint)
{
    switch (codepoint) {
    case ',': case '.': case '!': case '?': case ':': case ';': case ')':
    case ']': case '}': case '>': case '%': case '\'': case '"':
    case 0x00b0: case 0x00b7: case 0x2010: case 0x2013: case 0x2014:
    case 0x2015: case 0x2019: case 0x201d: case 0x2025: case 0x2026:
    case 0x2030: case 0x2032: case 0x2033: case 0x3001: case 0x3002:
    case 0x3009: case 0x300b: case 0x300d: case 0x300f: case 0x3011:
    case 0x3015: case 0x3017: case 0x3019: case 0x301b: case 0x301e:
    case 0x301f: case 0x30fb: case 0xff01: case 0xff09: case 0xff0c:
    case 0xff0e: case 0xff1a: case 0xff1b: case 0xff1f: case 0xff3d:
    case 0xff5d: case 0xff5e: case 0xff63:
        return true;
    default:
        return false;
    }
}

static bool codepoint_forbidden_line_end(uint32_t codepoint)
{
    switch (codepoint) {
    case '$': case '(': case '[': case '{': case '<': case '\'': case '"':
    case 0x00a3: case 0x00a5: case 0x20ac:
    case 0x2018: case 0x201c: case 0x3008: case 0x300a: case 0x300c:
    case 0x300e: case 0x3010: case 0x3014: case 0x3016: case 0x3018:
    case 0x301a: case 0x301d: case 0xff08: case 0xff3b: case 0xff5b:
    case 0xff62:
        return true;
    default:
        return false;
    }
}

static uint16_t glyph_advance(const reader_layout_config_t *config,
                              uint32_t codepoint, uint8_t style_flags)
{
    int advance = config->styled_glyph_advance != NULL
                      ? config->styled_glyph_advance(
                            codepoint, style_flags, config->glyph_context)
                  : config->glyph_advance != NULL
                      ? config->glyph_advance(codepoint,
                                              config->glyph_context)
                      : 0;
    if (advance <= 0) {
        advance = config->fallback_advance_px;
    }
    if (advance < 1) {
        advance = 1;
    }
    return (uint16_t)advance;
}

static void line_recalculate(const layout_unit_t *units, size_t count,
                             uint16_t initial_x, uint16_t *width,
                             size_t *output_bytes)
{
    uint32_t new_width = initial_x;
    size_t new_bytes = 0;
    for (size_t index = 0; index < count; ++index) {
        new_width += units[index].advance_px;
        new_bytes += units[index].utf8_length;
    }
    *width = new_width > UINT16_MAX ? UINT16_MAX : (uint16_t)new_width;
    *output_bytes = new_bytes;
}

bool reader_text_is_justify_gap(uint32_t left_codepoint,
                                 uint32_t right_codepoint)
{
    if (codepoint_is_source_space(left_codepoint) ||
        codepoint_is_source_space(right_codepoint)) {
        return !codepoint_is_source_space(right_codepoint);
    }
    return !(codepoint_is_ascii_word(left_codepoint) &&
             codepoint_is_ascii_word(right_codepoint));
}

static uint16_t count_stretch_gaps(const layout_unit_t *units, size_t count)
{
    uint16_t gaps = 0;
    for (size_t index = 0; index + 1 < count; ++index) {
        if (reader_text_is_justify_gap(units[index].codepoint,
                                       units[index + 1].codepoint) &&
            gaps != UINT16_MAX) {
            ++gaps;
        }
    }
    return gaps;
}

static void copy_units_to_line(reader_layout_line_t *line,
                               const layout_unit_t *units, size_t count,
                               size_t output_bytes)
{
    size_t offset = 0;
    for (size_t index = 0; index < count && offset < output_bytes; ++index) {
        if (line->style_run_count == 0 ||
            line->style_runs[line->style_run_count - 1U].style_flags !=
                units[index].style_flags) {
            if (line->style_run_count < READER_LAYOUT_MAX_STYLE_RUNS) {
                reader_layout_style_run_t *const run =
                    &line->style_runs[line->style_run_count++];
                run->byte_start = (uint16_t)offset;
                run->byte_end = (uint16_t)offset;
                run->style_flags = units[index].style_flags;
            }
        }
        memcpy(line->utf8 + offset, units[index].utf8, units[index].utf8_length);
        offset += units[index].utf8_length;
        if (line->style_run_count != 0) {
            line->style_runs[line->style_run_count - 1U].byte_end =
                (uint16_t)offset;
        }
    }
    line->utf8[offset] = '\0';
}

static bool style_marker(uint32_t codepoint, uint8_t *style_flags)
{
    if (style_flags == NULL) {
        return false;
    }
    switch (codepoint) {
    case READER_TEXT_MARK_HEADING_START:
        *style_flags |= READER_TEXT_STYLE_HEADING;
        return true;
    case READER_TEXT_MARK_HEADING_END:
        *style_flags &= (uint8_t)~READER_TEXT_STYLE_HEADING;
        return true;
    case READER_TEXT_MARK_BOLD_START:
        *style_flags |= READER_TEXT_STYLE_BOLD;
        return true;
    case READER_TEXT_MARK_BOLD_END:
        *style_flags &= (uint8_t)~READER_TEXT_STYLE_BOLD;
        return true;
    default:
        return false;
    }
}

static bool consume_initial_indent(const char *utf8, size_t size, size_t *position,
                                   const reader_layout_config_t *config,
                                   uint16_t *initial_x)
{
    const size_t original = *position;
    size_t scan = original;
    bool saw_space = false;
    while (scan < size) {
        uint32_t codepoint = 0;
        size_t source_bytes = 0;
        if (reader_text_decode_one(READER_ENCODING_UTF8,
                                   (const uint8_t *)utf8 + scan, size - scan,
                                   &codepoint, &source_bytes) != 1 ||
            !codepoint_is_source_space(codepoint)) {
            break;
        }
        saw_space = true;
        scan += source_bytes;
    }
    const bool apply = config->indent_mode == READER_INDENT_FORCE_TWO_EM ||
                       (config->indent_mode == READER_INDENT_NORMALIZE_EXISTING && saw_space);
    if (!apply) {
        return false;
    }
    *position = saw_space ? scan : original;
    *initial_x = config->indent_width_px;
    return *initial_x > 0;
}

bool reader_layout_page(const char *utf8, size_t size, bool starts_paragraph,
                        const reader_layout_config_t *config,
                        reader_layout_page_t *page)
{
    if (utf8 == NULL || config == NULL || page == NULL ||
        config->viewport_width_px == 0) {
        return false;
    }
    memset(page, 0, sizeof(*page));
    size_t position = 0;
    uint32_t used_height_px = 0;
    bool stopped_by_capacity = false;
    bool paragraph_start = starts_paragraph;
    uint8_t style_flags = config->initial_style_flags;
    const uint8_t max_lines =
        config->max_lines == 0 || config->max_lines > READER_LAYOUT_MAX_LINES
            ? READER_LAYOUT_MAX_LINES
            : config->max_lines;

    if (size >= 3 && (uint8_t)utf8[0] == 0xef && (uint8_t)utf8[1] == 0xbb &&
        (uint8_t)utf8[2] == 0xbf) {
        position = 3;
    }

    while (position < size && page->line_count < max_lines) {
        reader_layout_line_t *line = &page->lines[page->line_count];
        layout_unit_t units[MAX_LAYOUT_UNITS];
        const size_t source_start = position;
        const uint8_t style_before_line = style_flags;
        size_t unit_count = 0;
        size_t output_bytes = 0;
        uint16_t initial_x = 0;
        uint16_t width = 0;
        bool paragraph_end = false;
        bool wrapped = false;
        bool hyphenated = false;

        if (paragraph_start &&
            (style_flags & READER_TEXT_STYLE_HEADING) == 0) {
            (void)consume_initial_indent(utf8, size, &position, config, &initial_x);
        }
        width = initial_x;

        while (position < size) {
            const uint8_t raw = (uint8_t)utf8[position];
            if (raw == '\r') {
                ++position;
                continue;
            }
            if (raw == '\n') {
                ++position;
                paragraph_end = true;
                break;
            }

            uint32_t codepoint = 0;
            size_t source_bytes = 0;
            const int decoded = reader_text_decode_one(READER_ENCODING_UTF8,
                                                        (const uint8_t *)utf8 + position,
                                                        size - position,
                                                        &codepoint, &source_bytes);
            if (decoded == 0) {
                /* Preserve a partial UTF-8 tail for the next file read. */
                break;
            }
            if (style_marker(codepoint, &style_flags)) {
                position += source_bytes;
                continue;
            }
            char encoded[4];
            const size_t encoded_bytes = reader_text_encode_utf8(codepoint, encoded);
            const uint16_t advance =
                glyph_advance(config, codepoint, style_flags);
            const bool over_bytes = output_bytes + encoded_bytes >= READER_LAYOUT_MAX_LINE_BYTES;
            const bool over_width = (uint32_t)width + advance > config->viewport_width_px;

            if ((over_bytes || over_width) && unit_count > 0) {
                const size_t original_unit_count = unit_count;
                size_t keep = unit_count;
                /* Keep one source unit on whitespace-only lines. Besides
                 * guaranteeing forward progress, this keeps keep - 1 valid
                 * for the rewind calculation below. */
                while (keep > 1 &&
                       codepoint_is_source_space(units[keep - 1].codepoint)) {
                    --keep;
                }
                if (config->kinsoku) {
                    bool carry_start_cluster =
                        codepoint_forbidden_line_start(codepoint);
                    /* Do not split a repeated dash/ellipsis cluster. Move
                     * trailing no-start punctuation plus one leading glyph
                     * to the next line as a unit. */
                    while (keep > 1 &&
                           codepoint_forbidden_line_start(
                               units[keep - 1].codepoint)) {
                        --keep;
                        carry_start_cluster = true;
                    }
                    if (carry_start_cluster && keep > 1) {
                        --keep;
                    }
                }
                while (config->kinsoku && keep > 1 &&
                       codepoint_forbidden_line_end(units[keep - 1].codepoint)) {
                    --keep;
                }
                if (config->english_hyphenation && keep > 1 &&
                    codepoint_is_ascii_word(codepoint) &&
                    codepoint_is_ascii_word(units[keep - 1].codepoint)) {
                    const uint16_t hyphen_width =
                        glyph_advance(config, '-', style_flags);
                    uint16_t test_width = 0;
                    size_t ignored_bytes = 0;
                    line_recalculate(units, keep, initial_x, &test_width, &ignored_bytes);
                    while (keep > 1 && codepoint_is_ascii_word(units[keep - 1].codepoint) &&
                           (uint32_t)test_width + hyphen_width > config->viewport_width_px) {
                        --keep;
                        line_recalculate(units, keep, initial_x, &test_width, &ignored_bytes);
                    }
                    if (keep > 0 && codepoint_is_ascii_word(units[keep - 1].codepoint) &&
                        keep < MAX_LAYOUT_UNITS &&
                        (uint32_t)test_width + hyphen_width <= config->viewport_width_px) {
                        layout_unit_t *hyphen = &units[keep++];
                        memset(hyphen, 0, sizeof(*hyphen));
                        hyphen->codepoint = '-';
                        hyphen->source_start = position;
                        hyphen->advance_px = hyphen_width;
                        hyphen->style_flags = style_flags;
                        hyphen->utf8[0] = '-';
                        hyphen->utf8_length = 1;
                        hyphenated = true;
                    }
                }
                if (keep != unit_count || hyphenated) {
                    unit_count = keep;
                    size_t kept_source_units = keep;
                    if (hyphenated) {
                        kept_source_units = keep - 1U;
                    }
                    position = units[kept_source_units - 1U].source_start +
                               units[kept_source_units - 1U].source_bytes;
                    /* Kinsoku/hyphenation may rewind over invisible style
                     * markers. Restore the style active after the final
                     * retained source glyph so the next line can replay any
                     * following markers from the rewound source position. */
                    if (kept_source_units < original_unit_count ||
                        hyphenated) {
                        style_flags =
                            units[kept_source_units - 1U].style_flags;
                    }
                    line_recalculate(units, unit_count, initial_x, &width, &output_bytes);
                }
                wrapped = true;
                break;
            }

            if (unit_count == 0 && (over_bytes || over_width)) {
                /* An oversized glyph still has to make progress. */
            }
            if (unit_count >= MAX_LAYOUT_UNITS ||
                output_bytes + encoded_bytes >= READER_LAYOUT_MAX_LINE_BYTES) {
                wrapped = true;
                break;
            }
            layout_unit_t *unit = &units[unit_count++];
            unit->codepoint = codepoint;
            unit->source_start = position;
            unit->source_bytes = source_bytes;
            unit->advance_px = advance;
            unit->utf8_length = (uint8_t)encoded_bytes;
            unit->style_flags = style_flags;
            memcpy(unit->utf8, encoded, encoded_bytes);
            output_bytes += encoded_bytes;
            width = (uint16_t)((uint32_t)width + advance > UINT16_MAX
                                   ? UINT16_MAX
                                   : (uint32_t)width + advance);
            position += source_bytes;
        }

        if (unit_count == 0 && !paragraph_end && position == source_start) {
            break;
        }
        memset(line, 0, sizeof(*line));
        line->source_start = unit_count != 0 ? units[0].source_start
                                             : source_start;
        line->source_end = position;
        line->width_px = width;
        line->initial_x_px = initial_x;
        line->y_offset_px = used_height_px > UINT16_MAX
                                ? UINT16_MAX
                                : (uint16_t)used_height_px;
        if (wrapped) {
            line->flags |= READER_LAYOUT_LINE_WRAPPED;
        }
        if (paragraph_end) {
            line->flags |= READER_LAYOUT_LINE_PARAGRAPH_END;
        }
        if (hyphenated) {
            line->flags |= READER_LAYOUT_LINE_HYPHENATED;
        }
        if (initial_x != 0) {
            line->flags |= READER_LAYOUT_LINE_INDENTED;
        }
        copy_units_to_line(line, units, unit_count, output_bytes);
        bool line_has_heading = false;
        bool line_has_image = false;
        for (size_t index = 0; index < unit_count; ++index) {
            line_has_heading = line_has_heading ||
                               (units[index].style_flags &
                                READER_TEXT_STYLE_HEADING) != 0;
            line_has_image = line_has_image ||
                             units[index].codepoint == 0xfffcU;
        }
        uint16_t effective_line_height = config->line_height_px;
        if (line_has_heading && config->heading_line_height_px != 0) {
            effective_line_height = config->heading_line_height_px;
        }
        if (line_has_image && config->image_block_height_px != 0) {
            effective_line_height = config->image_block_height_px;
        }
        if (effective_line_height == 0) {
            effective_line_height = 1;
        }
        if (page->line_count != 0 && config->viewport_height_px != 0 &&
            used_height_px + effective_line_height >
                config->viewport_height_px) {
            position = source_start;
            style_flags = style_before_line;
            memset(line, 0, sizeof(*line));
            stopped_by_capacity = true;
            break;
        }
        line->line_height_px = effective_line_height;
        if (config->justify && wrapped && !paragraph_end && unit_count > 1 &&
            width < config->viewport_width_px) {
            line->stretch_gaps = count_stretch_gaps(units, unit_count);
            if (line->stretch_gaps != 0) {
                line->justify_extra_px = (uint16_t)(config->viewport_width_px - width);
            }
        }
        ++page->line_count;
        if (effective_line_height != 0) {
            used_height_px += effective_line_height;
            if (paragraph_end) {
                used_height_px += config->paragraph_spacing_px;
            }
        }
        paragraph_start = paragraph_end;
        if (page->line_count >= max_lines && position < size) {
            stopped_by_capacity = true;
        }
    }

    const bool buffered_content_remaining = position < size;
    const bool source_content_remaining =
        config->layout_metadata_valid
            ? (buffered_content_remaining ||
               (config->continuation && !config->chapter_end))
            : buffered_content_remaining;

    if (config->vertical_justify && page->line_count != 0 &&
        config->viewport_height_px != 0 && config->line_height_px != 0) {
        const reader_layout_line_t *last =
            &page->lines[page->line_count - 1U];
        const uint32_t last_bottom =
            (uint32_t)last->y_offset_px +
            (last->line_height_px != 0 ? last->line_height_px
                                       : config->line_height_px);
        int32_t visual_last_bottom = (int32_t)last_bottom;
        if (config->layout_metadata_valid && config->last_line_ink_bottom_px > 0) {
            visual_last_bottom = (int32_t)last->y_offset_px +
                                 config->last_line_ink_bottom_px;
        }
        uint32_t target_last_bottom = config->viewport_height_px;
        if (config->layout_metadata_valid &&
            config->first_line_ink_top_px > 0) {
            const uint32_t optical_top =
                (uint32_t)config->first_line_ink_top_px;
            if (optical_top < target_last_bottom) {
                /* Keep the first line's real ink inset mirrored at the
                 * bottom instead of pinning the last glyph to the viewport
                 * edge. */
                target_last_bottom -= optical_top;
            }
        }
        if ((uint32_t)visual_last_bottom < target_last_bottom) {
            const uint32_t extra = target_last_bottom -
                                   (uint32_t)visual_last_bottom;
            const bool metadata = config->layout_metadata_valid;
            const bool has_next = stopped_by_capacity &&
                                  source_content_remaining;
            if (has_next && page->line_count > 1) {
                /* A continuing page keeps its first line fixed and spreads
                 * spare space between the existing line intervals. */
                const uint32_t intervals = page->line_count - 1U;
                for (uint8_t index = 1; index < page->line_count; ++index) {
                    const uint32_t offset = (extra * index) / intervals;
                    const uint32_t adjusted =
                        (uint32_t)page->lines[index].y_offset_px + offset;
                    page->lines[index].y_offset_px =
                        adjusted > UINT16_MAX ? UINT16_MAX : (uint16_t)adjusted;
                }
            } else if (!metadata && position >= size) {
                /* The final page has no following line to consume the spare
                 * viewport. Center the complete line block instead, using a
                 * ceiling so an odd pixel is kept at the bottom margin. */
                const uint32_t offset = (extra + 1U) / 2U;
                for (uint8_t index = 0; index < page->line_count; ++index) {
                    const uint32_t adjusted =
                        (uint32_t)page->lines[index].y_offset_px + offset;
                    page->lines[index].y_offset_px =
                        adjusted > UINT16_MAX ? UINT16_MAX : (uint16_t)adjusted;
                }
            }
        }
    }

    page->source_bytes_consumed = position;
    page->next_starts_paragraph = paragraph_start;
    page->filled_by_capacity = config->layout_metadata_valid
                                   ? stopped_by_capacity
                                   : (position < size);
    page->chapter_end = config->layout_metadata_valid
                            ? (config->chapter_end &&
                               !buffered_content_remaining)
                            : !buffered_content_remaining;
    page->next_style_flags = style_flags;
    page->next_anchor_valid = source_content_remaining;
    page->first_line_ink_top_px = config->first_line_ink_top_px;
    page->last_line_ink_bottom_px = config->last_line_ink_bottom_px;
    return page->line_count != 0;
}
