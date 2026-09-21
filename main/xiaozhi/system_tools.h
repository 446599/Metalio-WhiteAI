#pragma once
#include "notes/note_store.h"
#include "reminders/reminder_store.h"
#include <functional>
#include <string>
struct cJSON;
namespace xiaozhi {
struct SystemCommand {
    enum class Kind { Status, Volume, Open, Calendar, Dashboard, Refresh,
                      RecorderStatus, Record, StopRecording, PlayRecording, ActionStatus, CancelAction };
    Kind kind=Kind::Status;
    std::string value;
    uint32_t number=0;
};
struct ToolReply { bool ok=false; std::string text; };
class SystemTools {
public:
    using Execute=std::function<ToolReply(const SystemCommand&)>;
    SystemTools(reminders::Store& reminders,notes::Store& notes,Execute execute)
        : reminders_(reminders),notes_(notes),execute_(std::move(execute)) {}
    static size_t Count();
    static const char* Schema(size_t index);
    static bool Knows(const char* name);
    ToolReply Handle(const char* name,const cJSON* args,int64_t now);
private:
    reminders::Store& reminders_;
    notes::Store& notes_;
    Execute execute_;
};
}
