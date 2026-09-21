#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace notes {
struct Note { uint32_t id=0; std::string title, text; int64_t updated=0; };
class Store {
public:
    static constexpr size_t kCapacity=8, kTitleBytes=96, kTextBytes=1536;
    explicit Store(std::function<bool(const std::string&)> save) : save_(std::move(save)) {}
    bool Restore(const std::string& json);
    bool Put(Note note, Note& saved, std::string& error);
    bool Remove(uint32_t id, std::string& error);
    std::vector<Note> List() const;
    uint32_t Revision() const;
    bool Ready() const;
private:
    bool Commit(const std::vector<Note>& notes, uint32_t next);
    mutable std::mutex mutex_;
    std::function<bool(const std::string&)> save_;
    std::vector<Note> notes_;
    uint32_t next_=1, revision_=0;
    bool ready_=false;
};
}  // namespace notes
