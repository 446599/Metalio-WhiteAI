#include "setup_model.h"
#include "input/text_input.h"
#include <algorithm>

namespace network {
bool ValidCredentials(const std::string& ssid,const std::string& password,Security security) {
    if (ssid.empty() || ssid.size()>32 || !input::ValidUtf8(ssid) ||
        std::any_of(ssid.begin(),ssid.end(),[](unsigned char c){return c<32 || c==127;})) return false;
    if (security==Security::Unsupported) return false;
    if (security==Security::Open) return password.empty();
    if (password.size()==64) return std::all_of(password.begin(),password.end(),[](unsigned char c){
        return (c>='0' && c<='9') || (c>='a' && c<='f') || (c>='A' && c<='F');});
    return password.size()>=8 && password.size()<=63 &&
        std::all_of(password.begin(),password.end(),[](unsigned char c){return c>=32 && c<=126;});
}
std::vector<AccessPoint> NormalizeScan(std::vector<AccessPoint> items) {
    items.erase(std::remove_if(items.begin(),items.end(),[](const auto& a){
        return a.ssid.empty() || a.ssid.size()>32 || !input::ValidUtf8(a.ssid) ||
            std::any_of(a.ssid.begin(),a.ssid.end(),[](unsigned char c){return c<32 || c==127;});
    }),items.end());
    std::stable_sort(items.begin(),items.end(),[](const auto& a,const auto& b){
        return a.rssi!=b.rssi ? a.rssi>b.rssi : a.ssid<b.ssid;});
    std::vector<AccessPoint> result;
    for (auto& a:items) {
        if (std::none_of(result.begin(),result.end(),[&](const auto& b){return a.ssid==b.ssid && a.security==b.security;}))
            result.push_back(std::move(a));
        if (result.size()==24) break;
    }
    return result;
}
bool Busy(SetupState s) {return s==SetupState::Scanning || s==SetupState::Connecting || s==SetupState::Cancelling || s==SetupState::Saving;}
uint32_t SetupModel::Begin(bool scan,const std::string& ssid) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Busy(state_.state) || state_.operation==UINT32_MAX) return 0;
    ++state_.operation;++state_.revision;state_.saved=false;state_.ip.clear();state_.ssid=ssid;
    state_.state=scan ? SetupState::Scanning : SetupState::Connecting;
    state_.message=scan ? "正在扫描附近网络" : "正在连接，请稍候";
    return state_.operation;
}
bool SetupModel::Cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.state!=SetupState::Scanning && state_.state!=SetupState::Connecting) return false;
    state_.state=SetupState::Cancelling;state_.message="正在取消";++state_.revision;return true;
}
bool SetupModel::Cancelled(uint32_t operation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return operation!=state_.operation || state_.state==SetupState::Cancelling || state_.state==SetupState::Cancelled;
}
bool SetupModel::Scanned(uint32_t operation,bool ok,std::vector<AccessPoint> items) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (operation!=state_.operation || (state_.state!=SetupState::Scanning && state_.state!=SetupState::Cancelling)) return false;
    if (state_.state==SetupState::Cancelling) {state_.state=SetupState::Cancelled;state_.message="已取消扫描";}
    else if (!ok) {state_.state=SetupState::Failed;state_.message="扫描失败，可重新扫描";}
    else {state_.access_points=NormalizeScan(std::move(items));state_.state=SetupState::Choosing;
        state_.message=state_.access_points.empty() ? "未发现网络，可手动添加" : "请选择网络";}
    ++state_.revision;return true;
}
bool SetupModel::BeginSave(uint32_t operation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (operation!=state_.operation || state_.state!=SetupState::Connecting) return false;
    state_.state=SetupState::Saving;state_.message="已连接，正在保存配置";++state_.revision;return true;
}
bool SetupModel::Finish(uint32_t operation,bool connected,bool saved,const std::string& ip) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (operation!=state_.operation || !Busy(state_.state)) return false;
    if (state_.state==SetupState::Cancelling) {state_.state=SetupState::Cancelled;state_.message="已取消，未保存新密码";state_.saved=false;}
    else if (!connected) {state_.state=SetupState::Failed;state_.message="连接失败，请检查密码、信号或 DHCP";state_.saved=false;}
    else {state_.state=SetupState::Connected;state_.ip=ip;state_.saved=saved;
        state_.message=saved ? "已连接并保存" : "已连接，但保存失败；重启后需重新配置";}
    ++state_.revision;return true;
}
SetupSnapshot SetupModel::Snapshot() const {std::lock_guard<std::mutex> lock(mutex_);return state_;}
uint32_t SetupModel::Revision() const {std::lock_guard<std::mutex> lock(mutex_);return state_.revision;}
}  // namespace network
