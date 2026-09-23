#!/usr/bin/env python3
"""Run real MCP system parsing, notes persistence and operation queue on the host."""
from pathlib import Path
import subprocess,tempfile
from host_cjson import cjson_flags
ROOT=Path(__file__).resolve().parents[1]
TEST=r'''
#include "xiaozhi/mcp_server.h"
#include "xiaozhi/system_tools.h"
#include "system/action_queue.h"
#include "notes/snapshot_file.h"
#include <fstream>
#include <cassert>
#include <cJSON.h>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <set>
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
const cJSON* get(const cJSON* o,const char* k){return cJSON_GetObjectItemCaseSensitive(o,k);}
Json parse(const std::string& s){Json j(cJSON_Parse(s.c_str()),cJSON_Delete);assert(j);return j;}
std::string call(int id,const char* name,const std::string& args="{}") {
 return "{\"jsonrpc\":\"2.0\",\"id\":"+std::to_string(id)+",\"method\":\"tools/call\",\"params\":{\"name\":\""+name+"\",\"arguments\":"+args+"}}";
}
int main(){
 setenv("TZ","CST-8",1);tzset();int64_t now;assert(reminders::ParseLocalTime("2026-09-19 08:00",now));
 bool fail=false;std::string disk;notes::Store notes([&](const auto& value){if(fail)return false;disk=value;return true;});assert(notes.Restore(""));
 reminders::Store reminders([](const auto&){return true;});assert(reminders.Restore(""));
 int hardware_calls=0;xiaozhi::SystemCommand last;
 xiaozhi::SystemTools tools(reminders,notes,[&](const auto& command){++hardware_calls;last=command;return xiaozhi::ToolReply{true,"{\"status\":\"queued\",\"id\":1}"};});
 xiaozhi::McpServer server(reminders,[]{return true;},&tools);
 int id=0;
 auto result=[&](const char* name,const std::string& args="{}",bool failed=false){
  auto reply=parse(server.Handle(call(++id,name,args),now));assert(!get(reply.get(),"error"));
  const auto* r=get(reply.get(),"result");assert(cJSON_IsTrue(get(r,"isError"))==failed);
  const auto* text=get(cJSON_GetArrayItem(get(r,"content"),0),"text");assert(cJSON_IsString(text));return std::string(text->valuestring);
 };
 std::set<std::string> names;std::string cursor;int pages=0;
 do {
  auto r=parse(server.Handle("{\"jsonrpc\":\"2.0\",\"id\":"+std::to_string(++id)+",\"method\":\"tools/list\",\"params\":{\"cursor\":\""+cursor+"\"}}",now));
  assert(!get(r.get(),"error"));const auto* page=get(r.get(),"result");const auto* list=get(page,"tools");assert(cJSON_GetArraySize(list)==static_cast<int>(std::min(size_t{3},size_t{27}-names.size())));
  const cJSON* tool; cJSON_ArrayForEach(tool,list){assert(names.insert(get(tool,"name")->valuestring).second);assert(cJSON_IsObject(get(tool,"inputSchema")));}
  const auto* next=get(page,"nextCursor");cursor=next?next->valuestring:"";++pages;
 } while(!cursor.empty());
 assert(pages==9 && names.size()==27);
 for (const char* bad:{"1","03","27","-3","3x","999999999"}) {
  auto reply=parse(server.Handle("{\"jsonrpc\":\"2.0\",\"id\":"+std::to_string(++id)+",\"method\":\"tools/list\",\"params\":{\"cursor\":\""+bad+"\"}}",now));assert(get(reply.get(),"error"));
 }
 for (const char* bad:{"{}","{\"volume\":-1}","{\"volume\":101}","{\"volume\":0.5}","{\"volume\":true}","{\"volume\":\"50\"}","{\"volume\":50,\"extra\":1}","{\"volume\":1,\"volume\":2}"}) result("self.device.set_volume",bad,true);
 assert(hardware_calls==0);
 const auto request=call(++id,"self.device.set_volume","{\"volume\":50}");const auto reply=server.Handle(request,now);
 assert(reply==server.Handle(request,now) && hardware_calls==1 && last.number==50);
 assert(server.Handle("{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"params\":{\"name\":\"self.device.set_volume\",\"arguments\":{\"volume\":0}}}",now).empty());assert(hardware_calls==1);
 for(const char* app:{"home","alarm","calendar","recorder","assistant","voice_note","notes","capsules","apps","device","status"}) result("self.display.open",std::string("{\"app\":\"")+app+"\"}");
 result("self.display.open","{\"app\":\"factory_reset\"}",true);
 result("self.display.calendar","{\"date\":\"2028-02-29\"}");assert(last.value=="2028-02-29");
 result("self.display.calendar","{\"date\":\"2026-02-29\"}",true);
 for(const char* action:{"start","stop","play"}) result("self.recorder.control",std::string("{\"action\":\"")+action+"\"}");
 result("self.recorder.control","{\"action\":\"upload\"}",true);
 assert(result("self.notes.list")=="[]");
 const auto save_request=call(++id,"self.notes.save","{\"title\":\"购物清单\",\"text\":\"牛奶\\n面包\"}");
 const auto saved_reply=server.Handle(save_request,now);assert(saved_reply==server.Handle(save_request,now));assert(notes.List().size()==1);
 auto saved=notes.List()[0];const auto note_id=std::to_string(saved.id);
 assert(parse(result("self.notes.read","{\"id\":"+note_id+"}")));
 result("self.notes.save","{\"id\":"+note_id+",\"title\":\"购物清单\",\"text\":\"牛奶和水果\"}");
 assert(notes.List()[0].text=="牛奶和水果");
 fail=true;const auto revision=notes.Revision();result("self.notes.save","{\"id\":"+note_id+",\"title\":\"bad\",\"text\":\"bad\"}",true);
 result("self.notes.delete","{\"id\":"+note_id+"}",true);assert(notes.Revision()==revision && notes.List()[0].text=="牛奶和水果");fail=false;
 notes::Store reboot([](const auto&){return true;});assert(reboot.Restore(disk));assert(reboot.List()[0].text=="牛奶和水果");
 assert(!reboot.Restore("broken") && !reboot.Ready());notes::Note n,input;input.title="x";input.text="x";input.updated=now;std::string error;assert(!reboot.Put(input,n,error));
 for(int i=1;i<8;++i) result("self.notes.save","{\"title\":\"备忘\",\"text\":\"内容\"}");
 result("self.notes.save","{\"title\":\"多余\",\"text\":\"内容\"}",true);assert(notes.List().size()==8);
 result("self.notes.delete","{\"id\":"+note_id+"}");assert(notes.List().size()==7);
 result("self.notes.save","{\"title\":\"\",\"text\":\"x\"}",true);
 result("self.notes.save","{\"title\":\"x\",\"text\":\""+std::string(1537,'a')+"\"}",true);
 const auto focus_request=call(++id,"self.focus.start");assert(server.Handle(focus_request,now)==server.Handle(focus_request,now));
 assert(reminders.List().size()==1 && reminders.List()[0].at==now+25*60 && reminders.List()[0].kind=="alarm");
 result("self.focus.start","{\"minutes\":0}",true);result("self.focus.start","{\"minutes\":181}",true);
 std::vector<reminders::Item> due;assert(reminders.TakeDue(now+1500,due) && due.size()==1);
 using Kind=xiaozhi::SystemCommand::Kind;using State=device::ActionState;
 device::ActionQueue queue;xiaozhi::SystemCommand command;command.kind=Kind::Record;
 const auto cancelled=queue.Add(command,0);assert(queue.Cancel(cancelled));assert(!queue.Begin(cancelled,1));assert(queue.Get(cancelled,1)->state==State::Cancelled);
 const auto expired=queue.Add(command,0);assert(!queue.Begin(expired,30000));assert(queue.Get(expired,30000)->state==State::Expired);
 const auto running=queue.Add(command,30001);assert(queue.Begin(running,30002));assert(!queue.Cancel(running));queue.CancelRecordings();queue.Finish(running,true,"started");assert(queue.Get(running,30003)->state==State::Cancelled);
 device::ActionQueue full;std::vector<uint32_t> ids;
 for(int i=0;i<8;++i) ids.push_back(full.Add(command,0));
 assert(!full.Add(command,0));
 assert(full.Begin(ids[0],1));full.Finish(ids[0],true,"done");assert(full.Add(command,2));
 assert(!full.Get(ids[0],3));assert(full.Pending(30002).empty());
 char folder[]="/tmp/miaoink-notes-XXXXXX";assert(mkdtemp(folder));const std::string base=std::string(folder)+"/snapshot";
 auto valid=[](const std::string& value){notes::Store s([](const auto&){return true;});return s.Restore(value);};
 notes::SnapshotFile file(base);std::string restored;assert(file.Load(restored,valid) && restored.empty());
 assert(file.Save(disk));const auto previous=disk;
 result("self.notes.save","{\"title\":\"新记录\",\"text\":\"写入下一份快照\"}");assert(file.Save(disk));
 notes::SnapshotFile restart(base);assert(restart.Load(restored,valid) && restored==disk);
 // A torn newest slot restores the older complete snapshot.
 {std::ofstream corrupt(base+".1",std::ios::binary|std::ios::trunc);corrupt<<"partial";}
 notes::SnapshotFile fallback(base);assert(fallback.Load(restored,valid) && restored==previous);
 assert(fallback.Save(disk));notes::SnapshotFile recovered(base);assert(recovered.Load(restored,valid) && restored==disk);
 {std::ofstream corrupt(base+".0",std::ios::binary|std::ios::trunc);corrupt<<"bad";}
 {std::ofstream corrupt(base+".1",std::ios::binary|std::ios::trunc);corrupt<<"bad";}
 notes::SnapshotFile damaged(base);assert(!damaged.Load(restored,valid));assert(!damaged.Save(disk));
 std::remove((base+".0").c_str());std::remove((base+".1").c_str());rmdir(folder);
 std::puts("System MCP OK: 27 tools / 9 pages, strict dispatch, idempotent retries, notifications, note CRUD / reboot / rollback / bounds, focus due, queue capacity / cancellation / expiry");
}
'''
with tempfile.TemporaryDirectory(prefix='miaoink-system-') as directory:
 p=Path(directory);(p/'test.cc').write_text(TEST)
 subprocess.run(['c++','-std=c++17','-O1','-Wall','-Wextra','-fsanitize=undefined','-I',str(ROOT/'main'),str(p/'test.cc'),*[str(ROOT/'main'/name) for name in ['reminders/reminder_store.cc','notes/note_store.cc','xiaozhi/system_tools.cc','xiaozhi/mcp_server.cc','xiaozhi/conversation.cc','xiaozhi/memory_tools.cc']],*cjson_flags(p),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
