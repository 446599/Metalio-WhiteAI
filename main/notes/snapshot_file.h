#pragma once
#include "note_store.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <unistd.h>

namespace notes {
// Alternate complete snapshots. A failed/truncated write never overwrites the
// last valid slot. CRC rejects partial SD writes; schema validation is injected.
class SnapshotFile {
public:
    explicit SnapshotFile(std::string path):path_(std::move(path)) {}
    bool Load(std::string& json,const std::function<bool(const std::string&)>& valid) {
        ready_=false;
        bool missing[2]{};uint32_t newest=0;int selected=-1;std::string best;
        for (int slot=0;slot<2;++slot) {
            uint32_t sequence=0;std::string data;
            if (Read(slot,data,sequence,missing[slot]) && valid(data) && sequence>newest) {
                newest=sequence;selected=slot;best=std::move(data);
            }
        }
        if (selected<0 && !(missing[0] && missing[1])) return false;
        slot_=selected;sequence_=newest;json=std::move(best);ready_=true;return true;
    }
    bool Save(const std::string& json) {
        if (!ready_ || sequence_==UINT32_MAX || json.empty() || json.size()>Store::kSnapshotBytes) return false;
        const int target=slot_==0 ? 1 : 0;
        FILE* file=std::fopen(Path(target).c_str(),"wb");if (!file) return false;
        std::array<uint8_t,16> header{{'M','N','T',1}};
        Put(header.data()+4,sequence_+1);Put(header.data()+8,json.size());Put(header.data()+12,Crc(json));
        bool ok=std::fwrite(header.data(),1,header.size(),file)==header.size() &&
            std::fwrite(json.data(),1,json.size(),file)==json.size();
        if (std::fflush(file)!=0 || fsync(fileno(file))!=0) ok=false;
        if (std::fclose(file)!=0) ok=false;
        // Verify the new slot before publishing its sequence or migration.
        if (ok) {
            std::string verified;uint32_t seq=0;bool missing=false;
            ok=Read(target,verified,seq,missing) && seq==sequence_+1 && verified==json;
        }
        if (ok) {slot_=target;++sequence_;}return ok;
    }
private:
    static uint32_t Crc(const std::string& data) {
        uint32_t crc=~0U;
        for (uint8_t byte:data) {crc^=byte;for (int i=0;i<8;++i) crc=(crc>>1)^((crc&1) ? 0xedb88320U : 0);}
        return ~crc;
    }
    static void Put(uint8_t* p,uint32_t n) {for (int i=0;i<4;++i) p[i]=(n>>(8*i))&255;}
    static uint32_t Get(const uint8_t* p) {uint32_t n=0;for (int i=0;i<4;++i) n|=uint32_t(p[i])<<(8*i);return n;}
    std::string Path(int slot) const {return path_+"."+std::to_string(slot);}
    bool Read(int slot,std::string& json,uint32_t& sequence,bool& missing) const {
        FILE* file=std::fopen(Path(slot).c_str(),"rb");
        if (!file) {missing=errno==ENOENT;return false;}
        std::array<uint8_t,16> header{};bool ok=std::fread(header.data(),1,16,file)==16;
        const auto size=Get(header.data()+8);sequence=Get(header.data()+4);
        ok=ok && header[0]=='M' && header[1]=='N' && header[2]=='T' && header[3]==1 && sequence && size>0 && size<=Store::kSnapshotBytes;
        if (ok) {json.resize(size);ok=std::fread(json.data(),1,size,file)==size && std::fgetc(file)==EOF && !std::ferror(file) && Crc(json)==Get(header.data()+12);}
        std::fclose(file);return ok;
    }
    std::string path_;
    uint32_t sequence_=0;
    int slot_=-1;
    bool ready_=false;
};
}
