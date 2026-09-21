"""Host-only adapters and interaction fixtures; production drawing/routing is reused."""
HEADERS = r'''
#include <atomic>
#include "input/text_input.h"
#include "input/keyboard_layout.h"
#include "network/setup_model.h"
#include "notes/note_writer.h"
#include <climits>
static int ui_lock_depth=0;
struct DisplayLockGuard {
    template<class T> explicit DisplayLockGuard(T*) {assert(ui_lock_depth==0);++ui_lock_depth;}
    ~DisplayLockGuard(){--ui_lock_depth;}
};
enum class NetworkType {WIFI,ML307};
struct PreviewHal {
    bool wifi=true;
    bool IsWifiMode()const{return wifi;}
    bool RequestSwitchNetwork(NetworkType){assert(ui_lock_depth==0);wifi=true;return true;}
};
PreviewHal& GetHAL(){static PreviewHal h;return h;}
namespace network {
class WifiSetup {
public:
    SetupModel model;
    unsigned scans=0,connections=0;
    static WifiSetup& Instance(){static WifiSetup s;return s;}
    SetupSnapshot Snapshot()const{return model.Snapshot();}
    uint32_t Revision()const{return model.Revision();}
    bool Scan(){assert(ui_lock_depth==0);++scans;return model.Begin(true)!=0;}
    bool Connect(const AccessPoint& ap,const std::string& password){assert(ui_lock_depth==0);assert(ValidCredentials(ap.ssid,password,ap.security));++connections;return model.Begin(false,ap.ssid)!=0;}
    bool Cancel(){return model.Cancel();}
};
}
namespace notes {
Writer& Writer::Instance(){static Writer w;return w;}
WriteSnapshot Writer::Snapshot()const{return state_;}
uint32_t Writer::Save(const Note& n){assert(ui_lock_depth==0);state_.busy=true;state_.operation++;state_.revision++;return state_.operation;}
}
'''
FIELDS = r'''
    enum class HardwareKey {Previous,Next,Select,Back,Home};
    enum class EditTarget {None,WifiSsid,WifiPassword,NoteTitle,NoteProject,NoteBody,NoteSearch};
    input::Editor editor_;
    EditTarget edit_target_=EditTarget::None;
    ProductPage editor_parent_=ProductPage::WifiCredentials,discard_destination_=ProductPage::Home;
    std::atomic_bool form_active_{false};
    bool discard_pending_=false,draft_dirty_=false,password_reveal_=false,symbols_second_=false;
    bool test_console_mode_=false,screen_test_mode_=false;
    network::AccessPoint selected_ap_;
    std::string wifi_password_,form_message_,notes_query_;
    notes::Note draft_note_;
    int wifi_page_=0;
    bool wifi_manual_=false,wifi_switch_confirm_=false;
    uint32_t last_wifi_revision_=0,last_writer_revision_=0,note_save_operation_=0;
    void DrawHomeScreenLocked(){DrawProductScreenLocked();}
    void FlushLocked(){}
    void UpdateStatusBar(bool){DrawProductScreenLocked();}
    void ShowNotification(const char* msg,int){std::snprintf(notification_text_,sizeof(notification_text_),"%s",msg);}
'''
METHODS=['ClearFormLocked','LeaveFormLocked','OpenEditorLocked','FinishEditorLocked','OpenNoteEditorLocked','AdvanceFormsLocked','HandleSetupTap','HandleSetupKey']
EXERCISE = r'''
    display.ClearFormLocked();display.reminder_alert_.active=false;display.power_save_=false;
    auto& setup=network::WifiSetup::Instance();
    const auto scan=setup.model.Begin(true);assert(scan);
    assert(setup.model.Scanned(scan,true,{{"家里的 Wi-Fi",-30,network::Security::Personal},{"Guest",-40,network::Security::Open},{"Enterprise",-50,network::Security::Unsupported},{"Fourth",-55,network::Security::Personal},{"Fifth",-65,network::Security::Personal},{"Sixth",-70,network::Security::Personal}}));
    display.product_page_=RawDisplay::ProductPage::WifiList;save("wifi-list");
    assert(display.HandleSetupTap(48,240));assert(display.product_page_==RawDisplay::ProductPage::WifiCredentials);save("wifi-password-form");
    assert(display.HandleSetupTap(48,590));assert(setup.connections==0);save("wifi-password-validation");
    assert(display.HandleSetupTap(48,296));assert(display.editor_.Secret());
    for(char c:std::string("password")) {
        bool pressed=false;
        for(const auto& k:input::LetterKeys(input::Mode::Lower)) if(k.value==c) {assert(display.HandleSetupTap(k.x+1,k.y+1));pressed=true;break;}
        assert(pressed);
    }
    assert(display.editor_.Text()=="password");save("keyboard-password-masked");
    assert(display.HandleSetupTap(390,320));assert(display.password_reveal_);save("keyboard-password-revealed-fixture");
    display.password_reveal_=false;
    assert(display.HandleSetupTap(260,704));assert(display.wifi_password_=="password");
    assert(display.HandleSetupTap(48,590));assert(setup.connections==1 && display.wifi_password_.empty());save("wifi-connecting");
    assert(display.HandleSetupTap(48,590));assert(setup.Snapshot().state==network::SetupState::Cancelling);save("wifi-cancelling");
    setup.model.Finish(setup.Snapshot().operation,false,false);
    assert(display.HandleSetupTap(48,672));assert(display.product_page_==RawDisplay::ProductPage::WifiList);
    assert(display.HandleSetupTap(190,640));assert(display.wifi_manual_);
    assert(display.HandleSetupTap(48,190));assert(display.edit_target_==RawDisplay::EditTarget::WifiSsid);
    for(char c:std::string("nihao")) {
        for(const auto& k:input::LetterKeys(input::Mode::Pinyin)) if(k.value==c){assert(display.HandleSetupTap(k.x+1,k.y+1));break;}
    }
    save("keyboard-pinyin-candidates");assert(display.HandleSetupTap(40,366));assert(display.editor_.Text()=="你好");
    assert(display.HandleSetupTap(260,704));assert(display.selected_ap_.ssid=="你好");save("wifi-hidden-ssid");
    display.ClearFormLocked();display.product_page_=RawDisplay::ProductPage::Notes;
    assert(display.HandleSetupTap(48,694));assert(display.product_page_==RawDisplay::ProductPage::NoteCompose);save("note-compose-empty");
    assert(display.HandleSetupTap(48,170));display.editor_.Insert("离线笔记");display.FinishEditorLocked(true);
    assert(display.HandleSetupTap(48,345));display.editor_.Insert("中文拼音输入\n支持保存到 SD 卡。");display.FinishEditorLocked(true);save("note-compose-filled");
    assert(display.HandleSetupKey(RawDisplay::HardwareKey::Home));assert(display.discard_pending_);save("discard-confirm");
    assert(display.HandleSetupTap(40,510));assert(!display.discard_pending_ && display.draft_dirty_);
    assert(display.HandleSetupKey(RawDisplay::HardwareKey::Home));assert(display.HandleSetupTap(260,510));assert(display.product_page_==RawDisplay::ProductPage::Home && display.draft_note_.text.empty());
    display.product_page_=RawDisplay::ProductPage::NoteCompose;display.OpenEditorLocked(RawDisplay::EditTarget::NoteBody);
    for(auto mode:{input::Mode::Pinyin,input::Mode::Lower,input::Mode::Upper,input::Mode::Symbols}) {
        assert(display.editor_.SetMode(mode));save(("keyboard-mode-"+std::to_string(static_cast<int>(mode))).c_str());
    }
    display.symbols_second_=true;save("keyboard-symbols-second");
    display.editor_.Insert(std::string(1000,'a'));save("keyboard-long-text");
    display.reminder_alert_.active=true;display.password_reveal_=true;save("keyboard-alarm-priority");assert(!display.password_reveal_);
    display.reminder_alert_.active=false;display.ClearFormLocked();
    std::puts("Input UI: real touch/key routing, password masking, cancel, Pinyin candidates, notes/discard and radio-outside-lock assertions passed");
'''
