#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace input {
enum class Mode { Pinyin, Lower, Upper, Symbols };
struct Candidate { std::string text; size_t consumed = 0; };
bool ValidUtf8(const std::string& text);
void Wipe(std::string& text);
size_t PreviousBoundary(const std::string& text, size_t position);
size_t NextBoundary(const std::string& text, size_t position);

// UI-task-owned editor. No network, disk, global keyboard state or text logging.
class Editor {
public:
    static constexpr size_t kMaxBytes = 1536, kCompositionBytes = 32, kCandidatesPerPage = 4;
    bool Begin(const std::string& text, size_t limit, bool secret = false, bool multiline = false);
    void Clear();
    bool Type(char key);
    bool Insert(const std::string& text);
    bool Backspace();
    bool Move(int direction);
    bool SetMode(Mode mode);
    bool Choose(size_t visible_index);
    bool Space();
    bool NextCandidates();
    bool Ready() const { return composition_.empty(); }
    bool Secret() const { return secret_; }
    bool Multiline() const { return multiline_; }
    Mode CurrentMode() const { return mode_; }
    const std::string& Text() const { return text_; }
    const std::string& Composition() const { return composition_; }
    const std::string& Error() const { return error_; }
    size_t Cursor() const { return cursor_; }
    size_t Limit() const { return limit_; }
    size_t CandidatePage() const { return candidate_page_; }
    std::vector<Candidate> Candidates() const;
    size_t CandidateCount() const;
    std::string VisibleText(bool reveal = false) const;
private:
    bool Fail(const char* message);
    std::vector<Candidate> Lookup(size_t start, size_t count, size_t* total = nullptr) const;
    std::string text_, composition_, error_;
    size_t cursor_ = 0, limit_ = kMaxBytes, candidate_page_ = 0;
    bool secret_ = false, multiline_ = false;
    Mode mode_ = Mode::Pinyin;
};
}  // namespace input
