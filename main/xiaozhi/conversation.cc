#include "conversation.h"

#include <algorithm>
#include <cstring>

namespace xiaozhi {
namespace {
std::string Bounded(const char* text, size_t limit) {
    if (!text) return {};
    size_t bytes = std::strlen(text);
    if (bytes <= limit) return text;
    bytes = limit;
    while (bytes > 0 && (static_cast<uint8_t>(text[bytes]) & 0xC0) == 0x80) --bytes;
    return std::string(text, bytes);
}
}
Conversation& Conversation::GetInstance() { static Conversation instance; return instance; }
void Conversation::Changed() { if (++data_.revision == 0) ++data_.revision; }
void Conversation::ContentChanged() {
    if (++data_.content_revision == 0) ++data_.content_revision;
    data_.saved = false;
    data_.save_failed = false;
    if (data_.persisted_revision != 0) data_.message = "内容已更新，待保存";
}
ConversationSnapshot Conversation::Snapshot() const { std::lock_guard<std::mutex> lock(mutex_); return data_; }
ConversationSnapshot Conversation::PreviousCompleted() const {
    std::lock_guard<std::mutex> lock(mutex_);return previous_completed_;
}
void Conversation::FreezePreviousLocked() {
    previous_completed_ = data_.state==TurnState::Done && !data_.truncated && !data_.transcript.empty()
        ? data_ : ConversationSnapshot{};
}
uint32_t Conversation::Revision() const { std::lock_guard<std::mutex> lock(mutex_); return data_.revision; }
uint32_t Conversation::Begin() {
    std::lock_guard<std::mutex> lock(mutex_);
    FreezePreviousLocked();
    ++data_.turn;
    data_.transcript.clear(); data_.answer.clear(); data_.message.clear();
    data_.saved = false; data_.truncated = false; has_sentences_ = false;
    data_.content_revision = 0; data_.persisted_revision = 0;
    data_.saving_revision = 0; data_.save_failed = false;
    data_.state = TurnState::Connecting;
    Changed();
    return data_.turn;
}
void Conversation::SetState(TurnState state, const char* message) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string next = Bounded(message, 180);
    if (data_.state == state && data_.message == next) return;
    data_.state = state; data_.message = next; Changed();
}
void Conversation::SetTranscript(const char* text) {
    std::lock_guard<std::mutex> lock(mutex_);
    data_.transcript = Bounded(text, kTranscriptBytes);
    data_.truncated = text && std::strlen(text) > kTranscriptBytes;
    ContentChanged(); data_.state = TurnState::Thinking; Changed();
}
void Conversation::ReceiveAnswer(const char* text, bool sentence) {
    if (!text || !*text) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sentence && has_sentences_) return;
    // The reference server supplies the spoken text in sentence_start. Prefer
    // those complete sentences over optional LLM token/cumulative updates.
    if (sentence && !has_sentences_) { data_.answer.clear(); has_sentences_ = true; }
    const auto incoming = Bounded(text, kAnswerBytes);
    if (!sentence && incoming.compare(0, data_.answer.size(), data_.answer) == 0) {
        data_.answer = incoming;
    } else {
        if (sentence && !data_.answer.empty() && data_.answer.size() < kAnswerBytes) data_.answer += '\n';
        const size_t remaining = kAnswerBytes - data_.answer.size();
        data_.answer += Bounded(text, remaining);
        if (std::strlen(text) > remaining) data_.truncated = true;
    }
    if (std::strlen(text) > kAnswerBytes) data_.truncated = true;
    ContentChanged(); Changed();
}
void Conversation::BeginFollowup(const char* label) {
    std::lock_guard<std::mutex> lock(mutex_);
    FreezePreviousLocked();
    ++data_.turn; data_.answer.clear(); data_.saved = false;
    data_.content_revision = 1; data_.persisted_revision = 0;
    data_.saving_revision = 0; data_.save_failed = false;
    has_sentences_ = false; data_.truncated = false;
    data_.state = TurnState::Thinking; data_.message = Bounded(label, 180); Changed();
}
void Conversation::MarkSaving(uint32_t turn, uint32_t content_revision) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (data_.turn != turn || data_.content_revision != content_revision) return;
    data_.saving_revision = content_revision;
    data_.save_failed = false;
    data_.message = "正在保存到本机";
    Changed();
}
void Conversation::MarkSaved(uint32_t turn, uint32_t content_revision, bool saved) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (data_.turn != turn || content_revision > data_.content_revision) return;
    if (saved) data_.persisted_revision = std::max(data_.persisted_revision, content_revision);
    if (data_.saving_revision == content_revision) data_.saving_revision = 0;
    data_.saved = data_.persisted_revision != 0 && data_.persisted_revision == data_.content_revision;
    // A completion for older text must not claim the displayed answer was saved
    // or replace a newer save's progress/error message.
    if (data_.content_revision == content_revision) {
        data_.save_failed = !saved;
        data_.message = saved ? "已保存到本机" : "保存失败，内容仍在当前页面";
    } else if (data_.saving_revision == 0 && data_.message == "正在保存到本机") {
        data_.save_failed = !saved;
        data_.message = saved ? "内容已更新，待保存" : "保存失败，内容仍在当前页面";
    }
    Changed();
}
void Conversation::Restore(const std::string& transcript, const std::string& answer) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++data_.turn;
    data_.transcript = Bounded(transcript.c_str(), kTranscriptBytes);
    data_.answer = Bounded(answer.c_str(), kAnswerBytes);
    data_.state = TurnState::Done; data_.message = "上一次保存的胶囊";
    data_.saved = true; data_.truncated = false; has_sentences_ = false;
    data_.content_revision = 1; data_.persisted_revision = 1;
    data_.saving_revision = 0; data_.save_failed = false;
    Changed();
}
void Conversation::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t turn = data_.turn + 1, revision = data_.revision;
    previous_completed_ = {};
    data_ = {}; data_.turn = turn; data_.revision = revision; has_sentences_ = false; Changed();
}
std::string Conversation::Prompt(QuickAction action, const std::string& transcript) {
    const char* instructions[] = {
        "请把下面这段口述整理成简洁的灵感笔记，保留原意，不编造信息：\n",
        "请从下面这段口述提炼待办清单，未提及的时间和人员不要编造；仅生成草稿，不执行任务：\n",
        "请把下面这段口述翻译成自然的英文，只返回译文：\n",
    };
    const auto i = static_cast<unsigned>(action);
    return i < 3 ? std::string(instructions[i]) + Bounded(transcript.c_str(), kTranscriptBytes) : "";
}
}  // namespace xiaozhi
