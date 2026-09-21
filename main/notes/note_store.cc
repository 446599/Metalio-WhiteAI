#include "note_store.h"
#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <set>

namespace notes {
namespace {
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
bool Valid(const Note& n) {
    return !n.title.empty() && n.title.size()<=Store::kTitleBytes && !n.text.empty() &&
        n.text.size()<=Store::kTextBytes && n.title.find('\0')==std::string::npos &&
        n.text.find('\0')==std::string::npos && n.updated>=0 && n.updated<4102444800LL;
}
bool Number(const cJSON* root,const char* key,int64_t min,int64_t max,int64_t& value) {
    auto* n=cJSON_GetObjectItemCaseSensitive(root,key);
    if (!cJSON_IsNumber(n) || !std::isfinite(n->valuedouble) || std::trunc(n->valuedouble)!=n->valuedouble ||
        n->valuedouble<min || n->valuedouble>max) return false;
    value=n->valuedouble; return true;
}
}
bool Store::Restore(const std::string& json) {
    std::lock_guard<std::mutex> lock(mutex_);
    ready_=false;
    if (json.empty()) { notes_.clear(); next_=1; ready_=true; ++revision_; return true; }
    if (json.size()>20000) return false;
    Json root(cJSON_ParseWithLengthOpts(json.c_str(),json.size()+1,nullptr,true),cJSON_Delete);
    int64_t schema,next;
    const auto* items=root ? cJSON_GetObjectItemCaseSensitive(root.get(),"items") : nullptr;
    if (!Number(root.get(),"schema",1,1,schema) || !Number(root.get(),"next",1,UINT32_MAX,next) ||
        !cJSON_IsArray(items) || cJSON_GetArraySize(items)>static_cast<int>(kCapacity)) return false;
    std::vector<Note> loaded; std::set<uint32_t> ids;
    const cJSON* item;
    cJSON_ArrayForEach(item,items) {
        Note n; int64_t id;
        const auto* title=cJSON_GetObjectItemCaseSensitive(item,"title");
        const auto* text=cJSON_GetObjectItemCaseSensitive(item,"text");
        if (!Number(item,"id",1,next-1,id) || !Number(item,"updated",0,4102444799LL,n.updated) ||
            !cJSON_IsString(title) || !cJSON_IsString(text)) return false;
        n.id=id; n.title=title->valuestring; n.text=text->valuestring;
        if (!Valid(n) || !ids.insert(n.id).second) return false;
        loaded.push_back(std::move(n));
    }
    notes_=std::move(loaded); next_=next; ready_=true; ++revision_; return true;
}
bool Store::Commit(const std::vector<Note>& notes,uint32_t next) {
    Json root(cJSON_CreateObject(),cJSON_Delete);
    if (!root || !cJSON_AddNumberToObject(root.get(),"schema",1) || !cJSON_AddNumberToObject(root.get(),"next",next)) return false;
    auto* items=cJSON_AddArrayToObject(root.get(),"items"); if (!items) return false;
    for (const auto& n:notes) {
        auto* item=cJSON_CreateObject();
        if (!item) return false;
        cJSON_AddItemToArray(items,item);
        if (!cJSON_AddNumberToObject(item,"id",n.id) || !cJSON_AddNumberToObject(item,"updated",n.updated) ||
            !cJSON_AddStringToObject(item,"title",n.title.c_str()) || !cJSON_AddStringToObject(item,"text",n.text.c_str())) return false;
    }
    char* raw=cJSON_PrintUnformatted(root.get()); if (!raw) return false;
    const std::string json(raw); cJSON_free(raw);
    if (json.size()>20000 || !save_(json)) return false;
    notes_=notes; next_=next; ++revision_; return true;
}
bool Store::Put(Note note,Note& saved,std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_) { error="笔记存储不可用"; return false; }
    if (!Valid(note)) { error="标题和正文不能为空或超长"; return false; }
    auto next=notes_; auto next_id=next_;
    if (note.id) {
        auto found=std::find_if(next.begin(),next.end(),[&](const auto& n){return n.id==note.id;});
        if (found==next.end()) { error="未找到笔记，请先查询ID"; return false; }
        *found=note;
    } else {
        if (next.size()>=kCapacity || next_id==UINT32_MAX) { error="笔记已满，请先整理旧笔记"; return false; }
        note.id=next_id++; next.push_back(note);
    }
    if (!Commit(next,next_id)) { error="保存失败，原笔记未改变"; return false; }
    saved=note; return true;
}
bool Store::Remove(uint32_t id,std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_) { error="笔记存储不可用"; return false; }
    auto next=notes_;
    auto found=std::find_if(next.begin(),next.end(),[id](const auto& n){return n.id==id;});
    if (found==next.end()) { error="未找到笔记"; return false; }
    next.erase(found);
    if (!Commit(next,next_)) { error="删除失败，原笔记未改变"; return false; }
    return true;
}
std::vector<Note> Store::List() const {
    std::lock_guard<std::mutex> lock(mutex_); auto result=notes_;
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.updated!=b.updated ? a.updated>b.updated : a.id>b.id;});
    return result;
}
uint32_t Store::Revision() const { std::lock_guard<std::mutex> lock(mutex_); return revision_; }
bool Store::Ready() const { std::lock_guard<std::mutex> lock(mutex_); return ready_; }
}  // namespace notes
