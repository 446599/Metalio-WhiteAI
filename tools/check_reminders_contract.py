#!/usr/bin/env python3
"""Exercise the actual reminder store and MCP parser on the host."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TEST = r'''
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <cJSON.h>
#include "reminders/reminder_store.h"
#include "reminders/presentation.h"
#include "xiaozhi/mcp_server.h"
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
const cJSON* get(const cJSON* o, const char* key) { return cJSON_GetObjectItemCaseSensitive(o,key); }
Json parse(const std::string& s) {
    Json j(cJSON_Parse(s.c_str()),cJSON_Delete); assert(j); return j;
}
Json result(const std::string& s, bool failed=false) {
    auto j=parse(s); assert(!get(j.get(),"error"));
    const auto* r=get(j.get(),"result"); assert(cJSON_IsObject(r));
    assert(cJSON_IsBool(get(r,"isError")) && cJSON_IsTrue(get(r,"isError"))==failed);
    const auto* text=get(cJSON_GetArrayItem(get(r,"content"),0),"text");
    assert(cJSON_IsString(text));
    return Json(cJSON_Duplicate(text,true),cJSON_Delete);
}
std::string call(int id, const char* name, const std::string& args="{}") {
    return "{\"jsonrpc\":\"2.0\",\"id\":"+std::to_string(id)+
        ",\"method\":\"tools/call\",\"params\":{\"name\":\""+name+"\",\"arguments\":"+args+"}}";
}
int64_t at(const char* s) { int64_t t; assert(reminders::ParseLocalTime(s,t)); return t; }
int main() {
    setenv("TZ","CST-8",1); tzset();
    const int64_t now=at("2025-01-01 08:00:00"); // Wednesday, midnight UTC
    assert(now==1735689600);
    assert(reminders::LocalTime(now)=="2025-01-01 08:00:00");
    reminders::Item shown; shown.at=now+60; shown.kind="alarm";
    assert(UpcomingOccurrence(shown,now)==now+60);
    shown.enabled=false; assert(UpcomingOccurrence(shown,now)==0);
    shown.snoozed_until=now+300; assert(UpcomingOccurrence(shown,now)==now+300);
    shown.snoozed_until=0; shown.kind="event";
    assert(OccurrenceOnDay(shown,now)==now+60); // Completed one-shot still visible in calendar.
    assert(OccurrenceOnDay(shown,now+86400)==0);
    shown.enabled=true; shown.weekdays=31;
    assert(UpcomingOccurrence(shown,now)==now+60);
    assert(OccurrenceOnDay(shown,at("2025-01-04 00:00:00"))==0); // Saturday.
    assert(OccurrenceOnDay(shown,at("2025-01-06 00:00:00"))==at("2025-01-06 08:01:00"));
    int64_t ignored;
    for (const char* invalid : {"2025-02-30 10:00", "2025-02-29 10:00", "2025-01-01 24:00", "2025-01-01 10:00:60", "2023-12-31 23:59", "2025-1-01 10:00"})
        assert(!reminders::ParseLocalTime(invalid,ignored));
    assert(reminders::ParseLocalTime("2024-02-29T10:00",ignored));
    assert(!reminders::ValidClock(0));

    std::string persisted; bool commit_ok=true;
    reminders::Store store([&](const std::string& s){ if(!commit_ok)return false; persisted=s; return true; });
    assert(store.Restore(""));
    int stops=0;
    xiaozhi::McpServer server(store,[&]{++stops;return true;});
    auto init=parse(server.Handle(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",now));
    assert(cJSON_IsObject(get(get(init.get(),"result"),"capabilities")));
    std::set<std::string> tools;
    for (int page=0;page<2;++page) {
        const auto payload=server.Handle(page==0 ? R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})" : R"({"jsonrpc":"2.0","id":3,"method":"tools/list","params":{"cursor":"3"}})",now);
        assert(payload.size()<4096);
        auto j=parse(payload); const auto* r=get(j.get(),"result");
        const auto* list=get(r,"tools"); assert(cJSON_GetArraySize(list)==3);
        assert((get(r,"nextCursor")!=nullptr)==(page==0));
        const cJSON* tool;
        cJSON_ArrayForEach(tool,list) tools.insert(get(tool,"name")->valuestring);
    }
    assert(tools.size()==6 && tools.count("self.reminders.update") && tools.count("self.reminders.stop"));
    auto clock=parse(result(server.Handle(call(4,"self.clock.get_time"),now))->valuestring);
    assert(cJSON_IsTrue(get(clock.get(),"valid")));
    assert(std::string(get(clock.get(),"now")->valuestring)=="2025-01-01 08:00:00");

    const auto request=call(5,"self.reminders.create",R"({"kind":"event","title":"晨会","after_seconds":60,"repeat":"weekdays"})");
    const auto created=server.Handle(request,now);
    auto item=parse(result(created)->valuestring);
    assert(get(item.get(),"id")->valueint==1 && store.List().size()==1);
    assert(server.Handle(request,now+20)==created && store.List().size()==1);
    assert(get(parse(server.Handle(call(5,"self.reminders.stop"),now)).get(),"error"));
    result(server.Handle(call(6,"self.reminders.update",R"({"id":1,"title":"新晨会","after_seconds":120,"duration_minutes":30})"),now));
    assert(store.List()[0].title=="新晨会" && store.List()[0].at==now+120);
    result(server.Handle(call(7,"self.reminders.update",R"({"id":1,"enabled":false})"),now));
    assert(!store.List()[0].enabled);
    result(server.Handle(call(8,"self.reminders.update",R"({"id":1,"enabled":true})"),now));
    result(server.Handle(call(9,"self.reminders.stop"),now)); assert(stops==1);
    assert(server.Handle(R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"self.reminders.stop"}})",now).empty());
    assert(stops==1);
    result(server.Handle(call(10,"self.reminders.list"),now));
    int id=20;
    for (const char* args : {
        R"({"kind":"alarm","title":"坏日期","when":"2025-02-30 10:00"})",
        R"({"kind":"alarm","title":"过期","when":"2025-01-01 07:59"})",
        R"({"kind":"alarm","title":"冲突","when":"2025-01-02 10:00","after_seconds":60})",
        R"({"kind":"alarm","title":"无时间"})",
        R"({"kind":"alarm","title":"负数","after_seconds":-1})",
        R"({"kind":"alarm","title":"小数","after_seconds":1.5})",
        R"({"kind":"alarm","title":"未知字段","after_seconds":60,"bad":true})",
        R"({"kind":"alarm","title":"重复字段","after_seconds":60,"after_seconds":70})",
        R"({"kind":"alarm","title":"空星期","after_seconds":60,"repeat":"custom","weekdays":[]})",
        R"({"kind":"alarm","title":"重复星期","after_seconds":60,"repeat":"custom","weekdays":[1,1]})"
    }) result(server.Handle(call(id++,"self.reminders.create",args),now),true);
    result(server.Handle(call(id++,"self.reminders.create",R"({"kind":"alarm","title":"未对时","after_seconds":60})"),0),true);
    result(server.Handle(call(id++,"self.reminders.delete",R"({"id":999})"),now),true);
    const auto before=persisted;
    commit_ok=false;
    result(server.Handle(call(id++,"self.reminders.update",R"({"id":1,"title":"不应保存"})"),now),true);
    result(server.Handle(call(id++,"self.reminders.delete",R"({"id":1})"),now),true);
    std::vector<reminders::Item> due;
    assert(!store.TakeDue(now+120,due) && due.empty());
    assert(persisted==before && store.List()[0].title=="新晨会");
    commit_ok=true;
    assert(store.TakeDue(now+120,due) && due.size()==1);
    assert(store.List()[0].at==at("2025-01-02 08:02:00"));
    assert(store.TakeDue(now+121,due) && due.empty());
    reminders::Store restored([](const std::string&){return true;});
    assert(restored.Restore(persisted));
    assert(restored.TakeDue(now+122,due) && due.empty());
    // Multi-day offline recovery still alerts the current occurrence once.
    assert(restored.TakeDue(at("2025-01-06 08:02:30"),due) && due.size()==1);
    assert(due[0].at==at("2025-01-06 08:02:00"));
    assert(restored.List()[0].at==at("2025-01-07 08:02:00"));
    assert(restored.TakeDue(at("2025-01-20 09:00:00"),due) && due.empty());
    assert(restored.List()[0].at==at("2025-01-21 08:02:00"));
    result(server.Handle(call(id++,"self.reminders.delete",R"({"id":1})"),now),false);
    assert(store.List().empty());

    reminders::Item one, saved; std::string error;
    one.title="单次"; one.at=now+60;
    assert(store.Put(one,now,saved,error));
    const auto assigned=saved.id;
    assert(store.Put(one,now,saved,error) && saved.id==assigned && store.List().size()==1);
    assert(store.TakeDue(now+59,due) && due.empty());
    assert(store.TakeDue(now+60,due) && due.size()==1 && !store.List()[0].enabled);
    assert(store.TakeDue(now+61,due) && due.empty());
    one.title="过时不补响"; one.at=now+120;
    assert(store.Put(one,now,saved,error));
    assert(store.TakeDue(now+421,due) && due.empty());
    for (size_t n=store.List().size();n<reminders::kMaxItems;++n) {
        one.title="容量"+std::to_string(n); one.at=now+3600+n;
        assert(store.Put(one,now,saved,error));
    }
    one.title="超限"; assert(!store.Put(one,now,saved,error));
    assert(!restored.Restore("{bad") && !restored.Ready());
    assert(!restored.Put(one,now,saved,error));
    auto corrupt=parse(persisted); cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(corrupt.get(),"next_id"),1);
    char* bad=cJSON_PrintUnformatted(corrupt.get()); assert(!restored.Restore(bad)); cJSON_free(bad);
    std::string snoozed_json;
    reminders::Store snoozed([&](const std::string& json){snoozed_json=json;return commit_ok;});
    assert(snoozed.Restore(""));
    reminders::Item recurring; recurring.title="每天八点"; recurring.at=now+60; recurring.weekdays=127;
    assert(snoozed.Put(recurring,now,saved,error)); const auto alarm_id=saved.id;
    assert(snoozed.TakeDue(now+60,due) && due.size()==1);
    const auto next_day=snoozed.List()[0].at;
    commit_ok=false;
    assert(!snoozed.Snooze({alarm_id},now+61,error) && snoozed.List()[0].snoozed_until==0);
    commit_ok=true;
    assert(snoozed.Snooze({alarm_id},now+61,error));
    assert(snoozed.List()[0].at==next_day && snoozed.List()[0].snoozed_until==now+361);
    assert(restored.Restore(snoozed_json));
    assert(restored.TakeDue(now+360,due) && due.empty());
    assert(restored.TakeDue(now+361,due) && due.size()==1);
    assert(restored.List()[0].at==next_day && restored.List()[0].snoozed_until==0);
    assert(restored.TakeDue(now+362,due) && due.empty());
    assert(restored.TakeDue(next_day,due) && due.size()==1);
    recurring.weekdays=0; recurring.id=0; recurring.title="单次稍后"; recurring.at=now+600;
    assert(snoozed.Put(recurring,now,saved,error)); const auto once_id=saved.id;
    assert(snoozed.TakeDue(now+600,due) && !due.empty());
    assert(snoozed.Snooze({once_id},now+600,error));
    assert(snoozed.TakeDue(now+900,due) && due.size()==1 && due[0].id==once_id);
    assert(!snoozed.Snooze({99999},now+900,error));
    std::puts("Reminders/MCP OK: Beijing time, six tools/two pages, CRUD, strict errors, retries, persistence rollback/reboot, once/repeat/offline due, durable snooze preserving repeat time, capacity");
}
'''

with tempfile.TemporaryDirectory(prefix="miaoink-reminders-") as directory:
    path = Path(directory)
    (path / "test.cc").write_text(TEST)
    cjson = ROOT / "managed_components/espressif__cjson/cJSON"
    subprocess.run(["cc", "-c", str(cjson / "cJSON.c"), "-I", str(cjson),
                    "-o", str(path / "cjson.o")], check=True)
    subprocess.run(["c++", "-std=c++17", "-O1", "-Wall", "-Wextra", "-fsanitize=undefined",
                    "-I", str(ROOT / "main"), "-I", str(cjson), str(path / "test.cc"),
                    str(ROOT / "main/reminders/reminder_store.cc"),
                    str(ROOT / "main/xiaozhi/mcp_server.cc"), str(ROOT / "main/xiaozhi/system_tools.cc"), str(ROOT / "main/notes/note_store.cc"), str(path / "cjson.o"),
                    "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
