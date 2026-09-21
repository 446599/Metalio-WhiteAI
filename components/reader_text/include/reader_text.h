#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    READER_ENCODING_UNKNOWN = 0,
    READER_ENCODING_UTF8,
    READER_ENCODING_UTF16_LE,
    READER_ENCODING_UTF16_BE,
    READER_ENCODING_GBK,
    READER_ENCODING_CP950,
} reader_encoding_t;

/* Detects UTF BOMs first, then validates UTF-8. Legacy input is scored as
 * GBK/CP936 and CP950 over at most the first 8192 bytes; ambiguous input keeps
 * the historical GBK preference. */
reader_encoding_t reader_text_detect_encoding(const uint8_t *data, size_t size);

/* Decodes one character without allocating. A malformed sequence becomes
 * U+FFFD and advances by one byte/unit. 0 means that the caller needs more
 * source bytes to finish a character. */
int reader_text_decode_one(reader_encoding_t encoding, const uint8_t *data,
                           size_t size, uint32_t *codepoint,
                           size_t *source_bytes);
size_t reader_text_encode_utf8(uint32_t codepoint, char out[4]);

/* Shared chapter-title matcher for TXT documents. */
bool reader_text_is_chapter_heading(const char *utf8_line, size_t length);

typedef enum {
    READER_INDENT_NONE = 0,
    READER_INDENT_NORMALIZE_EXISTING,
    READER_INDENT_FORCE_TWO_EM,
} reader_indent_mode_t;

typedef struct {
    uint16_t paragraphs_seen;
    uint16_t paragraphs_indented;
    uint16_t paragraphs_blank_separated;
    uint16_t paragraphs_long;
    uint16_t paragraphs_short;
} reader_indent_analysis_t;

void reader_indent_analysis_reset(reader_indent_analysis_t *analysis);
void reader_indent_analysis_observe(reader_indent_analysis_t *analysis,
                                    const char *utf8_line, size_t length,
                                    bool follows_blank_line);
reader_indent_mode_t reader_indent_analysis_mode(const reader_indent_analysis_t *analysis);

enum {
    READER_LAYOUT_MAX_LINES = 32,
    READER_LAYOUT_MAX_LINE_BYTES = 768,
    READER_LAYOUT_MAX_STYLE_RUNS = 16,
};

/* Optional lightweight presentation semantics use invisible private-use
 * codepoints. The paginator consumes these markers and exposes style runs;
 * they are never copied into rendered line text. */
enum {
    READER_TEXT_MARK_HEADING_START = 0xe000,
    READER_TEXT_MARK_HEADING_END = 0xe001,
    READER_TEXT_MARK_BOLD_START = 0xe002,
    READER_TEXT_MARK_BOLD_END = 0xe003,
};

enum {
    READER_TEXT_STYLE_NONE = 0,
    READER_TEXT_STYLE_BOLD = 1u << 0,
    READER_TEXT_STYLE_HEADING = 1u << 1,
};

typedef int (*reader_glyph_advance_fn)(uint32_t codepoint, void *context);
typedef int (*reader_styled_glyph_advance_fn)(uint32_t codepoint,
                                              uint8_t style_flags,
                                              void *context);

/* Returns whether a compositor should distribute an extra justification pixel
 * between the two adjacent codepoints. Kept public so page rendering uses the
 * same gap definition as reader_layout_page(). */
bool reader_text_is_justify_gap(uint32_t left_codepoint,
                                 uint32_t right_codepoint);

typedef struct {
    uint16_t viewport_width_px;
    /* Zero keeps the legacy line-count-only behavior. */
    uint16_t viewport_height_px;
    uint16_t line_height_px;
    /* Optional larger line box for styled headings. */
    uint16_t heading_line_height_px;
    /* Optional full-width image block height. */
    uint16_t image_block_height_px;
    uint16_t paragraph_spacing_px;
    uint16_t indent_width_px;
    uint16_t fallback_advance_px;
    /* Zero retains READER_LAYOUT_MAX_LINES. */
    uint8_t max_lines;
    bool justify;
    bool vertical_justify;
    bool english_hyphenation;
    bool kinsoku;
    reader_indent_mode_t indent_mode;
    uint8_t initial_style_flags;
    /* Session-provided source state.  When set, layout uses these fields
     * instead of guessing from the currently decoded window. */
    bool layout_metadata_valid;
    size_t source_limit;
    bool continuation;
    bool chapter_end;
    int16_t first_line_ink_top_px;
    int16_t last_line_ink_bottom_px;
    reader_glyph_advance_fn glyph_advance;
    reader_styled_glyph_advance_fn styled_glyph_advance;
    void *glyph_context;
} reader_layout_config_t;

enum {
    READER_LAYOUT_LINE_WRAPPED = 1u << 0,
    READER_LAYOUT_LINE_PARAGRAPH_END = 1u << 1,
    READER_LAYOUT_LINE_HYPHENATED = 1u << 2,
    READER_LAYOUT_LINE_INDENTED = 1u << 3,
};

typedef struct {
    uint16_t byte_start;
    uint16_t byte_end;
    uint8_t style_flags;
} reader_layout_style_run_t;

typedef struct {
    char utf8[READER_LAYOUT_MAX_LINE_BYTES];
    size_t source_start;
    size_t source_end;
    uint16_t width_px;
    uint16_t initial_x_px;
    uint16_t y_offset_px;
    uint16_t line_height_px;
    uint16_t stretch_gaps;
    uint16_t justify_extra_px;
    uint8_t flags;
    uint8_t style_run_count;
    reader_layout_style_run_t style_runs[READER_LAYOUT_MAX_STYLE_RUNS];
} reader_layout_line_t;

typedef struct {
    reader_layout_line_t lines[READER_LAYOUT_MAX_LINES];
    uint8_t line_count;
    size_t source_bytes_consumed;
    bool next_starts_paragraph;
    bool filled_by_capacity;
    bool next_anchor_valid;
    bool chapter_end;
    uint8_t next_style_flags;
    int16_t first_line_ink_top_px;
    int16_t last_line_ink_bottom_px;
} reader_layout_page_t;

/* Creates a render plan for one page from a UTF-8 chunk. Text after the
 * returned byte count stays untouched, so the caller can page a file without
 * rereading or retaining the complete book. */
bool reader_layout_page(const char *utf8, size_t size, bool starts_paragraph,
                        const reader_layout_config_t *config,
                        reader_layout_page_t *page);

#ifdef __cplusplus
}
#endif
