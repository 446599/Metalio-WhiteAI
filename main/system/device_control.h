#pragma once
#include "action_queue.h"
namespace device {
class Control {
public:
    static Control& Instance();
    xiaozhi::ToolReply Execute(const xiaozhi::SystemCommand& command);
    // Called only by the UI/event task, never by an audio or TLS callback.
    void Tick();
private:
    void Run(uint32_t id,const xiaozhi::SystemCommand& command);
    ActionQueue actions_;
    std::mutex recorder_mutex_;
    int64_t audio_idle_since_=0;
};
}
