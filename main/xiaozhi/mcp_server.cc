#include "mcp_server.h"
#include "metadata_log.h"
#include "system_tools.h"
#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <climits>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <memory>

namespace xiaozhi {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
const cJSON* Get(const cJSON* o, const char* key) { return cJSON_GetObjectItemCaseSensitive(o, key); }
const char* Text(const cJSON* o, const char* key) {
    const auto* v = Get(o, key); return cJSON_IsString(v) ? v->valuestring : nullptr;
}
std::string Print(const cJSON* o) {
    char* text = cJSON_PrintUnformatted(o); if (!text) return {};
    std::string result(text); cJSON_free(text); return result;
}
bool Integer(const cJSON* n, int64_t min, int64_t max, int64_t& value) {
    if (!cJSON_IsNumber(n) || !std::isfinite(n->valuedouble) || n->valuedouble < min ||
        n->valuedouble > max || std::trunc(n->valuedouble) != n->valuedouble) return false;
    value = static_cast<int64_t>(n->valuedouble); return true;
}
bool OnlyKeys(const cJSON* o, std::initializer_list<const char*> keys) {
    if (!cJSON_IsObject(o)) return false;
    for (auto* p = o->child; p; p = p->next) {
        if (!p->string || std::none_of(keys.begin(), keys.end(), [&](const char* k) { return !std::strcmp(k, p->string); })) return false;
        for (auto* q = p->next; q; q = q->next) if (q->string && !std::strcmp(p->string, q->string)) return false;
    }
    return true;
}
cJSON* ItemJson(const reminders::Item& i) {
    auto* o = cJSON_CreateObject(); if (!o) return nullptr;
    cJSON_AddNumberToObject(o, "id", i.id); cJSON_AddStringToObject(o, "kind", i.kind.c_str());
    cJSON_AddStringToObject(o, "title", i.title.c_str());
    cJSON_AddStringToObject(o, "when", reminders::LocalTime(i.at).c_str());
    cJSON_AddStringToObject(o, "timezone", "Asia/Shanghai");
    cJSON_AddBoolToObject(o, "enabled", i.enabled);
    if (i.snoozed_until) cJSON_AddStringToObject(o, "snoozed_until", reminders::LocalTime(i.snoozed_until).c_str());
    cJSON_AddNumberToObject(o, "duration_minutes", i.duration_minutes);
    auto* days = cJSON_AddArrayToObject(o, "weekdays");
    for (int d = 0; d < 7; ++d) if (i.weekdays & (1U << d)) cJSON_AddItemToArray(days, cJSON_CreateNumber(d + 1));
    cJSON_AddStringToObject(o, "state", i.snoozed_until ? "snoozed" : i.enabled ? "scheduled" : i.last_fired ? "fired_or_disabled" : "disabled_or_expired");
    return o;
}
void Error(cJSON* response, int code, const char* message) {
    auto* error = cJSON_AddObjectToObject(response, "error");
    cJSON_AddNumberToObject(error, "code", code); cJSON_AddStringToObject(error, "message", message);
}
void ToolResult(cJSON* response, const std::string& text, bool failed) {
    auto* result = cJSON_AddObjectToObject(response, "result");
    auto* content = cJSON_AddArrayToObject(result, "content");
    auto* entry = cJSON_CreateObject(); cJSON_AddItemToArray(content, entry);
    cJSON_AddStringToObject(entry, "type", "text"); cJSON_AddStringToObject(entry, "text", text.c_str());
    cJSON_AddBoolToObject(result, "isError", failed);
}
// Use the same JSON schemas for discovery, with bounded requests validated
// below as well: a model/schema promise is not a firmware safety boundary.
constexpr const char* kSchemas[] = {
R"({"name":"self.clock.get_time","description":"获取设备当前北京时间、日期和星期。设置明天、下周等相对日期前必须先调用本工具，不要猜测今天日期。","inputSchema":{"type":"object","properties":{},"additionalProperties":false}})",
R"({"name":"self.reminders.list","description":"查询本机所有闹钟和日程，包含ID、时间、重复星期和启用状态。修改或删除前先查询确认ID。","inputSchema":{"type":"object","properties":{},"additionalProperties":false}})",
R"({"name":"self.reminders.create","description":"在设备本地真正保存闹钟、倒计时或日程，到点独立闹钟界面、铃声和振动提醒。仅开机时提醒，关机不唤醒。when使用北京时间YYYY-MM-DD HH:MM或含秒格式；也可用after_seconds设置几秒或几分钟后，二者必须且只能提供一个。repeat可为once/daily/weekdays/weekends/custom；custom需weekdays数组1=周一至7=周日。先get_time解析明天等日期；仅工具成功后告诉用户已设置。","inputSchema":{"type":"object","properties":{"kind":{"type":"string","enum":["alarm","event"]},"title":{"type":"string","maxLength":32},"when":{"type":"string"},"after_seconds":{"type":"integer","minimum":1,"maximum":604800},"repeat":{"type":"string","enum":["once","daily","weekdays","weekends","custom"]},"weekdays":{"type":"array","items":{"type":"integer","minimum":1,"maximum":7},"maxItems":7},"duration_minutes":{"type":"integer","minimum":0,"maximum":1440}},"required":["kind","title"],"additionalProperties":false}})",
R"({"name":"self.reminders.update","description":"修改本机已有闹钟或日程。先list确定ID；只传要修改的字段。when为北京时间YYYY-MM-DD HH:MM[:SS]，或after_seconds表示从现在起。repeat与create相同；启用过期提醒需指定新的未来时间。","inputSchema":{"type":"object","properties":{"id":{"type":"integer","minimum":1},"title":{"type":"string","maxLength":32},"when":{"type":"string"},"after_seconds":{"type":"integer","minimum":1,"maximum":604800},"repeat":{"type":"string","enum":["once","daily","weekdays","weekends","custom"]},"weekdays":{"type":"array","items":{"type":"integer","minimum":1,"maximum":7},"maxItems":7},"duration_minutes":{"type":"integer","minimum":0,"maximum":1440},"enabled":{"type":"boolean"}},"required":["id"],"additionalProperties":false}})",
R"({"name":"self.reminders.delete","description":"删除指定ID的本机闹钟或日程。先list确认用户要删除的条目，不能猜ID。","inputSchema":{"type":"object","properties":{"id":{"type":"integer","minimum":1}},"required":["id"],"additionalProperties":false}})",
R"({"name":"self.reminders.stop","description":"停止当前正在响铃或振动的提醒，不删除重复闹钟。用户说停止闹钟、别响了时使用。","inputSchema":{"type":"object","properties":{},"additionalProperties":false}})"
};
constexpr size_t kTools = sizeof(kSchemas) / sizeof(kSchemas[0]);

bool Apply(const cJSON* args, reminders::Item& i, int64_t now, bool creating, std::string& error) {
    auto fail = [&]() { error = "参数无效：请按工具schema传入有效标题、未来北京时间和重复规则"; return false; };
    if (!OnlyKeys(args, {"id","kind","title","when","after_seconds","repeat","weekdays","duration_minutes","enabled"})) return fail();
    if (Get(args,"kind")) { const char* kind = Text(args,"kind"); if (!creating || !kind) return fail(); i.kind = kind; }
    if (Get(args,"title")) { const char* title = Text(args,"title"); if (!title) return fail(); i.title = title; }
    if (creating && (!Text(args,"kind") || !Text(args,"title") || Get(args,"id") || Get(args,"enabled"))) return fail();
    const auto* when = Get(args,"when"); const auto* after = Get(args,"after_seconds");
    if ((when && after) || (creating && !when && !after)) return fail();
    if (when && (!cJSON_IsString(when) || !reminders::ParseLocalTime(when->valuestring, i.at))) return fail();
    if (after) { int64_t seconds; if (!Integer(after, 1, 604800, seconds)) return fail(); i.at = now + seconds; }
    if (Get(args,"repeat")) {
        const char* repeat = Text(args,"repeat"); if (!repeat) return fail();
        if (!std::strcmp(repeat,"once")) i.weekdays = 0;
        else if (!std::strcmp(repeat,"daily")) i.weekdays = 127;
        else if (!std::strcmp(repeat,"weekdays")) i.weekdays = 31;
        else if (!std::strcmp(repeat,"weekends")) i.weekdays = 96;
        else if (!std::strcmp(repeat,"custom")) {
            const auto* days = Get(args,"weekdays");
            if (!cJSON_IsArray(days) || cJSON_GetArraySize(days) < 1 || cJSON_GetArraySize(days) > 7) return fail();
            i.weekdays = 0; const cJSON* d;
            cJSON_ArrayForEach(d, days) { int64_t day; if (!Integer(d,1,7,day) || (i.weekdays & (1U << (day-1)))) return fail(); i.weekdays |= 1U << (day-1); }
        } else return fail();
        if (Get(args,"weekdays") && std::strcmp(repeat,"custom")) return fail();
    } else if (Get(args,"weekdays")) return fail();
    if (Get(args,"duration_minutes")) { int64_t duration; if (!Integer(Get(args,"duration_minutes"),0,1440,duration)) return fail(); i.duration_minutes = duration; }
    if (Get(args,"enabled")) { if (!cJSON_IsBool(Get(args,"enabled"))) return fail(); i.enabled = cJSON_IsTrue(Get(args,"enabled")); }
    return true;
}
}  // namespace

