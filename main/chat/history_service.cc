#include "history_service.h"
#include "hal/hal.h"
#include "application.h"
#include <algorithm>
#include <ctime>

namespace chat {
#ifndef WHITEAI_CHAT_PATH
#define WHITEAI_CHAT_PATH "/sdcard/chats"
#endif
namespace {Store& Disk(){static Store store(WHITEAI_CHAT_PATH);return store;}}
History& History::Instance(){static History history;return history;}
View History::Snapshot()const{std::lock_guard<std::mutex> lock(mutex_);return view_;}
uint32_t History::Revision()const{std::lock_guard<std::mutex> lock(mutex_);return view_.revision;}
bool History::Switching()const{std::lock_guard<std::mutex> lock(mutex_);return switching_;}
bool History::CanCapture()const{std::lock_guard<std::mutex> lock(mutex_);return !switching_&&queue_.size()<8;}
void History::Retry(){std::lock_guard<std::mutex> lock(mutex_);retry_at_=0;}
bool History::Capture(const xiaozhi::ConversationSnapshot& s,const std::string& action,const char* status){
    if(s.transcript.empty())return true;
    Job job;job.kind=Kind::Save;job.wire_turn=s.turn;job.turn.user=s.transcript;job.turn.assistant=s.answer;
    job.turn.action=action;job.turn.status=status;job.turn.truncated=s.truncated;
    auto now=time(nullptr);job.turn.updated=now>0&&now<4102444800LL?now:0;
    std::lock_guard<std::mutex> lock(mutex_);
    for(auto& old:queue_)if(old.kind==Kind::Save&&old.wire_turn==s.turn){
        if(old.turn.status!="pending"&&job.turn.status=="pending")return true;
        old=std::move(job);++view_.revision;return true;
    }
    if(switching_||queue_.size()>=8){view_.failed=true;view_.message="历史待保存队列已满，请检查 SD 卡";++view_.revision;return false;}
    queue_.push_back(std::move(job));view_.pending=queue_.size();view_.message="对话正在保存";++view_.revision;return true;
}
bool History::Request(Job job){
    std::lock_guard<std::mutex> lock(mutex_);
    if(view_.busy||switching_)return false;
    // A failed pending save must not prevent opening history and removing an
    // unrelated session to make room. A single UI job may precede the bounded
    // eight-save queue; never evict an unsaved turn to enqueue maintenance.
    const bool recovery=!queue_.empty()&&view_.failed;
    if(job.kind==Kind::Switch || (job.kind==Kind::Delete&&job.id==view_.active)){
        if(!queue_.empty()){view_.message="请先完成历史保存，避免丢失对话";++view_.revision;return false;}
    }
    if(!recovery&&queue_.size()>=8)return false;
    if(job.kind==Kind::Switch||job.kind==Kind::Delete)switching_=true;
    if(job.kind==Kind::Open){view_.turn.reset();view_.selected=job.id;view_.turn_count=0;}
    if(recovery){queue_.push_front(std::move(job));retry_at_=0;}
    else queue_.push_back(std::move(job));
    view_.busy=true;++view_.revision;return true;
}
bool History::List(){Job j;j.kind=Kind::List;return Request(std::move(j));}
bool History::Open(uint32_t id){Job j;j.kind=Kind::Open;j.id=id;return Request(std::move(j));}
bool History::Move(int d){Job j;j.kind=Kind::Move;j.direction=d;return Request(std::move(j));}
bool History::Switch(uint32_t id){Job j;j.kind=Kind::Switch;j.id=id;return Request(std::move(j));}
bool History::Delete(uint32_t id){Job j;j.kind=Kind::Delete;j.id=id;return Request(std::move(j));}
bool History::TakeResumed(std::string& context){std::lock_guard<std::mutex> lock(mutex_);if(!resume_ready_)return false;context=std::move(resume_context_);resume_ready_=false;return true;}
void History::CancelResume(){std::lock_guard<std::mutex> lock(mutex_);resume_ready_=false;resume_context_.clear();}
void History::Poll(int64_t now){
    Job job;View result;
    {std::lock_guard<std::mutex> lock(mutex_);if(queue_.empty()||now<retry_at_)return;job=queue_.front();result=view_;view_.busy=true;} // Block UI queue reordering during SD I/O.
    bool ok=true;std::string error;auto& store=Disk();std::string context;
    if(!GetHAL().IsSdMounted() && !(job.kind==Kind::Switch&&!job.id)){ok=false;error="SD 卡不可用，历史未保存；内容暂留内存";}
    else if(job.kind==Kind::Save){
        if(active_id_ && last_wire_turn_!=job.wire_turn && number_>=Store::kTurns){
            uint32_t next=0;ok=store.Create(next,error);if(ok){active_id_=next;number_=last_wire_turn_=0;}
        }
        if(ok&&!active_id_)ok=store.Create(active_id_,error);
        if(ok){
            if(last_wire_turn_!=job.wire_turn){last_wire_turn_=job.wire_turn;++number_;}
            job.turn.number=number_;ok=store.Save(active_id_,job.turn,error);
            if(ok){result.active=active_id_;result.message=job.turn.status=="pending"?"原文已保存，等待回答":"对话已自动保存";}
        }
    }else if(job.kind==Kind::List){ok=store.List(result.sessions,error);result.message="选择历史会话，或新建对话";}
    else if(job.kind==Kind::Switch){
        std::vector<uint32_t> numbers;
        if(job.id){ok=store.Numbers(job.id,numbers,error);if(ok&&numbers.empty()){ok=false;error="会话没有可继续的内容";}
            if(ok){context=store.Context(job.id,error);ok=!context.empty();}}
        if(ok){active_id_=job.id;last_wire_turn_=0;number_=numbers.empty()?0:numbers.back();result.active=active_id_;result.message=job.id?"历史已打开，正在恢复最近上下文":"新对话，按住 AI 键开始";}
    }else if(job.kind==Kind::Delete){
        ok=store.Delete(job.id,error);if(ok){if(active_id_==job.id){active_id_=last_wire_turn_=number_=0;result.active=0;}
            result.turn.reset();result.selected=0;ok=store.List(result.sessions,error);result.message="会话已移出历史（SD 保留恢复目录）";}
    }else{
        const uint32_t id=job.kind==Kind::Open?job.id:result.selected;std::vector<uint32_t> numbers;
        ok=store.Numbers(id,numbers,error);if(ok&&numbers.empty()){ok=false;error="会话为空";}
        if(ok){size_t index=job.kind==Kind::Open?numbers.size()-1:job.direction<0?(result.turn_index?result.turn_index-1:0):result.turn_index+1;
            index=std::min(index,numbers.size()-1);Turn t;ok=store.Read(id,numbers[index],t,error);
            if(ok){result.selected=id;result.turn_index=index;result.turn_count=numbers.size();result.turn=std::make_shared<const Turn>(std::move(t));result.message="本地历史 / 用户与助手消息";}}
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(ok||job.kind!=Kind::Save){
            // A newer complete snapshot may replace a pending front while SD
            // writes run. Do not pop that newer version as though it was saved.
            const auto& front=queue_.front();
            const bool changed=job.kind==Kind::Save&&(front.turn.user!=job.turn.user||front.turn.assistant!=job.turn.assistant||front.turn.status!=job.turn.status||front.turn.truncated!=job.turn.truncated);
            if(!changed)queue_.pop_front();
            retry_at_=0;
        }else retry_at_=now+5000;
        if(job.kind==Kind::Switch){switching_=false;if(ok){resume_context_=std::move(context);resume_ready_=job.id!=0;}}
        if(job.kind==Kind::Delete)switching_=false;
        result.busy=std::any_of(queue_.begin(),queue_.end(),[](const auto& j){return j.kind!=Kind::Save;});
        result.pending=queue_.size();result.failed=!ok;if(!ok)result.message=error;
        result.revision=view_.revision+1;view_=std::move(result);
    }
    Application::GetInstance().RequestStatusUpdate(true);
}
}
