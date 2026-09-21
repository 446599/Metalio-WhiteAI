#include "ssid_manager.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <nvs.h>

namespace {
constexpr size_t kMax=10, kEntry=33+65, kSize=8+kMax*kEntry;
bool Valid(const SsidItem& i) {
    return !i.ssid.empty() && i.ssid.size()<=32 && i.ssid.find('\0')==std::string::npos &&
        i.password.size()<=64 && i.password.find('\0')==std::string::npos;
}
void Wipe(void* data,size_t size) {volatile unsigned char* p=static_cast<unsigned char*>(data);while(size--) *p++=0;}
}
SsidManager::SsidManager() {LoadFromNvs();}
std::vector<SsidItem> SsidManager::GetSsidList() const {std::lock_guard<std::mutex> lock(mutex_);return ssid_list_;}
void SsidManager::LoadFromNvs() {
    nvs_handle_t h=0;if(nvs_open("wifi",NVS_READONLY,&h)!=ESP_OK) return;
    std::array<char,kSize> blob{};size_t length=blob.size();
    const auto err=nvs_get_blob(h,"profiles_v2",blob.data(),&length);
    if(err==ESP_OK) {
        writable_=length==kSize && std::memcmp(blob.data(),"WIFI\x01",5)==0 &&
            static_cast<unsigned char>(blob[5])<=kMax && blob[6]==0 && blob[7]==0;
        std::vector<SsidItem> loaded;
        if(writable_) for(size_t i=0;i<static_cast<unsigned char>(blob[5]);++i) {
            const char* p=blob.data()+8+i*kEntry;
            if(!std::memchr(p,0,33) || !std::memchr(p+33,0,65)) {writable_=false;break;}
            SsidItem item{p,p+33};
            if(!Valid(item) || std::any_of(loaded.begin(),loaded.end(),[&](const auto& n){return n.ssid==item.ssid;})) {writable_=false;break;}
            loaded.push_back(std::move(item));
        }
        if(writable_) ssid_list_=std::move(loaded);
    } else if(err==ESP_ERR_NVS_NOT_FOUND) {
        // Read old keys without modifying them; migrate only on a successful save.
        for(size_t i=0;i<kMax;++i) {
            const std::string suffix=i ? std::to_string(i) : "";
            char ssid[33]{},password[65]{};size_t a=sizeof(ssid),b=sizeof(password);
            if(nvs_get_str(h,("ssid"+suffix).c_str(),ssid,&a)==ESP_OK &&
               nvs_get_str(h,("password"+suffix).c_str(),password,&b)==ESP_OK) {
                SsidItem item{ssid,password};if(Valid(item) && std::none_of(ssid_list_.begin(),ssid_list_.end(),[&](const auto& old){return old.ssid==item.ssid;})) ssid_list_.push_back(std::move(item));
            }
            Wipe(password,sizeof(password));
        }
    } else writable_=false;
    Wipe(blob.data(),blob.size());nvs_close(h);
}
bool SsidManager::Save(const std::vector<SsidItem>& items) {
    if(!writable_ || items.size()>kMax || std::any_of(items.begin(),items.end(),[](const auto& i){return !Valid(i);})) return false;
    std::array<char,kSize> blob{};std::memcpy(blob.data(),"WIFI\x01",5);blob[5]=static_cast<char>(items.size());
    for(size_t i=0;i<items.size();++i) {
        std::memcpy(blob.data()+8+i*kEntry,items[i].ssid.data(),items[i].ssid.size());
        std::memcpy(blob.data()+8+i*kEntry+33,items[i].password.data(),items[i].password.size());
    }
    nvs_handle_t h=0;bool ok=false;
    if(nvs_open("wifi",NVS_READWRITE,&h)==ESP_OK) {
        ok=nvs_set_blob(h,"profiles_v2",blob.data(),blob.size())==ESP_OK && nvs_commit(h)==ESP_OK;
        nvs_close(h);
    }
    Wipe(blob.data(),blob.size());if(ok) ssid_list_=items;return ok;
}
bool SsidManager::AddSsid(const std::string& ssid,const std::string& password) {
    std::lock_guard<std::mutex> lock(mutex_);if(!Valid({ssid,password})) return false;
    auto next=ssid_list_;next.erase(std::remove_if(next.begin(),next.end(),[&](const auto& i){return i.ssid==ssid;}),next.end());
    next.insert(next.begin(),{ssid,password});if(next.size()>kMax) next.resize(kMax);return Save(next);
}
void SsidManager::RemoveSsid(int index) {
    std::lock_guard<std::mutex> lock(mutex_);if(index<0 || index>=static_cast<int>(ssid_list_.size())) return;
    auto next=ssid_list_;next.erase(next.begin()+index);(void)Save(next);
}
void SsidManager::SetDefaultSsid(int index) {
    std::lock_guard<std::mutex> lock(mutex_);if(index<0 || index>=static_cast<int>(ssid_list_.size())) return;
    auto next=ssid_list_;auto item=next[index];next.erase(next.begin()+index);next.insert(next.begin(),std::move(item));(void)Save(next);
}
void SsidManager::Clear() {std::lock_guard<std::mutex> lock(mutex_);(void)Save({});}
