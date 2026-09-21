#pragma once
#include "reminder_store.h"
#include "alert_state.h"
#include "xiaozhi/mcp_server.h"
#include "xiaozhi/system_tools.h"
#include <atomic>
#include <mutex>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace reminders {
class Service {
public:
    static Service& Instance();
    void Start();
    // Enqueue explicit alert actions; persistence and audio stay on the task.
    bool Dismiss(uint32_t token = 0);
    bool Snooze(uint32_t token);
    bool IsActive() const { return active_id_.load() != 0; }
    std::string HandleMcp(const std::string& payload, uint32_t epoch, bool diagnostic = false);
    std::string Status() const;
    std::vector<Item> List() const { return store_.List(); }
    bool SetEnabled(uint32_t id, bool enabled, std::string& error);
private:
    Service();
    static void Task(void* arg);
    void Run();
    void Publish();
    void PublishAlert();
    Store store_;
    xiaozhi::SystemTools system_tools_;
    xiaozhi::McpServer remote_, diagnostic_;
    std::mutex rpc_mutex_;
    uint32_t rpc_epoch_ = 0;
    std::atomic<uint32_t> requests_{0}, initialized_{0}, discoveries_{0}, calls_{0};
    std::atomic<uint32_t> active_id_{0};
    std::mutex alert_mutex_;
    AlertSnapshot alert_;
    enum class Action { None, Stop, Snooze };
    Action alert_action_ = Action::None;
    bool alert_dirty_ = false;
    TaskHandle_t task_ = nullptr;
    uint32_t published_revision_ = 0;
    int64_t published_minute_ = -1;
};
}  // namespace reminders
