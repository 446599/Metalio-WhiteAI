#pragma once
#include "xiaozhi/system_tools.h"
#include <algorithm>
#include <deque>
#include <mutex>
#include <optional>

namespace device {
enum class ActionState { Queued, Running, Succeeded, Failed, Cancelled, Expired };
inline const char* StateName(ActionState state) {
    static const char* names[]={"queued","running","succeeded","failed","cancelled","expired"};
    return names[static_cast<unsigned>(state)];
}
struct Action {
    uint32_t id=0;
    xiaozhi::SystemCommand command;
    ActionState state=ActionState::Queued;
    int64_t deadline=0;
    std::string detail;
};
class ActionQueue {
public:
    uint32_t Add(const xiaozhi::SystemCommand& command,int64_t now) {
        std::lock_guard<std::mutex> lock(mutex_);Expire(now);
        if (items_.size()==8) {
            auto old=std::find_if(items_.begin(),items_.end(),[](const auto& item){return item.state!=ActionState::Queued && item.state!=ActionState::Running;});
            if (old==items_.end()) return 0;
            items_.erase(old);
        }
        if (next_==UINT32_MAX) return 0;
        const auto id=next_++;
        items_.push_back({id,command,ActionState::Queued,now+30000,{}});return id;
    }
    bool Begin(uint32_t id,int64_t now) {
        std::lock_guard<std::mutex> lock(mutex_);Expire(now);
        for (auto& item:items_) if (item.id==id && item.state==ActionState::Queued) {
            item.state=ActionState::Running;return true;
        }
        return false;
    }
    void Finish(uint32_t id,bool ok,const std::string& detail) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item:items_) if (item.id==id && (item.state==ActionState::Running || item.state==ActionState::Queued)) {
            item.state=ok ? ActionState::Succeeded : ActionState::Failed;item.detail=detail;break;
        }
    }
    bool Cancel(uint32_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item:items_) if (item.id==id && item.state==ActionState::Queued) {item.state=ActionState::Cancelled;return true;}
        return false;
    }
    void CancelRecordings() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item:items_) if ((item.state==ActionState::Queued || item.state==ActionState::Running) &&
            (item.command.kind==xiaozhi::SystemCommand::Kind::Record || item.command.kind==xiaozhi::SystemCommand::Kind::PlayRecording)) item.state=ActionState::Cancelled;
    }
    std::optional<Action> Get(uint32_t id,int64_t now) {
        std::lock_guard<std::mutex> lock(mutex_);Expire(now);
        for (const auto& item:items_) if (item.id==id) return item;
        return {};
    }
    std::vector<Action> Pending(int64_t now) {
        std::lock_guard<std::mutex> lock(mutex_);Expire(now);std::vector<Action> pending;
        for (const auto& item:items_) if (item.state==ActionState::Queued) pending.push_back(item);
        return pending;
    }
private:
    void Expire(int64_t now) {
        for (auto& item:items_) if (item.state==ActionState::Queued && now>=item.deadline) item.state=ActionState::Expired;
    }
    std::mutex mutex_;
    std::deque<Action> items_;
    uint32_t next_=1;
};
}
