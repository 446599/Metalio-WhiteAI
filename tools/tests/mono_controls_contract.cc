#include <cassert>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <string>
#include "test_platform.h"
#include "input/keyboard_layout.h"
#include "input/gesture.h"
#include "reader/book_text.h"
#include "reader/reader_service.h"
#include "system/quick_controls.h"
#include "notes/conversation_note.h"
#include "notes/note_writer.h"
#include "display/font/raw_font.h"
#include "display/font/text_layout.h"
static int checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"CHECK failed line %d: %s\n",__LINE__,#x);std::abort();}}while(0)
namespace notes {Store& DeviceStore(){static Store s([](const std::string&){return test_commit_ok;});return s;}}
int main(int argc,char** argv){
 CHECK(argc==2);const auto folder=std::filesystem::path(argv[1]);std::filesystem::create_directories(folder);
 using input::Pull;
 CHECK(input::ControlPull(200,12,205,100,500,false)==Pull::Open);
 CHECK(input::ControlPull(200,72,200,144,500,false)==Pull::None);
 CHECK(input::ControlPull(200,12,400,180,500,false)==Pull::None);
 CHECK(input::ControlPull(200,700,200,600,500,true)==Pull::Close);
 CHECK(input::ControlPull(200,12,205,100,2000,false)==Pull::None);
 CHECK(input::ControlPull(200,12,205,900,500,false)==Pull::None);
 for(auto mode:{input::Mode::Pinyin,input::Mode::Lower,input::Mode::Upper,input::Mode::Symbols}){
   auto back=input::BackspaceKey(mode);CHECK(back.w>=56);CHECK(back.x+back.w<=448);
   for(const auto& key:input::LetterKeys(mode)) {
     CHECK(key.x>=32 && key.x+key.w<=448);
     CHECK(!(key.x<back.x+back.w && back.x<key.x+key.w && key.y<back.y+back.h && back.y<key.y+key.h));
   }
 }
 input::Editor edit;CHECK(edit.Begin("你好",100));CHECK(edit.Backspace());CHECK(edit.Text()=="你");
 CHECK(edit.Type('n'));CHECK(edit.Type('i'));CHECK(edit.Backspace());CHECK(edit.Composition()=="n" && edit.Text()=="你");
 // The actual font renderer must select native-size embedded coverage first.
 const auto g=raw_font::Lookup(ui_font_body,'A');CHECK(g.bpp==2 && g.width==4 && g.height==2 && !g.missing);
 for(int y=0;y<2;++y)for(int x=0;x<4;++x)CHECK(raw_font::Pixel(g,x,y)==(y?x<2:x>=2));
 CHECK(!raw_font::Pixel(g,-1,0));CHECK(!raw_font::Pixel(g,4,0));
 const uint8_t stripes[]={0xaa,0x55,0xaa,0x55};
 for(int w=1;w<=13;++w)for(int h=1;h<=9;++h){
   raw_font::Glyph f{stripes,8,4,w,h,0,0,w+1,1,false};
   for(int y=0;y<h;++y)for(int x=0;x<w;++x){
     int ink=0,total=0;
     // Brute-force subpixel-cell reference for integer area resampling.
     for(int yy=y*4;yy<(y+1)*4;++yy)for(int xx=x*8;xx<(x+1)*8;++xx){
       ink+=(stripes[yy/h] & (0x80>>(xx/w)))?1:0;++total;
     }
     CHECK(raw_font::Pixel(f,x,y)==(2*ink>=total));
   }
 }
 CHECK(reader::FileName("中文.TXT"));for(const auto& bad:{"../x.txt",".txt","a/b.txt","a\\b.txt","bad.pdf"})CHECK(!reader::FileName(bad));
 std::string text="\xef\xbb\xbf你好\t世界\r\n";CHECK(reader::Normalize(text));CHECK(text=="你好 世界\r\n");
 text=std::string("a\0b",3);CHECK(!reader::Normalize(text));text=std::string(reader::kBookBytes+1,'x');CHECK(!reader::Normalize(text));
 notes::Note draft;std::string error;xiaozhi::ConversationSnapshot source;
 source.transcript="磁铁孔要先试装";source.answer="先制作试装件";source.content_revision=3;
 CHECK(!notes::PrepareConversationNote(source,draft,error));source.state=xiaozhi::TurnState::Done;
 CHECK(notes::PrepareConversationNote(source,draft,error));CHECK(draft.raw_text==source.transcript && draft.text==source.answer && draft.source_id.size()==16);
 auto& store=notes::DeviceStore();CHECK(store.Restore(""));notes::Note saved;bool duplicate=false;draft.updated=0;
 CHECK(store.Archive(draft,saved,duplicate,error));CHECK(!duplicate);CHECK(store.Archive(draft,saved,duplicate,error));CHECK(duplicate && store.List().size()==1);
 source.truncated=true;CHECK(!notes::PrepareConversationNote(source,draft,error));source.truncated=false;source.answer.assign(2000,'a');
 CHECK(notes::PrepareConversationNote(source,draft,error));CHECK(draft.text==source.transcript && error.find("超过")!=std::string::npos);
 // Execute the actual writer worker, including UI -> immutable archive path.
 CHECK(notes::Writer::Instance().Save(draft)!=0);RunTasks();CHECK(notes::Writer::Instance().Snapshot().ok);CHECK(store.List().size()==2);
 auto& settings=device::QuickControls::Instance();settings.Start();settings.Refresh();CHECK(settings.RingEnabled() && settings.VibrationEnabled());
 CHECK(settings.SetAlerts(false,true));CHECK(!settings.RingEnabled() && settings.VibrationEnabled());
 test_commit_ok=false;CHECK(!settings.SetAlerts(true,false));CHECK(!settings.RingEnabled() && settings.VibrationEnabled());test_commit_ok=true;
 CHECK(settings.SetVolume(500));CHECK(settings.Snapshot().volume==100);
 CHECK(settings.SetVolume(-50));CHECK(settings.Snapshot().volume==0);
 xiaozhi::AudioSession::GetInstance().stats.capturing=true;CHECK(!settings.ScanBluetooth());xiaozhi::AudioSession::GetInstance().stats.capturing=false;
 test_task_ok=false;CHECK(!settings.ScanBluetooth());CHECK(!settings.Snapshot().ble_busy);test_task_ok=true;
 CHECK(settings.ScanBluetooth());CHECK(!settings.ScanBluetooth());settings.CancelBluetooth();RunTasks();CHECK(settings.Snapshot().ble_cancelled && settings.Snapshot().nearby.empty());
 CHECK(settings.ScanBluetooth());RunTasks();CHECK(settings.Snapshot().ble_ok && settings.Snapshot().nearby.size()==1);
 // Actual bounded book service reads host files, creates pages and NVS bookmarks.
 std::ofstream(folder/"chapter.txt")<<std::string(3000,'A');std::ofstream(folder/"ignore.pdf")<<"not txt";
 auto& books=reader::Service::Instance();CHECK(books.List());CHECK(!books.List());RunTasks();CHECK(books.Get().files.size()==1);
 CHECK(!books.Open("../chapter.txt"));CHECK(books.Open("chapter.txt"));RunTasks();CHECK(books.Get().opened && books.Get().pages>1);
 CHECK(books.Turn(1));RunTasks();CHECK(books.Get().page==1);const auto bookmark=durable["reader/position"];
 CHECK(books.List());RunTasks();CHECK(books.Open("chapter.txt"));RunTasks();CHECK(books.Get().page==1);
 test_commit_ok=false;CHECK(books.Turn(1));RunTasks();CHECK(books.Get().page==2 && books.Get().message.find("失败")!=std::string::npos);CHECK(durable["reader/position"]==bookmark);test_commit_ok=true;
 GetHAL().sd=false;CHECK(books.Open("chapter.txt"));RunTasks();CHECK(books.Get().message=="SD 卡不可用");CHECK(books.Get().page==2);GetHAL().sd=true;
 std::ofstream(folder/"invalid.txt",std::ios::binary)<<std::string("a\0b",3);CHECK(books.Open("invalid.txt"));RunTasks();CHECK(books.Get().message.find("UTF-8")!=std::string::npos);
 std::ofstream(folder/"large.txt")<<std::string(reader::kBookBytes+1,'x');CHECK(books.Open("large.txt"));RunTasks();CHECK(books.Get().message.find("256")!=std::string::npos);
 test_task_ok=false;CHECK(!books.List());CHECK(!books.Get().busy);test_task_ok=true;
 std::printf("Mono controls PASS: %d CHECKs; native font/area resampling, layout, immutable archive, alert rollback, BLE lifecycle requests and actual book files/bookmarks (hardware mocked)\n",checks);
}
