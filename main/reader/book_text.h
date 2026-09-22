#pragma once
#include "input/text_input.h"
#include <algorithm>
#include <cstdint>
#include <string>
namespace reader {
constexpr size_t kBookBytes=256*1024, kBooks=32;
inline bool FileName(const std::string& name) {
    if(name.empty() || name.size()>192 || name.front()=='.' || name.find('/')!=std::string::npos ||
       name.find('\\')!=std::string::npos || !input::ValidUtf8(name)) return false;
    for(unsigned char c:name) if(c<32) return false;
    if(name.size()<5) return false;
    auto extension=name.substr(name.size()-4);
    for(auto& c:extension) if(c>='A' && c<='Z') c+=32;
    return extension==".txt";
}
inline bool Normalize(std::string& text) {
    if(text.size()>kBookBytes || text.find('\0')!=std::string::npos || !input::ValidUtf8(text)) return false;
    if(text.compare(0,3,"\xef\xbb\xbf")==0) text.erase(0,3);
    // Keep newlines and tabs as whitespace. Other control codes have no display semantics.
    for(unsigned char c:text) if(c<32 && c!='\r' && c!='\n' && c!='\t') return false;
    std::replace(text.begin(),text.end(),'\t',' ');
    return true;
}
inline uint32_t Fingerprint(const std::string& text) {
    uint32_t hash=2166136261u;for(unsigned char c:text) hash=(hash^c)*16777619u;return hash;
}
} // namespace reader
