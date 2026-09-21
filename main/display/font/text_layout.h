#pragma once
#include <algorithm>
#include <string>
#include <vector>

namespace raw_font {
// UTF-8 codepoint boundaries, explicit newlines and bounded row storage. The
// caller supplies the same advance function used by the framebuffer renderer.
template <class Measure>
std::vector<std::string> Wrap(const std::string& text, int max_width, Measure measure) {
    std::vector<std::string> lines;
    std::string line;
    int width = 0;
    for (size_t i = 0; i < text.size() && lines.size() < 512;) {
        const unsigned char first = text[i];
        size_t bytes = first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
        bytes = std::min(bytes, text.size() - i);
        uint32_t cp = first & (bytes == 1 ? 0x7f : bytes == 2 ? 0x1f : bytes == 3 ? 0x0f : 7);
        bool valid = true;
        for (size_t j = 1; j < bytes; ++j) {
            if ((static_cast<unsigned char>(text[i+j]) & 0xc0) != 0x80) { valid = false; break; }
            cp = cp << 6 | (static_cast<unsigned char>(text[i+j]) & 0x3f);
        }
        if (!valid) { bytes = 1; cp = 0xfffd; }
        if (cp == '\r') { i += bytes; continue; }
        if (cp == '\n') { lines.push_back(line); line.clear(); width = 0; i += bytes; continue; }
        const int advance = measure(cp);
        if (!line.empty() && width + advance - 1 > max_width) {
            lines.push_back(line); line.clear(); width = 0;
        }
        line.append(text, i, bytes); width += advance; i += bytes;
    }
    if (!line.empty() || lines.empty()) lines.push_back(line);
    return lines;
}
}
