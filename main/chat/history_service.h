#pragma once
#include "history_store.h"
#include <deque>
#include <memory>
#include <mutex>

namespace chat {
struct View {
    uint32_t revision=0,selected=0,active=0;
    size_t turn_index=0,turn_count=0,pending=0;
    bool busy=false,failed=false;
    std::string message="对话将自动保存到 SD 卡";
    std::vector<Summary> sessions;
    std::shared_ptr<const Turn> turn;
};
class History final {
public:
    static History& Instance();
    // Reuse the existing Xiaozhi worker: never allocate an extra internal-RAM
    // task stack, and never perform SD access on RX, input or display threads.
    void Poll(int64_t monotonic_ms);
    bool Capture(const xiaozhi::ConversationSnapshot& snapshot,const std::string& action,const char* status);
    bool List(); bool Open(uint32_t id); bool Move(int direction);
    bool Switch(uint32_t id); // 0 starts a new local/cloud conversation.
    bool Delete(uint32_t id);
    void Retry();
    bool Switching() const;
    bool CanCapture() const;
    View Snapshot() const;
    uint32_t Revision() const;
    bool TakeResumed(std::string& context);
    void CancelResume();
private:
    History()=default;
    enum class Kind {Save,List,Open,Move,Switch,Delete};
    struct Job {Kind kind=Kind::List;uint32_t id=0,wire_turn=0;int direction=0;Turn turn;};
    bool Request(Job job);
    mutable std::mutex mutex_;
    std::deque<Job> queue_;
    View view_;
    bool switching_=false,resume_ready_=false;
    std::string resume_context_;
    uint32_t active_id_=0,last_wire_turn_=0,number_=0;
    int64_t retry_at_=0;
};
}
