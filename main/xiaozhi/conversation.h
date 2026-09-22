#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace xiaozhi {

enum class TurnState : uint8_t { Idle, Connecting, Listening, Transcribing, Thinking, Speaking, Done, Error };
enum class QuickAction : uint8_t { Organize, Tasks, Translate };

struct ConversationSnapshot {
    uint32_t revision = 0;
    uint32_t turn = 0;
    uint32_t content_revision = 0;
    uint32_t persisted_revision = 0;
    uint32_t saving_revision = 0;
    TurnState state = TurnState::Idle;
    std::string transcript;
    std::string answer;
    std::string message;
    bool saved = false;
    bool save_failed = false;
    bool truncated = false;
};

// Text is independent from the three short dashboard summaries. Snapshots use
// bounded heap strings so a full answer never consumes a task's small stack.
class Conversation final {
public:
    static constexpr size_t kTranscriptBytes = 1536;
    static constexpr size_t kAnswerBytes = 6144;
    static Conversation& GetInstance();
    ConversationSnapshot Snapshot() const;
    TurnState State() const;
    // Frozen at Begin(): an archive command must not archive its own speech.
    ConversationSnapshot PreviousCompleted() const;
    uint32_t Revision() const;
    uint32_t Begin();
    void SetState(TurnState state, const char* message = "");
    void SetTranscript(const char* text);
    void ReceiveAnswer(const char* text, bool sentence);
    void BeginFollowup(const char* label);
    void MarkSaving(uint32_t turn, uint32_t content_revision);
    void MarkSaved(uint32_t turn, uint32_t content_revision, bool saved);
    void Restore(const std::string& transcript, const std::string& answer);
    void Clear();
    static std::string Prompt(QuickAction action, const std::string& transcript);
private:
    void Changed();
    void ContentChanged();
    void FreezePreviousLocked();
    mutable std::mutex mutex_;
    ConversationSnapshot data_;
    ConversationSnapshot previous_completed_;
    bool has_sentences_ = false;
};

// Last-capsule persistence. Never called on the button or WebSocket RX tasks.
bool SaveLastCapsule(const ConversationSnapshot& snapshot);
bool LoadLastCapsule();

}  // namespace xiaozhi
