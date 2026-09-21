#include "text_input.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>

namespace input {
namespace {
struct Syllable { const char* spelling; const char* characters; };
#include "pinyin_dictionary.inc"
struct Phrase { const char* spelling; const char* text; };
constexpr Phrase phrases[] = {
    {"nihao","你好"},{"xiexie","谢谢"},{"zaijian","再见"},{"zhongguo","中国"},
    {"zhongwen","中文"},{"pinyin","拼音"},{"shurufa","输入法"},{"wangluo","网络"},
    {"mima","密码"},{"lianjie","连接"},{"shebei","设备"},{"shezhi","设置"},
    {"biji","笔记"},{"xiangmu","项目"},{"gongzuo","工作"},{"xuexi","学习"},
    {"jintian","今天"},{"mingtian","明天"},{"shijian","时间"},{"tixing","提醒"},
    {"baocun","保存"},{"quxiao","取消"},{"wancheng","完成"},{"jilu","记录"},
    {"linggan","灵感"},{"jihua","计划"},{"ceshi","测试"},{"sheji","设计"},
    {"dayin","打印"},{"citie","磁铁"},{"moban","模板"},{"dianzi","电子"},
    {"mohu","模糊"},{"dianliang","电量"},{"moshuiping","墨水屏"},{"shijie","世界"}
};
const char* Characters(const std::string& key) {
    const auto it=std::lower_bound(std::begin(kPinyin),std::end(kPinyin),key,
        [](const Syllable& s,const std::string& k){return std::strcmp(s.spelling,k.c_str())<0;});
    return it!=std::end(kPinyin) && key==it->spelling ? it->characters : nullptr;
}
}
void Wipe(std::string& text) {
    text.resize(text.capacity(), '\0');
    if (!text.empty()) {
        volatile char* p=&text[0];
        for (size_t i=0;i<text.size();++i) p[i]=0;
    }
    text.clear();
}
bool ValidUtf8(const std::string& s) {
    for (size_t i=0;i<s.size();) {
        const auto first=static_cast<uint8_t>(s[i++]);
        if (first==0) return false;
        if (first<0x80) continue;
        unsigned extra; uint32_t cp,minimum;
        if (first>=0xc2 && first<=0xdf) {extra=1;cp=first&31;minimum=0x80;}
        else if (first>=0xe0 && first<=0xef) {extra=2;cp=first&15;minimum=0x800;}
        else if (first>=0xf0 && first<=0xf4) {extra=3;cp=first&7;minimum=0x10000;}
        else return false;
        if (extra>s.size()-i) return false;
        while (extra--) {const auto c=static_cast<uint8_t>(s[i++]);if ((c&0xc0)!=0x80) return false;cp=(cp<<6)|(c&63);}
        if (cp<minimum || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff)) return false;
    }
    return true;
}
size_t PreviousBoundary(const std::string& s,size_t p) {
    p=std::min(p,s.size());if (!p) return 0;
    do {--p;} while (p && (static_cast<uint8_t>(s[p])&0xc0)==0x80);
    return p;
}
size_t NextBoundary(const std::string& s,size_t p) {
    p=std::min(p,s.size());if (p==s.size()) return p;
    do {++p;} while (p<s.size() && (static_cast<uint8_t>(s[p])&0xc0)==0x80);
    return p;
}
bool Editor::Fail(const char* text) {error_=text;return false;}
bool Editor::Begin(const std::string& text,size_t limit,bool secret,bool multiline) {
    const std::string initial=text;
    Clear();
    if (!limit || limit>kMaxBytes || !ValidUtf8(initial) || initial.size()>limit) return Fail("输入内容或长度无效");
    secret_=secret;multiline_=multiline;limit_=limit;
    mode_=secret ? Mode::Lower : Mode::Pinyin;
    if (!Insert(initial)) {Clear();return Fail("输入内容无效");}
    return true;
}
void Editor::Clear() {Wipe(text_);Wipe(composition_);error_.clear();cursor_=candidate_page_=0;secret_=multiline_=false;mode_=Mode::Pinyin;}
bool Editor::Insert(const std::string& value) {
    if (!ValidUtf8(value)) return Fail("无效的文字编码");
    for (unsigned char c:value) {
        if ((c<32 && !(multiline_ && c=='\n')) || c==127 || (secret_ && c>126)) return Fail("此字段不支持该字符");
    }
    if (value.size()>limit_-text_.size()) return Fail("已达到字节上限，内容未截断");
    text_.insert(cursor_,value);cursor_+=value.size();error_.clear();return true;
}
bool Editor::Type(char key) {
    if (mode_==Mode::Pinyin && ((key>='a' && key<='z') || (key>='A' && key<='Z') || key=='\'')) {
        if (composition_.size()==kCompositionBytes) return Fail("请先选字再继续输入");
        if (key=='\'' && (composition_.empty() || composition_.back()=='\'')) return Fail("分隔符应放在拼音之间");
        composition_+=key>='A' && key<='Z' ? char(key-'A'+'a') : key;
        candidate_page_=0;error_.clear();return true;
    }
    if (!Ready()) return Fail("请先选字或删除未完成拼音");
    if (mode_==Mode::Upper && key>='a' && key<='z') key=char(key-'a'+'A');
    return Insert(std::string(1,key));
}
bool Editor::Backspace() {
    error_.clear();candidate_page_=0;
    if (!composition_.empty()) {composition_.pop_back();return true;}
    if (!cursor_) return false;
    const auto previous=PreviousBoundary(text_,cursor_);
    if (secret_) {for(size_t i=previous;i<cursor_;++i) text_[i]=0;}
    text_.erase(previous,cursor_-previous);cursor_=previous;return true;
}
bool Editor::Move(int direction) {
    if (!Ready()) return Fail("请先完成拼音");
    cursor_=direction<0 ? PreviousBoundary(text_,cursor_) : NextBoundary(text_,cursor_);error_.clear();return true;
}
bool Editor::SetMode(Mode mode) {
    if (secret_ && mode==Mode::Pinyin) return Fail("密码只使用英数符号");
    if (!Ready()) return Fail("请先选字或删除未完成拼音");
    mode_=mode;candidate_page_=0;error_.clear();return true;
}
std::vector<Candidate> Editor::Lookup(size_t start,size_t count,size_t* total) const {
    std::vector<Candidate> out;out.reserve(std::min(count,kCandidatesPerPage));size_t found=0;
    auto emit=[&](const std::string& value,size_t consumed) {
        if (found>=start && out.size()<count) out.push_back({value,consumed});
        ++found;
    };
    if (!composition_.empty()) {
        // Exact phrase prefix first; apostrophe is an explicit syllable boundary.
        for (const auto& p:phrases) {
            const size_t n=std::strlen(p.spelling);
            if (composition_.compare(0,n,p.spelling)==0) emit(p.text,n);
        }
        const auto sep=composition_.find('\'');
        const size_t max=sep==std::string::npos ? std::min(size_t(6),composition_.size()) : sep;
        for (size_t n=max;n>0;--n) {
            if (sep!=std::string::npos && n!=sep) break;
            if (const char* characters=Characters(composition_.substr(0,n))) {
                const std::string chars(characters);
                for(size_t i=0;i<chars.size();) {const auto next=NextBoundary(chars,i);emit(chars.substr(i,next-i),n);i=next;}
                break;
            }
        }
    }
    if (total) *total=found;
    return out;
}
std::vector<Candidate> Editor::Candidates() const {return Lookup(candidate_page_*kCandidatesPerPage,kCandidatesPerPage);}
size_t Editor::CandidateCount() const {size_t total=0;Lookup(0,0,&total);return total;}
bool Editor::Choose(size_t index) {
    const auto items=Candidates();if (index>=items.size()) return Fail("没有这个候选词");
    if (!Insert(items[index].text)) return false;
    composition_.erase(0,items[index].consumed);
    if (!composition_.empty() && composition_[0]=='\'') composition_.erase(0,1);
    candidate_page_=0;return true;
}
bool Editor::Space() {return composition_.empty() ? Insert(" ") : Choose(0);}
bool Editor::NextCandidates() {
    const auto count=CandidateCount();if (!count) return Fail("没有候选，请检查拼音");
    candidate_page_=(candidate_page_+1)%((count+kCandidatesPerPage-1)/kCandidatesPerPage);error_.clear();return true;
}
std::string Editor::VisibleText(bool reveal) const {
    if (!secret_ || reveal) return text_;
    return std::string(text_.size(),'*'); // Secret fields only admit printable ASCII.
}
}  // namespace input
