#include "input/text_input.h"
#include "input/keyboard_layout.h"
#include "network/setup_model.h"
#include <cassert>
#include <cstdio>
#include <set>
#include <thread>
#include <vector>
using namespace input;
static size_t checks=0;
#define CHECK(x) do { ++checks; if(!(x)) {std::fprintf(stderr,"failed line %d: %s\n",__LINE__,#x);return 1;} } while(0)
void Type(Editor& e,const char* s) {while(*s) assert(e.Type(*s++));}
int main() {
    Editor e;
    CHECK(e.Begin("",96));Type(e,"nihao");CHECK(e.Candidates().at(0).text=="你好");CHECK(e.Space());CHECK(e.Text()=="你好" && e.Ready());
    CHECK(e.Move(-1));CHECK(e.Backspace());CHECK(e.Text()=="好" && e.Cursor()==0);
    CHECK(e.Insert("你"));CHECK(e.Text()=="你好");CHECK(e.Move(1));CHECK(e.Cursor()==6);
    CHECK(e.Begin("",6));Type(e,"zhongguo");CHECK(e.Space());CHECK(e.Text()=="中国");CHECK(!e.Insert("a"));CHECK(e.Text()=="中国");
    CHECK(e.Begin("",96));Type(e,"ni'hao");CHECK(e.Candidates().at(0).text=="你");CHECK(e.Choose(0));CHECK(e.Composition()=="hao");CHECK(e.Space());CHECK(e.Text()=="你好");
    CHECK(e.Begin("",96));Type(e,"lv");CHECK(e.CandidateCount()>0);CHECK(e.NextCandidates());CHECK(e.Choose(0));CHECK(ValidUtf8(e.Text()));
    CHECK(e.Begin("",96));Type(e,"shi");const auto count=e.CandidateCount();CHECK(count>20);
    std::set<std::string> choices;
    for(size_t i=0;i<(count+3)/4;++i) {for(auto& c:e.Candidates()) CHECK(choices.insert(c.text).second);CHECK(e.NextCandidates());}
    CHECK(choices.size()==count);CHECK(!e.SetMode(Mode::Upper));CHECK(e.Backspace());CHECK(e.Composition()=="sh");
    CHECK(e.Begin("",96));for(size_t i=0;i<Editor::kCompositionBytes;++i) CHECK(e.Type('z'));CHECK(!e.Type('z'));CHECK(e.Text().empty());CHECK(!e.Space());
    CHECK(e.Begin("",64,true));CHECK(!e.SetMode(Mode::Pinyin));CHECK(!e.Insert("中"));
    CHECK(e.Insert(" abc\"\\!"));CHECK(e.VisibleText()==std::string(e.Text().size(),'*'));CHECK(e.VisibleText(true)==e.Text());
    CHECK(e.SetMode(Mode::Upper));CHECK(e.Type('a'));CHECK(e.Text().back()=='A');
    CHECK(!e.Insert("\n"));CHECK(e.Begin("",64,true));CHECK(e.Insert(std::string(64,'a')));CHECK(!e.Type('b'));e.Clear();CHECK(e.Text().empty());CHECK(e.Composition().empty());
    CHECK(e.Begin("你好🙂",96,false,true));CHECK(e.Backspace());CHECK(e.Text()=="你好");CHECK(e.Insert("\n测试"));CHECK(ValidUtf8(e.Text()));
    for(const auto& bad:std::vector<std::string>{std::string("\0",1),"\xc0\xaf","\xed\xa0\x80","\xf4\x90\x80\x80","\xe4\xb8","\x80"}) CHECK(!ValidUtf8(bad));
    CHECK(!e.Begin("",0));CHECK(!e.Begin("x",1537));CHECK(!e.Begin("abcd",3));
    std::set<char> keyboard;
    for(auto mode:{Mode::Lower,Mode::Upper,Mode::Symbols}) for(bool second:{false,true}) {
        auto keys=LetterKeys(mode,second);
        for(size_t i=0;i<keys.size();++i) {
            const auto& k=keys[i];keyboard.insert(k.label[0]);CHECK(k.x>=32 && k.y>=400 && k.x+k.w<=448 && k.y+k.h<=584);
            CHECK(Inside(k.x,k.y,k.x,k.y,k.w,k.h));CHECK(!Inside(k.x+k.w,k.y,k.x,k.y,k.w,k.h));
            for(size_t j=i+1;j<keys.size();++j) CHECK(!(k.x<keys[j].x+keys[j].w && keys[j].x<k.x+k.w && k.y<keys[j].y+keys[j].h && keys[j].y<k.y+k.h));
        }
    }
    for(int c=33;c<127;++c) CHECK(keyboard.count(char(c))==1);
    using namespace network;
    CHECK(ValidCredentials("中文 WiFi","",Security::Open));
    CHECK(ValidCredentials(std::string(32,'s'),std::string(63,'p'),Security::Personal));
    CHECK(ValidCredentials("x",std::string(64,'a'),Security::Personal));CHECK(!ValidCredentials("x",std::string(64,'g'),Security::Personal));
    CHECK(!ValidCredentials("x","short",Security::Personal));CHECK(!ValidCredentials("x","12345678",Security::Open));
    CHECK(!ValidCredentials("x","12345678",Security::Unsupported));CHECK(!ValidCredentials(std::string(33,'s'),"12345678",Security::Personal));
    CHECK(!ValidCredentials("x\n","12345678",Security::Personal));CHECK(!ValidCredentials("x","1234567\n",Security::Personal));
    CHECK(!ValidCredentials("x","中文密码",Security::Personal));CHECK(ValidCredentials("x"," space \"!",Security::Personal));
    auto aps=NormalizeScan({{"same",-80,Security::Personal},{"same",-30,Security::Personal},{"same",-40,Security::Open},{"",-1,Security::Open},{"中",-50,Security::Personal}});
    CHECK(aps.size()==3 && aps[0].rssi==-30 && aps[1].security==Security::Open);
    std::vector<AccessPoint> many;for(int i=0;i<100;++i) many.push_back({"network"+std::to_string(i),-i,Security::Personal});CHECK(NormalizeScan(many).size()==24);
    SetupModel m;auto op=m.Begin(true);CHECK(op);CHECK(!m.Begin(false,"x"));CHECK(m.Cancel());CHECK(m.Cancelled(op));CHECK(!m.Begin(false,"x"));
    CHECK(m.Scanned(op,true,aps));CHECK(m.Snapshot().state==SetupState::Cancelled);CHECK(m.Snapshot().access_points.empty());
    op=m.Begin(true);CHECK(m.Scanned(op,true,aps));CHECK(m.Snapshot().access_points.size()==3);
    op=m.Begin(false,"x");CHECK(!m.Finish(op-1,true,true,"old"));CHECK(m.BeginSave(op));CHECK(!m.Cancel());CHECK(m.Finish(op,true,false,"192.0.2.1"));
    CHECK(m.Snapshot().state==SetupState::Connected && !m.Snapshot().saved);CHECK(!m.Finish(op,false,false));
    op=m.Begin(false,"x");CHECK(m.Cancel());CHECK(!m.BeginSave(op));CHECK(m.Finish(op,true,true,"late"));CHECK(!m.Snapshot().saved && m.Snapshot().state==SetupState::Cancelled);
    op=m.Begin(false,"x");CHECK(m.Finish(op,false,false));CHECK(m.Snapshot().state==SetupState::Failed);
    op=m.Begin(true);CHECK(m.Scanned(op,false,{}));CHECK(m.Snapshot().state==SetupState::Failed);
    for(int i=0;i<200;++i) {auto id=m.Begin(false,"x");CHECK(id);CHECK(m.BeginSave(id));CHECK(m.Finish(id,true,true,"192.0.2.2"));}
    std::vector<std::thread> workers;for(int i=0;i<8;++i) workers.emplace_back([&](){for(int j=0;j<100;++j) (void)m.Snapshot();});for(auto& t:workers)t.join();
    std::printf("Input/network PASS: %zu checks; real Pinyin dictionary, byte-safe editor, keyboard geometry, credentials, scans and generation-fenced setup\n",checks);
}
