#include "system_tools.h"
#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <initializer_list>

namespace xiaozhi {
namespace {
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
struct Spec { const char* name; const char* schema; };
constexpr Spec specs[]={
{"self.device.get_status",R"({"name":"self.device.get_status","description":"读取本机电量、充电、音量、网络、当前应用和录音状态。用于设备管家与状态问答，不返回凭据。","inputSchema":{"type":"object","properties":{},"additionalProperties":false}})"},
{"self.device.set_volume",R"({"name":"self.device.set_volume","description":"设置扬声器音量0到100。返回操作ID；queued只表示已排队，需action_status确认完成。","inputSchema":{"type":"object","properties":{"volume":{"type":"integer","minimum":0,"maximum":100}},"required":["volume"],"additionalProperties":false}})"},
{"self.display.open",R"({"name":"self.display.open","description":"打开本机应用。home首页，alarm闹钟，calendar日历，recorder录音，assistant小智，voice_note语音文字笔记，notes AI笔记，capsules最近胶囊，reader阅读，apps应用目录，device设备选项，status设备状态。响铃时拒绝覆盖。返回操作ID，用action_status确认。","inputSchema":{"type":"object","properties":{"app":{"type":"string","enum":["home","alarm","calendar","recorder","assistant","voice_note","notes","capsules","reader","apps","device","status"]}},"required":["app"],"additionalProperties":false}})"},
{"self.display.calendar",R"({"name":"self.display.calendar","description":"打开指定北京时间日期的月历和日程。date格式YYYY-MM-DD，可用于查看明天或下周某日，界面支持当前月份前后10年。","inputSchema":{"type":"object","properties":{"date":{"type":"string"}},"required":["date"],"additionalProperties":false}})"},
{"self.dashboard.get",R"({"name":"self.dashboard.get","description":"读取设备已获取的天气、额度和更新时间/新鲜度。晨间简报可组合clock、reminders.list和notes.list；没有数据时不要编造天气。不是任意城市搜索或天气预报。","inputSchema":{"type":"object","properties":{},"additionalProperties":false}})"},
{"self.dashboard.refresh",R"({"name":"self.dashboard.refresh","description":"请求后台更新设备天气和额度；requested不表示获取成功，之后用dashboard.get读取真实更新时间和状态。","inputSchema":{"type":"object","properties":{},"additionalProperties":false}})"},
{"self.recorder.get_status",R"({"name":"self.recorder.get_status","description":"查询本地录音状态、时长、是否有最近录音以及SD保存结果。","inputSchema":{"type":"object","properties":{},"additionalProperties":false}})"},
{"self.recorder.control",R"({"name":"self.recorder.control","description":"按用户明确要求控制本地录音。start开始新录音（替换最近一段，最长30秒），stop停止，play回放最近录音。start/play等待小智说完且音频空闲后执行，不上传音频。queued只表示排队，使用action_status查询结果；可cancel_action取消尚未开始的操作。","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["start","stop","play"]}},"required":["action"],"additionalProperties":false}})"},
{"self.system.action_status",R"({"name":"self.system.action_status","description":"查询异步系统操作结果：queued/running/succeeded/failed/cancelled/expired。只有succeeded才能说明对应操作已经执行；录音文件是否保存请再查recorder.get_status。","inputSchema":{"type":"object","properties":{"id":{"type":"integer","minimum":1}},"required":["id"],"additionalProperties":false}})"},
{"self.system.cancel_action",R"({"name":"self.system.cancel_action","description":"取消尚未执行的系统操作。已开始录音需使用recorder.control stop，不会通过此工具停止已运行任务。","inputSchema":{"type":"object","properties":{"id":{"type":"integer","minimum":1}},"required":["id"],"additionalProperties":false}})"},
{"self.notes.list",R"({"name":"self.notes.list","description":"查询本地AI笔记目录（最多8条），返回ID、标题、更新时间，不返回全文。可保存备忘、清单、晨间计划或学习摘记。","inputSchema":{"type":"object","properties":{},"additionalProperties":false}})"},
{"self.notes.read",R"({"name":"self.notes.read","description":"读取指定ID的本地AI笔记全文。先list确认ID。","inputSchema":{"type":"object","properties":{"id":{"type":"integer","minimum":1}},"required":["id"],"additionalProperties":false}})"},
{"self.notes.save",R"({"name":"self.notes.save","description":"真正持久保存文字笔记到本机SD卡（需已挂载）。无id新建；有id更新整条笔记，先read保留原内容。标题最多96个UTF-8字节，正文最多1536字节；适合购物清单、工作备忘、日记与学习卡片。成功后可display.open notes在屏幕查看。","inputSchema":{"type":"object","properties":{"id":{"type":"integer","minimum":1},"title":{"type":"string","maxLength":32},"text":{"type":"string","maxLength":512}},"required":["title","text"],"additionalProperties":false}})"},
{"self.notes.delete",R"({"name":"self.notes.delete","description":"按用户要求删除指定ID的AI笔记。先list或read确认，不能猜ID。","inputSchema":{"type":"object","properties":{"id":{"type":"integer","minimum":1}},"required":["id"],"additionalProperties":false}})"},
{"self.focus.start",R"({"name":"self.focus.start","description":"开始一次专注计时/番茄钟，默认25分钟。到时由本地闹钟轻铃提醒，重启可恢复但关机不唤醒。返回reminder_id；可用reminders.delete取消。不会覆盖已有闹钟。","inputSchema":{"type":"object","properties":{"minutes":{"type":"integer","minimum":1,"maximum":180},"title":{"type":"string","maxLength":32}},"additionalProperties":false}})"}
};
const cJSON* Get(const cJSON* a,const char* key) {return cJSON_GetObjectItemCaseSensitive(a,key);}
const char* Text(const cJSON* a,const char* key) {auto* v=Get(a,key);return cJSON_IsString(v) ? v->valuestring : nullptr;}
bool Keys(const cJSON* a,std::initializer_list<const char*> keys) {
    if (!cJSON_IsObject(a)) return false;
    for (auto* p=a->child;p;p=p->next) {
        if (!p->string || std::none_of(keys.begin(),keys.end(),[&](const char* k){return !std::strcmp(k,p->string);})) return false;
        for (auto* q=p->next;q;q=q->next) if (q->string && !std::strcmp(p->string,q->string)) return false;
    }
    return true;
}
bool Number(const cJSON* v,uint32_t min,uint32_t max,uint32_t& result) {
    if (!cJSON_IsNumber(v) || !std::isfinite(v->valuedouble) || std::trunc(v->valuedouble)!=v->valuedouble ||
        v->valuedouble<min || v->valuedouble>max) return false;
    result=v->valuedouble;return true;
}
std::string Print(const cJSON* json) {
    char* text=cJSON_PrintUnformatted(json);if (!text) return {};
    std::string out(text);cJSON_free(text);return out;
}
cJSON* NoteJson(const notes::Note& note,bool full) {
    auto* out=cJSON_CreateObject();if (!out) return nullptr;
    cJSON_AddNumberToObject(out,"id",note.id);cJSON_AddStringToObject(out,"title",note.title.c_str());
    cJSON_AddNumberToObject(out,"updated_epoch",note.updated);
    if (full) cJSON_AddStringToObject(out,"text",note.text.c_str());
    return out;
}
}
size_t SystemTools::Count() {return sizeof(specs)/sizeof(specs[0]);}
const char* SystemTools::Schema(size_t index) {return index<Count() ? specs[index].schema : nullptr;}
bool SystemTools::Knows(const char* name) {
    return name && std::any_of(std::begin(specs),std::end(specs),[name](const auto& spec){return !std::strcmp(name,spec.name);});
}
ToolReply SystemTools::Handle(const char* name,const cJSON* args,int64_t now) {
    const auto bad=[](){return ToolReply{false,"参数无效，请遵守工具schema"};};
    SystemCommand command;
    const auto is=[name](const char* other){return !std::strcmp(name,other);};
    if (is("self.notes.list") || is("self.notes.read") || is("self.notes.save") || is("self.notes.delete")) {
        if (!notes_.Ready()) return {false,"笔记存储不可用，请检查SD卡后重启设备"};
        if (is("self.notes.list")) {
            if (!Keys(args,{})) return bad();
            Json list(cJSON_CreateArray(),cJSON_Delete);
            for (const auto& n:notes_.List()) cJSON_AddItemToArray(list.get(),NoteJson(n,false));
            return {true,Print(list.get())};
        }
        uint32_t id=0;
        if (Get(args,"id") && !Number(Get(args,"id"),1,UINT32_MAX-1,id)) return bad();
        if (is("self.notes.save")) {
            const char* title=Text(args,"title");const char* text=Text(args,"text");
            if (!Keys(args,{"id","title","text"}) || !title || !text) return bad();
            notes::Note note{id,title,text,reminders::ValidClock(now) ? now : 0},saved;std::string error;
            if (!notes_.Put(note,saved,error)) return {false,error};
            Json result(NoteJson(saved,false),cJSON_Delete);return {true,Print(result.get())};
        }
        if (!Keys(args,{"id"}) || !id) return bad();
        if (is("self.notes.delete")) {
            std::string error;bool ok=notes_.Remove(id,error);return {ok,ok ? "已删除笔记" : error};
        }
        for (const auto& n:notes_.List()) if (n.id==id) {
            Json result(NoteJson(n,true),cJSON_Delete);return {true,Print(result.get())};
        }
        return {false,"未找到笔记，请先查询ID"};
    }
    if (is("self.focus.start")) {
        uint32_t minutes=25;
        if (!Keys(args,{"minutes","title"}) || (Get(args,"minutes") && !Number(Get(args,"minutes"),1,180,minutes)) ||
            (Get(args,"title") && !Text(args,"title"))) return bad();
        reminders::Item item,saved;item.title=Text(args,"title") ? Text(args,"title") : "专注结束，休息一下";
        item.at=now+minutes*60;std::string error;
        if (!reminders_.Put(item,now,saved,error)) return {false,error};
        Json result(cJSON_CreateObject(),cJSON_Delete);
        cJSON_AddNumberToObject(result.get(),"reminder_id",saved.id);
        cJSON_AddStringToObject(result.get(),"ends_at",reminders::LocalTime(saved.at).c_str());
        cJSON_AddNumberToObject(result.get(),"minutes",minutes);return {true,Print(result.get())};
    }
    if (is("self.device.set_volume")) {
        if (!Keys(args,{"volume"}) || !Number(Get(args,"volume"),0,100,command.number)) return bad();
        command.kind=SystemCommand::Kind::Volume;
    } else if (is("self.display.open")) {
        const char* app=Text(args,"app");
        const char* apps[]={"home","alarm","calendar","recorder","assistant","voice_note","notes","capsules","reader","apps","device","status"};
        if (!Keys(args,{"app"}) || !app || std::none_of(std::begin(apps),std::end(apps),[app](const char* a){return !std::strcmp(a,app);})) return bad();
        command.kind=SystemCommand::Kind::Open;command.value=app;
    } else if (is("self.display.calendar")) {
        const char* date=Text(args,"date");int64_t epoch;
        if (!Keys(args,{"date"}) || !date || std::strlen(date)!=10 ||
            !reminders::ParseLocalTime(std::string(date)+" 12:00:00",epoch)) return bad();
        command.kind=SystemCommand::Kind::Calendar;command.value=date;
    } else if (is("self.recorder.control")) {
        const char* action=Text(args,"action");
        if (!Keys(args,{"action"}) || !action) return bad();
        if (!std::strcmp(action,"start")) command.kind=SystemCommand::Kind::Record;
        else if (!std::strcmp(action,"stop")) command.kind=SystemCommand::Kind::StopRecording;
        else if (!std::strcmp(action,"play")) command.kind=SystemCommand::Kind::PlayRecording;
        else return bad();
    } else if (is("self.system.action_status") || is("self.system.cancel_action")) {
        if (!Keys(args,{"id"}) || !Number(Get(args,"id"),1,UINT32_MAX-1,command.number)) return bad();
        command.kind=is("self.system.action_status") ? SystemCommand::Kind::ActionStatus : SystemCommand::Kind::CancelAction;
    } else {
        if (!Keys(args,{})) return bad();
        if (is("self.device.get_status")) command.kind=SystemCommand::Kind::Status;
        else if (is("self.dashboard.get")) command.kind=SystemCommand::Kind::Dashboard;
        else if (is("self.dashboard.refresh")) command.kind=SystemCommand::Kind::Refresh;
        else if (is("self.recorder.get_status")) command.kind=SystemCommand::Kind::RecorderStatus;
        else return bad();
    }
    return execute_ ? execute_(command) : ToolReply{false,"系统控制不可用"};
}
}