std::string McpServer::Handle(const std::string& payload, int64_t now) {
    if (payload.empty() || payload.size() > 4096 || notes::ContainsEncodedNull(payload)) return {};
    Json request(cJSON_ParseWithLengthOpts(payload.c_str(), payload.size()+1, nullptr, true), cJSON_Delete);
    Json response(cJSON_CreateObject(), cJSON_Delete);
    if (!response) return {};
    cJSON_AddStringToObject(response.get(), "jsonrpc", "2.0");
    const auto* id = request ? Get(request.get(),"id") : nullptr;
    int64_t number;
    const bool valid_id = (cJSON_IsString(id) && std::strlen(id->valuestring) <= 64) || Integer(id, INT32_MIN, INT32_MAX, number);
    cJSON_AddItemToObject(response.get(), "id", valid_id ? cJSON_Duplicate(id,true) : cJSON_CreateNull());
    const char* method = request ? Text(request.get(),"method") : nullptr;
    if (!request || !OnlyKeys(request.get(),{"jsonrpc","id","method","params"}) ||
        !Text(request.get(),"jsonrpc") || std::strcmp(Text(request.get(),"jsonrpc"),"2.0") || !method) {
        Error(response.get(), request ? -32600 : -32700, "Invalid JSON-RPC request"); return Print(response.get());
    }
    if (!id) return {}; // Notifications never perform mutating tools or receive responses.
    if (!valid_id) { Error(response.get(), -32600, "Invalid request id"); return Print(response.get()); }
    const auto* trace_params=Get(request.get(),"params");
    const char* trace_name=Text(trace_params,"name");
    // Tool names/argument values originate outside the trust boundary. Only
    // log names recognised by our schemas; never log arbitrary user strings.
    bool known=trace_name && system_ && SystemTools::Knows(trace_name);
    if(trace_name) for(size_t n=0;n<kTools && !known;++n) {
        Json schema(cJSON_Parse(kSchemas[n]),cJSON_Delete);
        const char* schema_name=Text(schema.get(),"name");
        known=schema_name && std::strcmp(schema_name,trace_name)==0;
    }
    const bool is_call=std::strcmp(method,"tools/call")==0;
    struct Trace {
        const cJSON* response; const char* name; bool call; bool cached=false;
        ~Trace(){ if(!call)return; if(cached){XZ_META("mcp retry tool=%s cached=1",name);return;}
            const auto* error=Get(response,"error"); const auto* result=Get(response,"result");
            const auto* code=Get(error,"code");
            XZ_META("mcp call tool=%s ok=%d code=%d",name,
                !error && !cJSON_IsTrue(Get(result,"isError")),cJSON_IsNumber(code)?code->valueint:0);
        }
    } trace{response.get(),known ? trace_name : "unknown",is_call};
    const auto id_key = Print(id);
    for (const auto& entry : cache_) if (entry.id == id_key) {
        if (entry.request == payload) {trace.cached=true;return entry.response;}
        Error(response.get(), -32600, "Request id already used with different parameters"); return Print(response.get());
    }
    const auto* params = Get(request.get(),"params");
    if (!std::strcmp(method,"initialize")) {
        auto* result = cJSON_AddObjectToObject(response.get(),"result");
        cJSON_AddStringToObject(result,"protocolVersion","2024-11-05");
        auto* caps = cJSON_AddObjectToObject(result,"capabilities"); cJSON_AddObjectToObject(caps,"tools");
        auto* info = cJSON_AddObjectToObject(result,"serverInfo");
        cJSON_AddStringToObject(info,"name","miaoink4-system"); cJSON_AddStringToObject(info,"version","2.1.0");
    } else if (!std::strcmp(method,"ping")) {
        cJSON_AddObjectToObject(response.get(),"result");
    } else if (!std::strcmp(method,"tools/list")) {
        const char* cursor = Text(params,"cursor");
        const bool valid = !params || (OnlyKeys(params,{"cursor","withUserTools"}) &&
            (!Get(params,"cursor") || cursor) && (!Get(params,"withUserTools") || cJSON_IsBool(Get(params,"withUserTools"))));
        const size_t count=kTools+(system_ ? SystemTools::Count() : 0);
        size_t start=0;bool cursor_ok=true;
        if (cursor && *cursor) {
            cursor_ok=std::strlen(cursor)<=3 && cursor[0]!='0';
            for (const char* p=cursor;cursor_ok && *p;++p) {
                if (*p<'0' || *p>'9') cursor_ok=false;
                else start=start*10+static_cast<size_t>(*p-'0');
            }
            cursor_ok=cursor_ok && start<count && start%3==0;
        }
        if (!valid || !cursor_ok) Error(response.get(),-32602,"Invalid cursor");
        else {
            auto* result = cJSON_AddObjectToObject(response.get(),"result"); auto* list = cJSON_AddArrayToObject(result,"tools");
            for (size_t n=start;n<std::min(start+3,count);++n)
                cJSON_AddItemToArray(list,cJSON_Parse(n<kTools ? kSchemas[n] : SystemTools::Schema(n-kTools)));
            if (start+3<count) cJSON_AddStringToObject(result,"nextCursor",std::to_string(start+3).c_str());
        }
    } else if (!std::strcmp(method,"tools/call")) {
        const char* name = Text(params,"name"); const auto* args = Get(params,"arguments");
        Json empty(cJSON_CreateObject(),cJSON_Delete); if (!args) args = empty.get();
        if (!OnlyKeys(params,{"name","arguments"}) || !name || !cJSON_IsObject(args)) Error(response.get(),-32602,"Invalid tool arguments");
        else if (!std::strcmp(name,"self.clock.get_time") && OnlyKeys(args,{})) {
            Json o(cJSON_CreateObject(),cJSON_Delete);
            cJSON_AddBoolToObject(o.get(),"valid",reminders::ValidClock(now));
            cJSON_AddStringToObject(o.get(),"now",reminders::LocalTime(now).c_str());
            cJSON_AddStringToObject(o.get(),"timezone","Asia/Shanghai");
            cJSON_AddNumberToObject(o.get(),"utc_offset_minutes",480);
            const time_t t = now; struct tm local{}; localtime_r(&t,&local);
            cJSON_AddNumberToObject(o.get(),"weekday",(local.tm_wday+6)%7+1);
            cJSON_AddBoolToObject(o.get(),"storage_ready",store_.Ready());
            cJSON_AddBoolToObject(o.get(),"power_off_wakeup",false);
            ToolResult(response.get(),Print(o.get()),false);
        } else if (!std::strcmp(name,"self.reminders.list") && OnlyKeys(args,{})) {
            Json list(cJSON_CreateArray(),cJSON_Delete);
            for (const auto& i : store_.List()) cJSON_AddItemToArray(list.get(),ItemJson(i));
            ToolResult(response.get(),store_.Ready() ? Print(list.get()) : "提醒存储不可用",!store_.Ready());
        } else if (!std::strcmp(name,"self.reminders.stop") && OnlyKeys(args,{})) {
            ToolResult(response.get(),stop_() ? "已停止当前提醒" : "当前没有正在响铃的提醒",false);
        } else if (!std::strcmp(name,"self.reminders.create") || !std::strcmp(name,"self.reminders.update")) {
            const bool create = !std::strcmp(name,"self.reminders.create");
            reminders::Item item, saved; std::string error; bool found = create;
            int64_t wanted;
            if (!create && Integer(Get(args,"id"),1,UINT32_MAX-1,wanted)) {
                for (const auto& i : store_.List()) if (i.id == wanted) { item=i; found=true; break; }
            }
            bool ok = found && Apply(args,item,now,create,error) && store_.Put(item,now,saved,error);
            if (!found) error = "未找到该提醒，请先查询列表确认ID";
            Json result(ok ? ItemJson(saved) : nullptr,cJSON_Delete);
            ToolResult(response.get(),ok ? Print(result.get()) : error,!ok);
        } else if (!std::strcmp(name,"self.reminders.delete")) {
            int64_t wanted; std::string error;
            bool ok = OnlyKeys(args,{"id"}) && Integer(Get(args,"id"),1,UINT32_MAX-1,wanted);
            if (ok) ok = store_.Remove(wanted,error); else error = "请提供有效提醒ID";
            ToolResult(response.get(),ok ? "已删除提醒" : error,!ok);
        } else if (system_ && SystemTools::Knows(name)) {
            const auto reply=system_->Handle(name,args,now);
            ToolResult(response.get(),reply.text,!reply.ok);
        } else Error(response.get(),-32601,"Unknown tool or invalid arguments");
    } else Error(response.get(),-32601,"Method not found");
    auto result = Print(response.get());
    if (!result.empty()) {
        // Keep retries bounded by bytes as well as count on the ESP32 heap.
        constexpr size_t kCacheBytes = 12288;
        const size_t added = id_key.size() + payload.size() + result.size();
        size_t bytes = added;
        for (const auto& entry : cache_) bytes += entry.id.size() + entry.request.size() + entry.response.size();
        while (!cache_.empty() && (cache_.size() >= 8 || bytes > kCacheBytes)) {
            const auto& old = cache_.front();
            bytes -= old.id.size() + old.request.size() + old.response.size();
            cache_.pop_front();
        }
        if (added <= kCacheBytes) cache_.push_back({id_key,payload,result});
    }
    return result;
}
}  // namespace xiaozhi
