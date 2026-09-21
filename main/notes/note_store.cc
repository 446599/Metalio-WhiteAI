#include "note_store.h"
#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <set>

namespace notes {
namespace {
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
bool Time(int64_t n) { return n>=0 && n<4102444800LL; }
bool Label(const std::string& s, size_t limit, bool empty=true) {
    return ValidText(s,limit,empty) && std::none_of(s.begin(),s.end(),[](unsigned char c){return c<32;});
}
bool Valid(const Note& n) {
    const bool source=n.source_id.empty()
        ? n.raw_text.empty() && n.source_revision==0
        : n.source_id.size()==16 && n.source_revision>0 && !n.raw_text.empty() &&
          std::all_of(n.source_id.begin(),n.source_id.end(),[](char c){return (c>='0' && c<='9') || (c>='a' && c<='f');});
    return Label(n.title,Store::kTitleBytes,false) && ValidText(n.text,Store::kTextBytes,false) &&
        ValidText(n.raw_text,Store::kRawBytes) && Label(n.project,Store::kProjectBytes) &&
        Time(n.created) && Time(n.updated) && n.revision>0 && n.reminder_id<UINT32_MAX && source;
}
bool Number(const cJSON* root,const char* key,int64_t min,int64_t max,int64_t& value) {
    const auto* n=cJSON_GetObjectItemCaseSensitive(root,key);
    if (!cJSON_IsNumber(n) || !std::isfinite(n->valuedouble) || std::trunc(n->valuedouble)!=n->valuedouble ||
        n->valuedouble<min || n->valuedouble>max) return false;
    value=static_cast<int64_t>(n->valuedouble); return true;
}
bool String(const cJSON* root,const char* key,std::string& out) {
    const auto* n=cJSON_GetObjectItemCaseSensitive(root,key);
    if (!cJSON_IsString(n)) return false;
    out=n->valuestring; return true;
}
bool UniqueKeys(const cJSON* root) {
    if (!cJSON_IsObject(root)) return false;
    std::set<std::string> keys;
    for (const auto* p=root->child;p;p=p->next)
        if (!p->string || !keys.insert(p->string).second) return false;
    return true;
}
// cJSON exposes strings as C strings; reject encoded NUL rather than silently
// accepting a prefix. Escaped backslashes ("\\u0000") remain ordinary text.
bool EncodedNull(const std::string& json) {
    for (size_t i=0;i<json.size();++i) {
        if (json[i]=='\0') return true;
        if (json[i]=='\\' && i+1<json.size()) {
            if (json.compare(i+1,5,"u0000")==0) return true;
            ++i;
        }
    }
    return false;
}
std::string Fold(std::string s) {
    for (auto& c:s) if (c>='A' && c<='Z') c=static_cast<char>(c-'A'+'a');
    return s;
}
}

bool ContainsEncodedNull(const std::string& json) {return EncodedNull(json);}

bool ValidText(const std::string& s,size_t limit,bool allow_empty) {
    if (s.size()>limit || (!allow_empty && s.empty())) return false;
    for (size_t i=0;i<s.size();) {
        const auto first=static_cast<uint8_t>(s[i++]);
        if (first==0) return false;
        if (first<0x80) continue;
        unsigned need=0; uint32_t cp=0,min=0;
        if (first>=0xc2 && first<=0xdf) {need=1;cp=first&31;min=0x80;}
        else if (first>=0xe0 && first<=0xef) {need=2;cp=first&15;min=0x800;}
        else if (first>=0xf0 && first<=0xf4) {need=3;cp=first&7;min=0x10000;}
        else return false;
        if (need>s.size()-i) return false;
        while (need--) {
            const auto c=static_cast<uint8_t>(s[i++]);
            if ((c&0xc0)!=0x80) return false;
            cp=(cp<<6)|(c&63);
        }
        if (cp<min || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff)) return false;
    }
    return true;
}

