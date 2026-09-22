#include "chat/history_service.h"
#include "xiaozhi/chat_task.h"
#include "notes/note_store.h"
#include "notes/snapshot_file.h"
#include "test_platform.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
namespace fs=std::filesystem;
static unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::cerr<<"CHECK "<<__LINE__<<": "<<#x<<"\n";std::abort();}}while(0)
static void damage(const fs::path& p){std::ofstream(p,std::ios::binary|std::ios::trunc)<<"broken";}
int main(int argc,char** argv){
    CHECK(argc==2);const fs::path base=argv[1];fs::create_directories(base);
    chat::Store store((base/"disk").string());std::string error;std::vector<chat::Summary> list;
    CHECK(store.List(list,error)&&list.empty());uint32_t id=0;CHECK(store.Create(id,error)&&id==1);
    chat::Turn t{1,123,"你好，我的计划","","","pending",false},read;
    CHECK(store.Save(id,t,error));CHECK(store.Read(id,1,read,error)&&read.status=="pending");
    t.user="你好，我的完整计划";t.assistant="先记录，再完成";t.status="complete";
    CHECK(store.Save(id,t,error));CHECK(store.Read(id,1,read,error)&&read.assistant==t.assistant);
    auto conflict=t;conflict.user="不能改掉原问答";CHECK(!store.Save(id,conflict,error));
    CHECK(store.Save(id,t,error)); // Retries overwrite the same numbered turn, not append.
    std::vector<uint32_t> nums;CHECK(store.Numbers(id,nums,error)&&nums==std::vector<uint32_t>{1});
    auto pending=t;pending.assistant.clear();pending.status="pending";CHECK(store.Save(id,pending,error));
    CHECK(store.Read(id,1,read,error)&&read.assistant==t.assistant);
    for(int n=2;n<=4;++n){t.number=n;t.user="问题"+std::to_string(n);t.assistant="回答"+std::to_string(n);CHECK(store.Save(id,t,error));}
    CHECK(store.List(list,error)&&list.size()==1&&list[0].turns==4&&list[0].title=="你好，我的完整计划");
    const auto context=store.Context(id,error);CHECK(context.find("问题2")==std::string::npos);CHECK(context.find("问题3")!=std::string::npos&&context.find("回答4")!=std::string::npos);
    chat::Store reopened((base/"disk").string());CHECK(reopened.Read(id,4,read,error)&&read.user=="问题4");
    // A torn newest slot falls back to the previous CRC-valid one; both corrupt
    // slots refuse writes rather than clearing the user's history.
    t.number=4;t.assistant="新版回答";CHECK(store.Save(id,t,error));damage(base/"disk/00000001/00000004.1");
    CHECK(store.Read(id,4,read,error)&&read.assistant=="回答4");damage(base/"disk/00000001/00000004.0");
    CHECK(!store.Save(id,t,error));CHECK(store.List(list,error)&&list[0].damaged);
    CHECK(!store.Read(0,1,read,error));CHECK(!store.Numbers(100000000,nums,error));
    for(const auto& bad:{std::string("\xc0\xaf",2),std::string("x\0y",3),std::string(1537,'x')}){t.user=bad;CHECK(!chat::Store::Valid(t));}
    t.user="合法";t.assistant=std::string(6145,'x');CHECK(!chat::Store::Valid(t));t.assistant.clear();t.status="invented";CHECK(!chat::Store::Valid(t));t.status="complete";
    t.number=129;CHECK(!chat::Store::Valid(t));t.number=1;t.action="任意执行";CHECK(!chat::Store::Valid(t));t.action="整理灵感";CHECK(chat::Store::Valid(t));
    uint32_t second=0;CHECK(store.Create(second,error)&&second==2);CHECK(store.Save(second,t,error));
    CHECK(store.Delete(second,error));CHECK(fs::is_directory(base/"disk/00000002.deleted"));CHECK(!store.Delete(second,error));
    uint32_t third=0;CHECK(store.Create(third,error)&&third==3); // No reuse after deleting.
    CHECK(notes::ValidText(chat::Store::Title(std::string(65,'a')+"汉"),66));
    // A host filesystem can contain links; unlike FatFs it must reject a
    // substituted root before creating a session outside the requested path.
    fs::create_directory_symlink(base/"disk",base/"linked-root");
    chat::Store linked((base/"linked-root").string());uint32_t linked_id=0;
    CHECK(!linked.Create(linked_id,error));CHECK(linked_id==0);
    // Fill a separate store to its documented cap without auto-eviction.
    chat::Store full((base/"full").string());for(unsigned i=0;i<64;++i){uint32_t n;CHECK(full.Create(n,error));}
    CHECK(!full.Create(third,error));CHECK(full.Delete(1,error));CHECK(full.Create(third,error)&&third==65);
    // Real task mailbox: strict generation/UTF-8 boundaries and full retrieval.
    auto& task=xiaozhi::ChatTask::Instance();task.Cancel();CHECK(!task.Read(0,0).ok);
    std::string body;for(int i=0;i<2000;++i)body+="汉";
    CHECK(task.Begin("翻译英文",body));CHECK(!task.ReadAll());CHECK(!task.Read(0,1).ok);
    auto chunk=task.Read(0,0);CHECK(chunk.ok&&chunk.next%3==0&&!task.ReadAll());const auto oldid=chunk.id;std::string joined=chunk.text;
    CHECK(!task.Read(chunk.id,chunk.next+3).ok);CHECK(!task.Read(chunk.id,1).ok);
    while(chunk.next<chunk.total){chunk=task.Read(chunk.id,chunk.next);CHECK(chunk.ok&&notes::ValidText(chunk.text,2048));joined+=chunk.text;}
    CHECK(joined==body&&task.ReadAll());CHECK(task.Begin("待办草稿","内容"));CHECK(!task.Read(oldid,0).ok);task.Cancel();CHECK(!task.ReadAll());CHECK(!task.Begin("操作",std::string("a\0b",3)));
    // Actual worker and store, with only SD presence/UI notification mocked.
    auto& history=chat::History::Instance();xiaozhi::ConversationSnapshot snapshot;
    snapshot.turn=10;snapshot.transcript="自动保存第一轮";snapshot.answer="答案一";
    CHECK(history.Capture(snapshot,"","pending"));snapshot.answer="完整答案一";CHECK(history.Capture(snapshot,"","complete"));
    CHECK(history.Snapshot().pending==1);CHECK(!history.Switch(0));
    GetHAL().sd_probe=[&](){CHECK(history.Snapshot().busy);CHECK(!history.List());};
    history.Poll(100);GetHAL().sd_probe={};
    CHECK(history.Snapshot().pending==0&&!history.Snapshot().failed&&history.Snapshot().active==1);
    CHECK(history.List());history.Poll(101);CHECK(history.Snapshot().sessions.size()==1);
    CHECK(history.Open(1));CHECK(!history.Snapshot().turn);history.Poll(102);CHECK(history.Snapshot().turn->assistant=="完整答案一");
    snapshot.turn=11;snapshot.transcript="第二轮";snapshot.answer="答案二";CHECK(history.Capture(snapshot,"翻译英文","complete"));history.Poll(103);
    CHECK(history.Open(1));history.Poll(104);CHECK(history.Snapshot().turn_count==2&&history.Snapshot().turn_index==1);
    CHECK(history.Move(-1));history.Poll(105);CHECK(history.Snapshot().turn->user=="自动保存第一轮");
    CHECK(history.Switch(1));CHECK(history.Switching()&&!history.CanCapture());history.Poll(106);std::string resumed;
    CHECK(history.TakeResumed(resumed)&&resumed.find("答案二")!=std::string::npos);CHECK(!history.TakeResumed(resumed));
    CHECK(history.Switch(1));history.Poll(106);history.CancelResume();CHECK(!history.TakeResumed(resumed));
    CHECK(history.Switch(0));history.Poll(107);CHECK(history.Snapshot().active==0);
    GetHAL().sd=false;snapshot.turn=12;snapshot.transcript="断卡不丢失";snapshot.answer="临时答案";
    CHECK(history.Capture(snapshot,"","pending"));history.Poll(108);CHECK(history.Snapshot().failed&&history.Snapshot().pending==1);
    snapshot.answer="断卡后完成";CHECK(history.Capture(snapshot,"","complete"));GetHAL().sd=true;history.Retry();history.Poll(109);
    CHECK(!history.Snapshot().failed&&history.Snapshot().pending==0&&history.Snapshot().active==2);
    CHECK(history.Open(2));history.Poll(110);CHECK(history.Snapshot().turn->assistant=="断卡后完成");
    CHECK(history.Delete(1));history.Poll(111);CHECK(history.Snapshot().sessions.size()==1&&history.Snapshot().active==2);
    // Bounded queue protects audio heap. Refuse a ninth turn rather than evicting.
    GetHAL().sd=false;for(unsigned i=0;i<8;++i){snapshot.turn=20+i;CHECK(history.Capture(snapshot,"","complete"));}
    CHECK(!history.CanCapture());snapshot.turn=40;CHECK(!history.Capture(snapshot,"","complete"));
    GetHAL().sd=true;history.Retry();for(int i=0;i<8;++i)history.Poll(200+i);CHECK(history.CanCapture()&&history.Snapshot().pending==0);
    std::cout<<"Chat history PASS: "<<checks<<" assertions; actual disk/CRC/restart/queue/MCP task code; hardware mocked\n";
}
