#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
namespace power {
// A tiny process-wide barrier. Workers keep a lease across I/O, not just the
// initial busy check. Freeze and acquiring a lease are mutually exclusive.
class Gate {
public:
    static Gate& Instance(){static Gate gate;return gate;}
    bool Enter(){std::lock_guard<std::mutex> lock(mutex_);if(frozen_)return false;++users_;return true;}
    void Leave(){std::lock_guard<std::mutex> lock(mutex_);if(users_)--users_;}
    bool Freeze(){std::lock_guard<std::mutex> lock(mutex_);if(users_||frozen_)return false;frozen_=true;return true;}
    void Thaw(){std::lock_guard<std::mutex> lock(mutex_);frozen_=false;}
    unsigned Users()const{std::lock_guard<std::mutex> lock(mutex_);return users_;}
    std::atomic<bool> locked{false};
    std::atomic<int64_t> last_input_ms{0};
private:
    mutable std::mutex mutex_; unsigned users_=0; bool frozen_=false;
};
inline bool Locked(){return Gate::Instance().locked.load();}
inline void Touch(int64_t now){if(!Locked())Gate::Instance().last_input_ms.store(now);}
class Activity {
public:
    Activity():held_(Gate::Instance().Enter()){}
    ~Activity(){Release();}
    Activity(const Activity&)=delete;Activity& operator=(const Activity&)=delete;
    explicit operator bool()const{return held_;}
    void Retry(){if(!held_)held_=Gate::Instance().Enter();}
    void Release(){if(held_){Gate::Instance().Leave();held_=false;}}
private:bool held_;
};
}
