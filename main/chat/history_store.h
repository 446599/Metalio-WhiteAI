#pragma once
#include "xiaozhi/conversation.h"
#include <cstdint>
#include <string>
#include <vector>

namespace chat {
struct Turn {
    uint32_t number=0;
    int64_t updated=0;
    std::string user,assistant,action,status;
    bool truncated=false;
};
struct Summary { uint32_t id=0,turns=0; int64_t updated=0; std::string title; bool damaged=false; };
// One bounded dual-slot record per turn. No large in-memory history/JSON index
// and no automatic eviction. The directory itself is the recoverable index.
class Store final {
public:
    static constexpr uint32_t kSessions=64,kTurns=128;
    explicit Store(std::string root):root_(std::move(root)){}
    bool List(std::vector<Summary>& out,std::string& error) const;
    bool Create(uint32_t& session,std::string& error);
    bool Save(uint32_t session,const Turn& turn,std::string& error);
    bool Read(uint32_t session,uint32_t turn,Turn& out,std::string& error) const;
    bool Numbers(uint32_t session,std::vector<uint32_t>& out,std::string& error) const;
    bool Delete(uint32_t session,std::string& error);
    std::string Context(uint32_t session,std::string& error) const;
    static std::string Title(const std::string& text);
    static bool Valid(const Turn& turn);
private:
    std::string Directory(uint32_t session) const;
    std::string root_;
};
}
