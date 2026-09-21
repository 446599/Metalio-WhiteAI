// Synthetic fixtures only. This runs the actual stores, MCP dispatcher and
// conversation state; hardware/audio/network are not claimed to be simulated.
#include "notes/note_store.h"
#include "notes/snapshot_file.h"
#include "display/font/text_layout.h"
#include "xiaozhi/conversation.h"
#include "xiaozhi/memory_tools.h"
#include "xiaozhi/mcp_server.h"
#include <cJSON.h>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <set>
#include <thread>

static size_t checks=0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);std::abort(); } } while (false)
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
const cJSON* Get(const cJSON* root,const char* key) {return cJSON_GetObjectItemCaseSensitive(root,key);}
Json Parse(const std::string& text) {
    Json root(cJSON_ParseWithLengthOpts(text.c_str(),text.size()+1,nullptr,true),cJSON_Delete);
    CHECK(root!=nullptr);return root;
}
std::string Print(const cJSON* root) {
    char* raw=cJSON_PrintUnformatted(root);CHECK(raw!=nullptr);std::string s(raw);cJSON_free(raw);return s;
}
std::string Str(const cJSON* root,const char* key) {
    const auto* value=Get(root,key);CHECK(cJSON_IsString(value));return value->valuestring;
}
uint32_t Num(const cJSON* root,const char* key) {
    const auto* value=Get(root,key);CHECK(cJSON_IsNumber(value));return static_cast<uint32_t>(value->valuedouble);
}
std::string Request(int id,const char* tool,const std::string& args="{}") {
    return "{\"jsonrpc\":\"2.0\",\"id\":"+std::to_string(id)+",\"method\":\"tools/call\",\"params\":{\"name\":\""+tool+"\",\"arguments\":"+args+"}}";
}
std::string Ref(uint32_t id,uint32_t rev,const std::string& fields) {
    return "{\"id\":"+std::to_string(id)+",\"revision\":"+std::to_string(rev)+","+fields+"}";
}
notes::Note ArchiveRecord(unsigned n) {
    notes::Note note;char id[17];std::snprintf(id,sizeof(id),"%016x",n+1);
    note.title="记录";note.text=std::string(1536,'s');note.raw_text=std::string(1536,'r');
    note.raw_text[0]=static_cast<char>('A'+n);note.project="测试项目";note.source_id=id;
    note.source_revision=1;note.updated=1789950000;return note;
}
int main() {
    setenv("TZ","CST-8",1);tzset();int64_t now=0;
    CHECK(reminders::ParseLocalTime("2026-09-21 12:00",now));
    const std::string legacy=R"({"schema":1,"next":2,"items":[{"id":1,"title":"旧笔记","text":"保留旧数据","updated":1789950000}]})";
    bool fail=false;size_t writes=0;std::string disk,error;
    notes::Store store([&](const std::string& json){if(fail)return false;disk=json;++writes;return true;});
    CHECK(store.Restore(legacy));CHECK(store.List().size()==1);CHECK(store.List()[0].created==1789950000);
    CHECK(store.List()[0].revision==1 && store.List()[0].raw_text.empty());
    CHECK(notes::DisplayBody(store.List()[0])=="保留旧数据");
    reminders::Store reminders([](const auto&){return true;});CHECK(reminders.Restore(""));
    xiaozhi::SystemTools tools(reminders,store,[](const auto&){return xiaozhi::ToolReply{true,"{}"};});
    xiaozhi::McpServer server(reminders,[]{return true;},&tools);int request_id=0;
    auto call_text=[&](const char* name,const std::string& args="{}",bool ok=true) {
        const auto reply=Parse(server.Handle(Request(++request_id,name,args),now));
        CHECK(!Get(reply.get(),"error"));const auto* result=Get(reply.get(),"result");CHECK(result);
        CHECK(cJSON_IsTrue(Get(result,"isError"))==!ok);
        return Str(cJSON_GetArrayItem(Get(result,"content"),0),"text");
    };
    auto call=[&](const char* name,const std::string& args="{}") {return Parse(call_text(name,args));};
    std::set<std::string> names;std::string cursor;size_t pages=0;
    do {
        auto reply=Parse(server.Handle("{\"jsonrpc\":\"2.0\",\"id\":"+std::to_string(++request_id)+
            ",\"method\":\"tools/list\",\"params\":{\"cursor\":\""+cursor+"\"}}",now));
        const auto* result=Get(reply.get(),"result");CHECK(result);const auto* list=Get(result,"tools");
        CHECK(cJSON_GetArraySize(list)==static_cast<int>(std::min<size_t>(3,26-names.size())));
        const cJSON* tool;cJSON_ArrayForEach(tool,list) {
            CHECK(names.insert(Str(tool,"name")).second);CHECK(cJSON_IsObject(Get(tool,"inputSchema")));
        }
        cursor=Get(result,"nextCursor") ? Str(result,"nextCursor") : "";++pages;
    } while (!cursor.empty());
    CHECK(names.size()==26 && pages==9 && xiaozhi::SystemTools::Count()==20);
    CHECK(names.count("self.memory.archive")==1 && !xiaozhi::MemoryToolSchema(5));

    auto& conversation=xiaozhi::Conversation::GetInstance();conversation.Clear();
    call_text("self.memory.source",R"({"which":"previous"})",false);
    const std::string raw="卡框磁铁孔采用暂停打印嵌入，ESP32-S3 记录不丢失。";
    conversation.Begin();conversation.SetTranscript(raw.c_str());
    conversation.ReceiveAnswer("先做试装件，再验证公差。",true);conversation.SetState(xiaozhi::TurnState::Done);
    conversation.Begin();conversation.SetTranscript("把刚才的想法归档到卡框项目");
    auto source=call("self.memory.source",R"({"which":"previous"})");
    CHECK(Str(source.get(),"raw_text")==raw);CHECK(cJSON_IsTrue(Get(source.get(),"archive_storage_ready")));
    call_text("self.memory.source",R"({"which":"current"})",false);
    CHECK(conversation.Snapshot().transcript!=conversation.PreviousCompleted().transcript);
    Json archive(cJSON_CreateObject(),cJSON_Delete);
    cJSON_AddStringToObject(archive.get(),"which","previous");
    cJSON_AddStringToObject(archive.get(),"source_id",Str(source.get(),"source_id").c_str());
    cJSON_AddNumberToObject(archive.get(),"source_revision",Num(source.get(),"source_revision"));
    cJSON_AddStringToObject(archive.get(),"title","卡框试装");cJSON_AddStringToObject(archive.get(),"text","先做试装件，再验证公差。");
    cJSON_AddStringToObject(archive.get(),"project","卡框");const auto args=Print(archive.get());
    fail=true;call_text("self.memory.archive",args,false);CHECK(store.List().size()==1 && writes==0);fail=false;
    auto result=call("self.memory.archive",args);const auto id=Num(result.get(),"id");
    CHECK(Str(result.get(),"status")=="saved" && writes==1 && id==2);
    const auto retry=Request(++request_id,"self.memory.archive",args);
    CHECK(server.Handle(retry,now)==server.Handle(retry,now));CHECK(writes==1);
    server.Reset();result=call("self.memory.archive",args);
    CHECK(Str(result.get(),"status")=="already_archived" && Num(result.get(),"id")==id && writes==1);
    auto changed_args=Parse(args);cJSON_ReplaceItemInObjectCaseSensitive(changed_args.get(),"project",cJSON_CreateString("不能静默覆盖"));
    result=call("self.memory.archive",Print(changed_args.get()));CHECK(Str(result.get(),"project")=="卡框" && writes==1);
    result=call("self.notes.read","{\"id\":"+std::to_string(id)+"}");
    CHECK(Str(result.get(),"raw_text")==raw && Str(result.get(),"text")=="先做试装件，再验证公差。");
    CHECK(Num(result.get(),"revision")==1 && Num(result.get(),"created_epoch")==now);
    const auto archived_disk=disk;notes::Store reboot([](const auto&){return true;});CHECK(reboot.Restore(disk));
    CHECK(reboot.List()[0].raw_text==raw && reboot.List()[0].project=="卡框");
    notes::Note duplicate;bool existed=false;
    CHECK(reboot.Archive(store.List()[0],duplicate,existed,error)==false); // id must not be supplied to Archive.
    auto copy=store.List()[0];copy.id=0;
    CHECK(reboot.Archive(copy,duplicate,existed,error) && existed && duplicate.id==id);
    const auto view=notes::DisplayBody(store.List()[0]);CHECK(view.find("整理\n先做试装件")!=std::string::npos);
    CHECK(view.find("原文\n"+raw)!=std::string::npos && view.find("项目：卡框")!=std::string::npos);

    CHECK(cJSON_GetArraySize(call("self.memory.search",R"({"query":"esp32-s3","project":"卡框","state":"open"})").get())==1);
    CHECK(cJSON_GetArraySize(call("self.memory.search",R"({"query":"磁铁孔"})").get())==1);
    CHECK(cJSON_GetArraySize(call("self.memory.search",R"({"project":"不存在"})").get())==0);
    call_text("self.notes.save","{\"id\":"+std::to_string(id)+",\"title\":\"不能改原文\",\"text\":\"替换\"}",false);
    call_text("self.memory.update",Ref(id,1,R"("raw_text":"伪造原文")"),false);
    result=call("self.memory.update",Ref(id,1,R"("project":"3D打印","done":true,"text":"已完成试装，下一步检查公差。")"));
    CHECK(Num(result.get(),"revision")==2 && cJSON_IsTrue(Get(result.get(),"done")));
    call_text("self.memory.update",Ref(id,1,R"("done":false)"),false);
    CHECK(cJSON_GetArraySize(call("self.memory.search",R"({"project":"3D打印","state":"done"})").get())==1);
    CHECK(cJSON_GetArraySize(call("self.memory.search",R"({"project":"3D打印","state":"open"})").get())==0);
    CHECK(Str(call("self.notes.read","{\"id\":"+std::to_string(id)+"}").get(),"raw_text")==raw);

    const auto reminder=call("self.reminders.create",R"({"kind":"alarm","title":"检查试装公差","after_seconds":600})");
    const auto reminder_id=Num(reminder.get(),"id");CHECK(reminders.List().size()==1);
    call_text("self.memory.link_reminder",Ref(id,2,R"("reminder_id":999)"),false);
    const auto link=Ref(id,2,"\"reminder_id\":"+std::to_string(reminder_id));
    fail=true;const auto old_disk=disk;call_text("self.memory.link_reminder",link,false);
    CHECK(disk==old_disk && reminders.List().size()==1 && store.List()[0].reminder_id==0);fail=false;
    result=call("self.memory.link_reminder",link);CHECK(Num(result.get(),"revision")==3);
    CHECK(Str(result.get(),"reminder_state")=="scheduled" && reminders.List()[0].enabled);
    CHECK(cJSON_IsTrue(Get(result.get(),"done"))); // done does not cancel the reminder.
    call_text("self.reminders.delete","{\"id\":"+std::to_string(reminder_id)+"}");
    result=call("self.notes.read","{\"id\":"+std::to_string(id)+"}");
    CHECK(Str(result.get(),"reminder_state")=="missing" && Num(result.get(),"reminder_id")==reminder_id);
    result=call("self.memory.link_reminder",Ref(id,3,R"("reminder_id":0)"));
    CHECK(Str(result.get(),"reminder_state")=="none");
    CHECK(reboot.Restore(disk));CHECK(reboot.List()[0].done && reboot.List()[0].raw_text==raw);

    // A newer turn, cancelled turn, or truncated source cannot archive the old token.
    conversation.SetState(xiaozhi::TurnState::Done);conversation.Begin();conversation.SetTranscript("新一轮");
    call_text("self.memory.archive",args,false);
    conversation.SetState(xiaozhi::TurnState::Error);conversation.Begin();
    call_text("self.memory.source",R"({"which":"previous"})",false);
    conversation.SetTranscript(std::string(1537,'x').c_str());conversation.SetState(xiaozhi::TurnState::Done);conversation.Begin();
    call_text("self.memory.source",R"({"which":"previous"})",false);
    conversation.Restore(raw,"先做试装件，再验证公差。");conversation.Begin();conversation.Clear();
    CHECK(conversation.PreviousCompleted().transcript.empty());

    // Strict types/fields, malformed UTF-8 and byte limits do not mutate state.
    const auto rev=store.Revision();
    for (const auto* bad:{R"({"state":"other"})",R"({"query":true})",R"({"project":null})",R"({"query":"x","query":"y"})",R"({"extra":1})"})
        call_text("self.memory.search",bad,false);
    for (const auto* bad:{R"({"id":2,"revision":4,"done":1})",R"({"id":2,"revision":0,"done":true})",R"({"id":2.5,"revision":4,"done":true})",R"({"id":2,"revision":4,"text":null})",R"({"id":2,"revision":4})"})
        call_text("self.memory.update",bad,false);
    CHECK(store.Revision()==rev);
    CHECK(!notes::ValidText(std::string("\xc0\xaf",2),10));CHECK(!notes::ValidText(std::string("\xed\xa0\x80",3),10));
    CHECK(!notes::ValidText(std::string("a\0b",3),10));CHECK(!notes::ValidText(std::string("\xf4\x90\x80\x80",4),10));
    CHECK(notes::ValidText("中文🙂",10));CHECK(!notes::ValidText("中文🙂",9));
    CHECK(notes::ValidText("literal \\u0000",30));

    // Transport must reject encoded NUL before cJSON can expose a truncated prefix.
    const auto prior=store.List().size();
    CHECK(server.Handle(Request(++request_id,"self.notes.save",R"({"title":"bad\u0000suffix","text":"x"})"),now).empty());
    CHECK(store.List().size()==prior);
    auto literal=call("self.notes.save",R"({"title":"literal \\u0000","text":"不是空字符"})");
    CHECK(Str(literal.get(),"title")=="literal \\u0000");

    // Capacity includes ordinary notes; a retry at capacity still finds its source.
    for (unsigned n=static_cast<unsigned>(store.List().size());n<8;++n) call("self.notes.save",R"({"title":"普通记录","text":"内容"})");
    call_text("self.notes.save",R"({"title":"第九条","text":"不能挤掉旧记录"})",false);CHECK(store.List().size()==8);
    CHECK(store.Archive(copy,duplicate,existed,error) && existed && duplicate.id==id);
    notes::Store unavailable([](const auto&){return true;});
    CHECK(!unavailable.Archive(copy,duplicate,existed,error));

    std::string large_disk;notes::Store large([&](const auto& value){large_disk=value;return true;});CHECK(large.Restore(""));
    for (unsigned n=0;n<8;++n) CHECK(large.Archive(ArchiveRecord(n),duplicate,existed,error) && !existed);
    CHECK(large_disk.size()>20000 && large_disk.size()<notes::Store::kSnapshotBytes);
    CHECK(reboot.Restore(large_disk) && reboot.List().size()==8 && reboot.List()[0].raw_text.size()==1536);
    auto collision=ArchiveRecord(0);collision.raw_text="另一个原文";
    CHECK(!large.Archive(collision,duplicate,existed,error));
    auto too_long=ArchiveRecord(8);too_long.raw_text+= 'x';CHECK(!large.Archive(too_long,duplicate,existed,error));
    for (const auto* broken:{R"({"schema":3,"next":1,"items":[]})",R"({"schema":2,"schema":2,"next":1,"items":[]})",
         R"({"schema":1,"next":2,"items":[{"id":1,"title":"x\u0000y","text":"x","updated":0}]})",
         R"({"schema":2,"next":2,"items":[{"id":1,"title":"x","text":"x","updated":0}]})"}) {
        CHECK(!reboot.Restore(broken) && !reboot.Ready());CHECK(!reboot.Put({0,"x","x",now},duplicate,error));
    }
    CHECK(!reboot.Restore(std::string(notes::Store::kSnapshotBytes+1,' ')));

    // Real files: schema-1 -> schema-2 migration, readback, torn newest recovery,
    // dual corruption protection, and an unsuccessful re-load must disable writes.
    char directory[]="/tmp/whiteai-memory-XXXXXX";CHECK(mkdtemp(directory)!=nullptr);
    const std::string path=std::string(directory)+"/notes";notes::SnapshotFile file(path);std::string restored;
    const auto valid=[](const std::string& value){notes::Store s([](const auto&){return true;});return s.Restore(value);};
    CHECK(file.Load(restored,valid) && restored.empty());CHECK(file.Save(legacy));CHECK(file.Save(large_disk));
    notes::SnapshotFile restart(path);CHECK(restart.Load(restored,valid) && restored==large_disk);
    {std::ofstream out(path+".1",std::ios::trunc);out<<"torn";}
    CHECK(restart.Load(restored,valid) && restored==legacy);CHECK(restart.Save(archived_disk));
    CHECK(file.Load(restored,valid) && restored==archived_disk);
    {std::ofstream out(path+".0",std::ios::trunc);out<<"bad";}
    {std::ofstream out(path+".1",std::ios::trunc);out<<"bad";}
    CHECK(!file.Load(restored,valid));CHECK(!file.Save(archived_disk));
    CHECK(std::remove((path+".0").c_str())==0);CHECK(std::remove((path+".1").c_str())==0);CHECK(rmdir(directory)==0);

    // Exercise actual pagination; synthetic widths test the row algorithm,
    // not glyph assets or physical display rendering. Preserve short-text Wrap.
    const auto measure=[](uint32_t cp){return cp<128 ? 8 : 16;};
    for (const auto& text:std::vector<std::string>{"","\n","\n\n","abc\r\ndef\n", "中文🙂 mixed\n最后一行",std::string(1200,'a')}) {
        const auto old=raw_font::Wrap(text,80,measure);
        std::vector<std::string> joined;
        auto page=raw_font::Paginate(text,80,0,12,measure);
        for (size_t i=0;i<page.pages;++i) {
            auto part=raw_font::Paginate(text,80,i,12,measure);
            CHECK(part.lines.size()<=12 && part.page==i);
            joined.insert(joined.end(),part.lines.begin(),part.lines.end());
        }
        CHECK(joined==old);
    }
    auto newline_note=ArchiveRecord(0);
    newline_note.text=std::string(1536,'\n');newline_note.raw_text=std::string(1535,'\n')+"Z";
    const auto body=notes::DisplayBody(newline_note);
    const auto last=raw_font::Paginate(body,416,SIZE_MAX,12,measure);
    CHECK(last.pages>200 && last.page==last.pages-1 && last.lines.size()<=12);
    CHECK(last.lines.back()=="Z");
    CHECK(raw_font::Paginate("a",0,0,0,measure).pages==1);
    CHECK(raw_font::Paginate("a",0,100,1000,measure).lines.size()==1);

    // Concurrent retries share a single committed record; optimistic edits cannot
    // both succeed at the same revision. No assertions from worker threads.
    std::atomic<unsigned> committed{0},success{0},duplicates{0};
    notes::Store concurrent([&](const auto&){++committed;return true;});CHECK(concurrent.Restore(""));
    std::vector<std::thread> workers;
    for (int i=0;i<8;++i) workers.emplace_back([&]{notes::Note saved;bool old=false;std::string e;
        if(concurrent.Archive(ArchiveRecord(0),saved,old,e)){++success;if(old)++duplicates;}});
    for (auto& worker:workers) worker.join();
    CHECK(success==8 && duplicates==7 && committed==1 && concurrent.List().size()==1);
    workers.clear();success=0;
    for (int i=0;i<2;++i) workers.emplace_back([&]{notes::Patch patch;patch.done=true;notes::Note saved;std::string e;
        if(concurrent.Update(1,1,patch,now,saved,e))++success;});
    for (auto& worker:workers) worker.join();
    CHECK(success==1 && concurrent.List()[0].revision==2);
    std::printf("Memory contract PASS: %zu checks; cJSON %s; real MCP 26 tools/9 pages; source isolation, archive idempotence, immutable raw text, migration, search, revision conflicts, reminder links, rollback, file recovery and concurrent retries\n",checks,cJSON_Version());
}
