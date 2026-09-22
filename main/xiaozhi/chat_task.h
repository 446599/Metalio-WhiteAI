#pragma once
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>

namespace xiaozhi {
// Long device text belongs in an explicitly requested MCP result, never in the
// wake-word field. This mailbox is frozen per action and expires/cancels with
// the transport epoch; it does not expose arbitrary files or credentials.
class ChatTask final {
public:
    static ChatTask& Instance() { static ChatTask task; return task; }
    static constexpr size_t kChunkBytes = 2048, kMaxBytes = 12288;
    bool Begin(const std::string& operation, const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (text.empty() || text.size() > kMaxBytes || text.find('\0') != std::string::npos || serial_ == UINT32_MAX) return false;
        ++serial_; operation_ = operation; text_ = text; read_until_ = 0; active_ = true; return true;
    }
    void Cancel() { std::lock_guard<std::mutex> lock(mutex_); active_ = false; text_.clear(); operation_.clear(); read_until_ = 0; }
    bool ReadAll() const { std::lock_guard<std::mutex> lock(mutex_); return active_ && read_until_ == text_.size(); }
    struct Chunk { bool ok=false; uint32_t id=0; size_t next=0,total=0; std::string operation,text,error; };
    Chunk Read(uint32_t id, size_t offset) {
        std::lock_guard<std::mutex> lock(mutex_);
        Chunk out;
        if (!active_) {out.error="没有待处理设备任务";return out;}
        if ((id && id != serial_) || (offset && !id)) {out.error="任务已改变，请从 offset=0 重新读取";return out;}
        if (offset > text_.size() || offset > read_until_ || (offset < text_.size() && (static_cast<unsigned char>(text_[offset]) & 0xc0) == 0x80)) {
            out.error="offset 无效，请按返回的 next_offset 顺序读取";return out;
        }
        size_t end=std::min(text_.size(),offset+kChunkBytes);
        while(end<text_.size() && end>offset && (static_cast<unsigned char>(text_[end])&0xc0)==0x80)--end;
        out.ok=true;out.id=serial_;out.operation=operation_;out.text=text_.substr(offset,end-offset);
        out.next=end;out.total=text_.size();read_until_=std::max(read_until_,end);return out;
    }
private:
    mutable std::mutex mutex_;
    uint32_t serial_=0; size_t read_until_=0; bool active_=false;
    std::string operation_,text_;
};
}
