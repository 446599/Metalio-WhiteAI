#pragma once
#include <algorithm>
#include <cstdint>
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
// Note bodies can contain thousands of explicit newlines. Count all rows, but
// retain only the requested page so the last source line remains reachable
// without allocating one std::string for every row. Other views keep Wrap().
struct TextPage {
    std::vector<std::string> lines;
    size_t page=0, pages=1;
};
template <class Measure>
TextPage Paginate(const std::string& text, int max_width, size_t requested_page,
                  size_t per_page, Measure measure) {
    max_width=std::max(1,max_width);
    per_page=std::clamp(per_page,size_t{1},size_t{64});
    const auto scan=[&](auto emit) {
        std::string line;
        size_t rows=0;
        int width=0;
        const auto finish=[&]() {emit(rows++,line);line.clear();width=0;};
        for (size_t i=0;i<text.size();) {
            const auto first=static_cast<unsigned char>(text[i]);
            size_t bytes=first<0x80 ? 1 : first<0xe0 ? 2 : first<0xf0 ? 3 : 4;
            bytes=std::min(bytes,text.size()-i);
            uint32_t cp=first & (bytes==1 ? 0x7f : bytes==2 ? 0x1f : bytes==3 ? 0x0f : 7);
            bool valid=true;
            for (size_t j=1;j<bytes;++j) {
                const auto next=static_cast<unsigned char>(text[i+j]);
                if ((next & 0xc0)!=0x80) {valid=false;break;}
                cp=(cp<<6) | (next & 0x3f);
            }
            if (!valid) {bytes=1;cp=0xfffd;}
            if (cp=='\r') {i+=bytes;continue;}
            if (cp=='\n') {finish();i+=bytes;continue;}
            const int advance=measure(cp);
            if (!line.empty() && width+advance-1>max_width) finish();
            line.append(text,i,bytes);width+=advance;i+=bytes;
        }
        if (!line.empty() || rows==0) finish();
        return rows;
    };
    const size_t rows=scan([](size_t,const std::string&){});
    TextPage result;
    result.pages=(rows+per_page-1)/per_page;
    result.page=std::min(requested_page,result.pages-1);
    const size_t first=result.page*per_page;
    result.lines.reserve(per_page);
    scan([&](size_t row,const std::string& line) {
        if (row>=first && row-first<per_page) result.lines.push_back(line);
    });
    return result;
}

}
