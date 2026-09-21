#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace audio {
// Bounded mono PCM WAV. Both save and restore run on the audio worker.
inline void WavU32(uint8_t* p, uint32_t value) {
    for (int i=0;i<4;++i) p[i]=static_cast<uint8_t>(value>>(8*i));
}
inline uint32_t WavReadU32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24;
}
inline bool SaveRecording(const std::string& path, const int16_t* pcm, uint32_t count, int rate) {
    if (!pcm || rate != 16000 || !count || count > static_cast<uint32_t>(rate*30)) return false;
    uint8_t h[44]{};
    std::memcpy(h,"RIFF",4); WavU32(h+4,count*2+36); std::memcpy(h+8,"WAVEfmt ",8);
    WavU32(h+16,16); h[20]=1; h[22]=1; WavU32(h+24,rate); WavU32(h+28,rate*2);
    h[32]=2; h[34]=16; std::memcpy(h+36,"data",4); WavU32(h+40,count*2);
    const auto temp=path+".tmp";
    FILE* f=std::fopen(temp.c_str(),"wb"); if (!f) return false;
    bool ok=std::fwrite(h,1,sizeof(h),f)==sizeof(h) && std::fwrite(pcm,2,count,f)==count;
    if (std::fflush(f)!=0 || fsync(fileno(f))!=0) ok=false;
    if (std::fclose(f)!=0) ok=false;
    const auto backup=path+".bak";
    struct stat existing{};
    bool moved=false;
    // FatFS rename cannot replace an existing destination. Keep the previous
    // complete clip as a recovery copy across an interrupted replacement.
    if (ok && stat(path.c_str(),&existing)==0) {
        std::remove(backup.c_str());
        moved=std::rename(path.c_str(),backup.c_str())==0;
        ok=moved;
    }
    if (ok) ok=std::rename(temp.c_str(),path.c_str())==0;
    if (!ok && moved) (void)std::rename(backup.c_str(),path.c_str());
    if (!ok) std::remove(temp.c_str());
    return ok;
}
inline uint32_t LoadRecording(const std::string& path, int16_t* pcm, uint32_t capacity, int rate) {
    FILE* f=std::fopen(path.c_str(),"rb"); if (!f) return 0;
    uint8_t h[44]{};
    const bool header=std::fread(h,1,44,f)==44 && !std::memcmp(h,"RIFF",4) &&
        !std::memcmp(h+8,"WAVEfmt ",8) && WavReadU32(h+16)==16 &&
        h[20]==1 && h[21]==0 && h[22]==1 && h[23]==0 &&
        WavReadU32(h+24)==static_cast<uint32_t>(rate) && WavReadU32(h+28)==static_cast<uint32_t>(rate*2) &&
        h[32]==2 && h[33]==0 && h[34]==16 && h[35]==0 && !std::memcmp(h+36,"data",4);
    const auto bytes=WavReadU32(h+40);
    const bool valid=header && bytes>0 && bytes%2==0 && bytes/2<=capacity && WavReadU32(h+4)==bytes+36;
    const bool ok=valid && pcm && std::fread(pcm,1,bytes,f)==bytes && std::fgetc(f)==EOF;
    std::fclose(f);
    return ok ? bytes/2 : 0;
}
}  // namespace audio
