#pragma once
#include "setup_model.h"
#include <atomic>
namespace network {
class WifiSetup {
public:
    static WifiSetup& Instance();
    bool Scan();
    bool Connect(const AccessPoint& ap,const std::string& password);
    bool Cancel() {return model_.Cancel();}
    SetupSnapshot Snapshot() const {return model_.Snapshot();}
    uint32_t Revision() const {return model_.Revision();}
private:
    struct Job;
    static void Worker(void* arg);
    bool Launch(Job* job);
    SetupModel model_;
};
}
