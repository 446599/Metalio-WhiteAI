#pragma once
#include "note_store.h"
#include <mutex>
namespace notes {
struct WriteSnapshot {uint32_t operation=0,id=0,revision=0;bool busy=false,ok=false;std::string error;};
class Writer {
public:
    static Writer& Instance();
    uint32_t Save(const Note& draft);
    WriteSnapshot Snapshot() const;
    uint32_t Revision() const;
private:
    struct Job;
    static void Worker(void* arg);
    mutable std::mutex mutex_;
    WriteSnapshot state_;
};
}