bool Store::Restore(const std::string& json) {
    std::lock_guard<std::mutex> lock(mutex_);
    ready_=false;
    if (json.empty()) {notes_.clear();next_=1;ready_=true;++revision_;return true;}
    if (json.size()>kSnapshotBytes || EncodedNull(json)) return false;
    Json root(cJSON_ParseWithLengthOpts(json.c_str(),json.size()+1,nullptr,true),cJSON_Delete);
    int64_t schema,next;
    const auto* items=root ? cJSON_GetObjectItemCaseSensitive(root.get(),"items") : nullptr;
    if (!UniqueKeys(root.get()) || !Number(root.get(),"schema",1,2,schema) ||
        !Number(root.get(),"next",1,UINT32_MAX,next) || !cJSON_IsArray(items) ||
        cJSON_GetArraySize(items)>static_cast<int>(kCapacity)) return false;
    std::vector<Note> loaded; std::set<uint32_t> ids; std::set<std::string> sources;
    const cJSON* item;
    cJSON_ArrayForEach(item,items) {
        Note n;int64_t id;
        if (!UniqueKeys(item) || !Number(item,"id",1,next-1,id) ||
            !Number(item,"updated",0,4102444799LL,n.updated) ||
            !String(item,"title",n.title) || !String(item,"text",n.text)) return false;
        n.id=static_cast<uint32_t>(id);n.created=n.updated;
        if (schema==2) {
            int64_t rev,source_rev,reminder;
            const auto* done=cJSON_GetObjectItemCaseSensitive(item,"done");
            if (!String(item,"raw_text",n.raw_text) || !String(item,"project",n.project) ||
                !String(item,"source_id",n.source_id) || !Number(item,"created",0,4102444799LL,n.created) ||
                !Number(item,"revision",1,UINT32_MAX,rev) || !Number(item,"source_revision",0,UINT32_MAX,source_rev) ||
                !Number(item,"reminder_id",0,UINT32_MAX-1,reminder) || !cJSON_IsBool(done)) return false;
            n.revision=static_cast<uint32_t>(rev);n.source_revision=static_cast<uint32_t>(source_rev);
            n.reminder_id=static_cast<uint32_t>(reminder);n.done=cJSON_IsTrue(done);
        }
        if (!Valid(n) || !ids.insert(n.id).second ||
            (!n.source_id.empty() && !sources.insert(n.source_id).second)) return false;
        loaded.push_back(std::move(n));
    }
    notes_=std::move(loaded);next_=static_cast<uint32_t>(next);ready_=true;++revision_;return true;
}
bool Store::Commit(const std::vector<Note>& notes,uint32_t next) {
    Json root(cJSON_CreateObject(),cJSON_Delete);
    if (!root || !cJSON_AddNumberToObject(root.get(),"schema",2) || !cJSON_AddNumberToObject(root.get(),"next",next)) return false;
    auto* items=cJSON_AddArrayToObject(root.get(),"items");if (!items) return false;
    for (const auto& n:notes) {
        auto* item=cJSON_CreateObject();if (!item) return false;
        if (!cJSON_AddItemToArray(items,item)) {cJSON_Delete(item);return false;}
        if (!cJSON_AddNumberToObject(item,"id",n.id) || !cJSON_AddNumberToObject(item,"updated",n.updated) ||
            !cJSON_AddStringToObject(item,"title",n.title.c_str()) || !cJSON_AddStringToObject(item,"text",n.text.c_str()) ||
            !cJSON_AddStringToObject(item,"raw_text",n.raw_text.c_str()) || !cJSON_AddStringToObject(item,"project",n.project.c_str()) ||
            !cJSON_AddStringToObject(item,"source_id",n.source_id.c_str()) || !cJSON_AddNumberToObject(item,"created",n.created) ||
            !cJSON_AddNumberToObject(item,"revision",n.revision) || !cJSON_AddNumberToObject(item,"source_revision",n.source_revision) ||
            !cJSON_AddNumberToObject(item,"reminder_id",n.reminder_id) || !cJSON_AddBoolToObject(item,"done",n.done)) return false;
    }
    char* raw=cJSON_PrintUnformatted(root.get());if (!raw) return false;
    const std::string json(raw);cJSON_free(raw);
    if (json.size()>kSnapshotBytes || !save_ || !save_(json)) return false;
    notes_=notes;next_=next;++revision_;return true;
}
bool Store::Put(Note note,Note& saved,std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_) {error="笔记存储不可用";return false;}
    if (!Valid(note)) {error="标题、正文或编码无效，内容不能超长";return false;}
    auto next=notes_;auto next_id=next_;
    if (note.id) {
        auto found=std::find_if(next.begin(),next.end(),[&](const auto& n){return n.id==note.id;});
        if (found==next.end()) {error="未找到笔记，请先查询ID";return false;}
        if (!found->source_id.empty()) {error="归档记忆请用memory.update并提供revision，原文不可覆盖";return false;}
        if (found->revision==UINT32_MAX) {error="笔记版本已达上限";return false;}
        Note merged=*found;merged.title=note.title;merged.text=note.text;
        merged.updated=note.updated;++merged.revision;*found=merged;note=std::move(merged);
    } else {
        if (!note.source_id.empty() || !note.raw_text.empty()) {error="原文归档请使用归档接口";return false;}
        if (next.size()>=kCapacity || next_id==UINT32_MAX) {error="笔记已满，请先整理旧笔记";return false;}
        note.id=next_id++;note.created=note.updated;note.revision=1;next.push_back(note);
    }
    if (!Commit(next,next_id)) {error="保存失败，原笔记未改变";return false;}
    saved=note;return true;
}
bool Store::Archive(Note note,Note& saved,bool& existed,std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);existed=false;
    if (!ready_) {error="SD笔记存储不可用，长期归档未完成";return false;}
    if (note.id || note.source_id.empty() || !Valid(note)) {error="归档内容或来源无效，不能截断保存";return false;}
    for (const auto& old:notes_) if (old.source_id==note.source_id) {
        // Fingerprints are deduplication hints, never authorization. Do not
        // overwrite on a collision or retry; return the actual persisted item.
        if (old.raw_text!=note.raw_text) {error="来源指纹冲突，原笔记未改变";return false;}
        existed=true;saved=old;return true;
    }
    if (notes_.size()>=kCapacity || next_==UINT32_MAX) {error="笔记已满，请先整理旧笔记";return false;}
    note.id=next_;note.created=note.updated;note.revision=1;
    auto next=notes_;next.push_back(note);
    if (!Commit(next,next_+1)) {error="归档失败，原笔记未改变";return false;}
    saved=note;return true;
}
bool Store::Update(uint32_t id,uint32_t expected_revision,const Patch& patch,int64_t now,Note& saved,std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_) {error="笔记存储不可用";return false;}
    auto next=notes_;auto found=std::find_if(next.begin(),next.end(),[id](const auto& n){return n.id==id;});
    if (found==next.end()) {error="未找到笔记，请先查询ID";return false;}
    if (!expected_revision || found->revision!=expected_revision || found->revision==UINT32_MAX) {
        error="笔记版本已改变，请重新读取后再修改";return false;
    }
    if (!patch.title && !patch.text && !patch.project && !patch.done && !patch.reminder_id) {
        error="没有需要修改的字段";return false;
    }
    Note n=*found;
    if (patch.title) n.title=*patch.title;
    if (patch.text) n.text=*patch.text;
    if (patch.project) n.project=*patch.project;
    if (patch.done) n.done=*patch.done;
    if (patch.reminder_id) n.reminder_id=*patch.reminder_id;
    n.updated=now;++n.revision;
    if (!Valid(n)) {error="修改内容或编码无效，内容不能超长";return false;}
    *found=n;
    if (!Commit(next,next_)) {error="保存失败，原笔记未改变";return false;}
    saved=std::move(n);return true;
}
bool Store::Remove(uint32_t id,std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_) {error="笔记存储不可用";return false;}
    auto next=notes_;auto found=std::find_if(next.begin(),next.end(),[id](const auto& n){return n.id==id;});
    if (found==next.end()) {error="未找到笔记";return false;}
    next.erase(found);
    if (!Commit(next,next_)) {error="删除失败，原笔记未改变";return false;}
    return true;
}
std::vector<Note> Store::List() const {
    std::lock_guard<std::mutex> lock(mutex_);auto result=notes_;
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.updated!=b.updated ? a.updated>b.updated : a.id>b.id;});
    return result;
}
std::vector<Note> Store::Search(const std::string& query,const std::string& project,std::optional<bool> done) const {
    if (!ValidText(query,192) || !ValidText(project,kProjectBytes)) return {};
    auto result=List();const auto keyword=Fold(query);
    result.erase(std::remove_if(result.begin(),result.end(),[&](const auto& n){
        return (!project.empty() && n.project!=project) || (done && n.done!=*done) ||
            (!keyword.empty() && Fold(n.title).find(keyword)==std::string::npos &&
             Fold(n.text).find(keyword)==std::string::npos && Fold(n.raw_text).find(keyword)==std::string::npos &&
             Fold(n.project).find(keyword)==std::string::npos);
    }),result.end());
    return result;
}
std::string DisplayBody(const Note& n) {
    if (n.raw_text.empty() && n.project.empty() && !n.done && !n.reminder_id) return n.text;
    std::string out="项目："+(n.project.empty() ? std::string("未分类") : n.project);
    out+="\n状态："+std::string(n.done ? "已处理" : "待处理");
    if (n.reminder_id) out+="\n提醒引用：#"+std::to_string(n.reminder_id)+"（独立管理）";
    out+="\n\n整理\n"+n.text;
    if (!n.raw_text.empty()) out+="\n\n原文\n"+n.raw_text;
    return out;
}
uint32_t Store::Revision() const {std::lock_guard<std::mutex> lock(mutex_);return revision_;}
bool Store::Ready() const {std::lock_guard<std::mutex> lock(mutex_);return ready_;}
}  // namespace notes
