#ifndef SSID_MANAGER_H
#define SSID_MANAGER_H
#include <mutex>
#include <string>
#include <vector>
struct SsidItem { std::string ssid; std::string password; };
class SsidManager {
public:
    static SsidManager& GetInstance() { static SsidManager instance; return instance; }
    bool AddSsid(const std::string& ssid, const std::string& password);
    void RemoveSsid(int index);
    void SetDefaultSsid(int index);
    void Clear();
    std::vector<SsidItem> GetSsidList() const;
private:
    SsidManager();
    ~SsidManager() = default;
    void LoadFromNvs();
    bool Save(const std::vector<SsidItem>& items);
    mutable std::mutex mutex_;
    std::vector<SsidItem> ssid_list_;
    bool writable_ = true;
};
#endif
