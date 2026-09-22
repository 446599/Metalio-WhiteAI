#include "power/activity.h"
#include "note_writer.h"
#include "note_service.h"
#include "application.h"
#include "reminders/reminder_store.h"
#include <ctime>
#include <memory>
#include <new>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
namespace notes {
struct Writer::Job {Writer* owner;Note note;uint32_t operation;};
Writer& Writer::Instance() {static Writer writer;return writer;}
WriteSnapshot Writer::Snapshot() const {std::lock_guard<std::mutex> lock(mutex_);return state_;}
uint32_t Writer::Revision() const {std::lock_guard<std::mutex> lock(mutex_);return state_.revision;}
uint32_t Writer::Save(const Note& draft) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(state_.busy || state_.operation==UINT32_MAX) return 0;
    std::unique_ptr<Job> job(new(std::nothrow) Job{this,draft,state_.operation+1});
    if(!job) return 0;
    state_.operation=job->operation;state_.busy=true;state_.ok=false;state_.error.clear();++state_.revision;
    if(xTaskCreate(Worker,"note_writer",8192,job.get(),2,nullptr)!=pdPASS) {state_.busy=false;state_.error="保存任务未启动";++state_.revision;return 0;}
    job.release();return state_.operation;
}
void Writer::Worker(void* arg) {
    {
        power::Activity activity;
        while(!activity){vTaskDelay(pdMS_TO_TICKS(20));activity.Retry();}
        std::unique_ptr<Job> job(static_cast<Job*>(arg));
        auto& store=DeviceStore();Note saved;std::string error;bool ok;
        const auto now=time(nullptr);const auto stamp=reminders::ValidClock(now) ? now : 0;
        if(job->note.id) {
            Patch patch;patch.title=job->note.title;patch.text=job->note.text;patch.project=job->note.project;
            ok=store.Update(job->note.id,job->note.revision,patch,stamp,saved,error);
        } else {
            job->note.updated=stamp;
            if(job->note.source_id.empty()) ok=store.Put(job->note,saved,error);
            else {bool existed=false;ok=store.Archive(job->note,saved,existed,error);}
        }
        {
            std::lock_guard<std::mutex> lock(job->owner->mutex_);
            auto& state=job->owner->state_;
            state.busy=false;state.ok=ok;state.id=ok ? saved.id : 0;state.error=error;++state.revision;
        }
        Application::GetInstance().RequestStatusUpdate(true);
    }
    vTaskDelete(nullptr);
}
}
