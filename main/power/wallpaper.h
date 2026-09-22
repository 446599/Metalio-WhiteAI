#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>
namespace power {
// Strict P4 480x800 PBM. No decoded image libraries, path from network, or gray.
// The fixed path is supplied by the device; P4 bits are black=1, opposite to FB.
inline bool LoadWallpaper(const char* path,std::vector<uint8_t>& bitmap) {
    bitmap.clear();FILE* file=std::fopen(path,"rb");if(!file)return false;
    std::vector<uint8_t> bytes(49153);size_t n=std::fread(bytes.data(),1,bytes.size(),file);
    bool ok=!std::ferror(file);std::fclose(file);if(!ok||n==bytes.size())return false;bytes.resize(n);
    size_t p=0;
    auto space=[](uint8_t c){return c==' '||c=='\n'||c=='\r'||c=='\t';};
    auto skip=[&](){while(p<n){if(space(bytes[p])){++p;continue;}if(bytes[p]!='#')break;while(p<n&&bytes[p]!='\n')++p;}};
    auto number=[&](){skip();unsigned v=0;size_t start=p;while(p<n&&bytes[p]>='0'&&bytes[p]<='9'){v=v*10+bytes[p++]-'0';if(v>10000)return 0u;}return p>start?v:0u;};
    if(n<9||bytes[0]!='P'||bytes[1]!='4'||!space(bytes[2]))return false;
    p=2;unsigned w=number(),h=number();if(w!=480||h!=800||p>=n||!space(bytes[p]))return false;
    const uint8_t delimiter=bytes[p++];if(delimiter=='\r'&&p<n&&bytes[p]=='\n')++p;
    if(n-p!=48000)return false;
    bitmap.assign(bytes.begin()+p,bytes.end());return true;
}
}
