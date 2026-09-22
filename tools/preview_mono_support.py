"""Host adapters for hardware; all new drawing and input methods are production code."""
HEADERS = r'''
#include "system/quick_controls.h"
#include "reader/reader_service.h"
#include "notes/conversation_note.h"
#include "input/gesture.h"
namespace device {
QuickSnapshot preview_quick;
QuickControls& QuickControls::Instance(){static QuickControls q;return q;}
void QuickControls::Start(){}
void QuickControls::Refresh(){assert(ui_lock_depth==0);preview_quick.version="test-build";preview_quick.network="家里的 Wi-Fi";}
QuickSnapshot QuickControls::Snapshot()const{return preview_quick;}
uint32_t QuickControls::Revision()const{return preview_quick.revision;}
bool QuickControls::SetVolume(int v){assert(ui_lock_depth==0);preview_quick.volume=std::clamp(v,0,100);++preview_quick.revision;return true;}
bool QuickControls::SetAlerts(bool r,bool v){assert(ui_lock_depth==0);preview_quick.ring=r;preview_quick.vibration=v;++preview_quick.revision;return true;}
bool QuickControls::ScanBluetooth(){assert(ui_lock_depth==0);preview_quick.ble_busy=true;preview_quick.nearby.clear();++preview_quick.revision;return true;}
void QuickControls::CancelBluetooth(){assert(ui_lock_depth==0);preview_quick.ble_busy=false;++preview_quick.revision;}
}
namespace reader {
Snapshot preview_book;
Service& Service::Instance(){static Service s;return s;}
Snapshot Service::Get()const{return preview_book;}
uint32_t Service::Revision()const{return preview_book.revision;}
bool Service::List(){assert(ui_lock_depth==0);preview_book.opened=false;preview_book.files={"中文笔记.txt","Hardware.txt"};++preview_book.revision;return true;}
bool Service::Open(const std::string& s){assert(ui_lock_depth==0);preview_book.opened=true;preview_book.title=s;preview_book.page=0;preview_book.pages=3;preview_book.lines={"这是一页用于验证布局的文字。","页面来自实际阅读页的绘图函数。"};++preview_book.revision;return true;}
bool Service::Turn(int direction){assert(ui_lock_depth==0);if(direction<0){if(preview_book.page)--preview_book.page;}else if(preview_book.page+1<preview_book.pages)++preview_book.page;++preview_book.revision;return true;}
}
'''
FIELDS = r'''
    std::atomic_bool quick_controls_open_{false};
    bool quick_bluetooth_=false;
    int quick_ble_page_=0,book_list_page_=0;
    uint32_t last_quick_revision_=0,last_reader_revision_=0;
    std::string selected_card_title_,selected_card_text_;
'''
METHODS = ["SelectCardSnapshotLocked","OpenConversationNote","SetQuickControls","HandleQuickPull","HandleQuickKey","HandleQuickTap","HandleReaderTap","HandleReaderKey"]
EXERCISE = r'''
    display.ClearFormLocked();display.reminder_alert_.active=false;
    display.product_page_=RawDisplay::ProductPage::Home;
    assert(display.HandleQuickPull(240,12,246,140,500));assert(display.quick_controls_open_);save("controls-open");
    assert(display.HandleQuickTap(410,220));assert(device::preview_quick.volume==10);
    assert(display.HandleQuickTap(240,220));assert(device::preview_quick.volume==50);save("controls-volume");
    assert(display.HandleQuickTap(80,490));assert(!device::preview_quick.ring);
    assert(display.HandleQuickTap(280,490));assert(!device::preview_quick.vibration);save("controls-silent");
    assert(display.HandleQuickPull(240,600,242,400,600));assert(!display.quick_controls_open_);
    assert(display.HandleQuickTap(240,24));assert(display.HandleQuickTap(100,390));save("bluetooth-idle");
    assert(display.HandleQuickTap(100,630));assert(device::preview_quick.ble_busy);save("bluetooth-scanning");
    device::preview_quick.nearby={{"BLE test sensor","AA:BB:CC:DD:EE:FF",-45},{"中文蓝牙设备名称很长时不会溢出屏幕","01:23:45:67:89:AB",-63}};
    device::preview_quick.ble_busy=false;save("bluetooth-results");
    assert(display.HandleQuickKey(RawDisplay::HardwareKey::Home));assert(!display.quick_controls_open_);
    display.product_page_=RawDisplay::ProductPage::NoteCompose;display.OpenEditorLocked(RawDisplay::EditTarget::NoteBody);
    assert(display.editor_.Insert("你好"));save("keyboard-new-backspace");
    auto back=input::BackspaceKey(display.editor_.CurrentMode());assert(display.HandleSetupTap(back.x+1,back.y+1));assert(display.editor_.Text()=="你");
    assert(display.HandleQuickTap(200,20));assert(display.quick_controls_open_);assert(display.HandleQuickTap(80,300));assert(display.product_page_==RawDisplay::ProductPage::TextEntry);save("controls-preserve-draft");
    assert(display.HandleQuickKey(RawDisplay::HardwareKey::Home));assert(display.editor_.Text()=="你");
    display.ClearFormLocked();display.product_page_=RawDisplay::ProductPage::Home;
    conversation.Restore("保留原文的离线语音笔记","先核对整理，再点击保存。中文输入在本地处理。");
    display.OpenConversationNote();assert(display.form_active_ && display.draft_note_.raw_text=="保留原文的离线语音笔记");save("ai-archive-draft");
    display.ClearFormLocked();display.product_page_=RawDisplay::ProductPage::Reader;
    assert(display.HandleReaderTap(360,90));save("reader-library");assert(display.HandleReaderTap(60,195));save("reader-file");
    assert(display.HandleReaderKey(RawDisplay::HardwareKey::Next));assert(reader::preview_book.page==1);save("reader-file-page2");
    assert(display.HandleQuickTap(200,20));display.reminder_alert_.active=true;save("controls-alarm-priority");assert(!display.quick_controls_open_);
    display.reminder_alert_.active=false;display.ClearFormLocked();
    std::puts("Mono UI PASS: real toolbar/reader/keyboard/AI archive routing; all mocked side effects outside display lock");
'''
