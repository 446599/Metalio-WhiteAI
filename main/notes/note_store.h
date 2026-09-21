#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace notes {
struct Note {
    // Keep the legacy aggregate prefix used by self.notes.save and host tests.
    uint32_t id=0;
    std::string title, text;
    int64_t updated=0;
    std::string raw_text, project, source_id;
    int64_t created=0;
    uint32_t revision=1, source_revision=0, reminder_id=0;
    bool done=false;
};
struct Patch {
    std::optional<std::string> title, text, project;
    std::optional<bool> done;
    std::optional<uint32_t> reminder_id;
};
// UTF-8 byte limits are enforced in firmware, not just in tool descriptions.
bool ValidText(const std::string& text, size_t limit, bool allow_empty=true);
// Check wire JSON before cJSON turns encoded NUL into a C-string terminator.
bool ContainsEncodedNull(const std::string& json);
std::string DisplayBody(const Note& note);
class Store {
public:
    static constexpr size_t kCapacity=8, kTitleBytes=96, kTextBytes=1536;
    static constexpr size_t kRawBytes=1536, kProjectBytes=96, kSnapshotBytes=65536;
    explicit Store(std::function<bool(const std::string&)> save) : save_(std::move(save)) {}
    bool Restore(const std::string& json);
    // Legacy updates only change title/text/time, never archived source metadata.
    bool Put(Note note, Note& saved, std::string& error);
    bool Archive(Note note, Note& saved, bool& existed, std::string& error);
    bool Update(uint32_t id, uint32_t expected_revision, const Patch& patch,
                int64_t now, Note& saved, std::string& error);
    bool Remove(uint32_t id, std::string& error);
    std::vector<Note> List() const;
    std::vector<Note> Search(const std::string& query, const std::string& project,
                             std::optional<bool> done=std::nullopt) const;
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
