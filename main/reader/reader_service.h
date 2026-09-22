#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
namespace reader {
struct Snapshot {
    uint32_t revision=0;
    bool busy=false,opened=false;
    size_t page=0,pages=1;
    std::string title,message;
    std::vector<std::string> files,lines;
};
class Service {
public:
    static Service& Instance();
    Snapshot Get() const;
    uint32_t Revision() const;
    bool List();
    bool Open(const std::string& filename);
    bool Turn(int direction);
private:
    enum class Command {List,Open,Turn};
    struct Job;
    bool Start(Command command,const std::string& filename,int direction);
    static void Worker(void*);
    void ReadList();
    void ReadBook(const std::string& filename);
    void Paginate(size_t page);
    bool SaveBookmark();
    mutable std::mutex mutex_;
    Snapshot state_;
    std::shared_ptr<const std::string> text_;
    std::string filename_;
    uint32_t fingerprint_=0;
};
} // namespace reader
