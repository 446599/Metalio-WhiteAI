#include "raw_display.h"
#include "input/keyboard_layout.h"
#include "font/raw_font.h"
#include "font/text_layout.h"
#include "network/wifi_setup.h"
#include "notes/note_service.h"
#include "notes/note_writer.h"
#include "hal/hal.h"
#include "application.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

void RawDisplay::ClearFormLocked() {
    editor_.Clear();input::Wipe(wifi_password_);draft_note_={};form_message_.clear();
    edit_target_=EditTarget::None;form_active_.store(false);discard_pending_=draft_dirty_=password_reveal_=false;
    note_save_operation_=0;
}
void RawDisplay::LeaveFormLocked(ProductPage destination) {
    if(note_save_operation_ || network::WifiSetup::Instance().Snapshot().state==network::SetupState::Saving) return;
    if(draft_dirty_ || (product_page_==ProductPage::TextEntry && !editor_.Text().empty())) {
        discard_pending_=true;discard_destination_=destination;password_reveal_=false;return;
    }
    ClearFormLocked();product_page_=destination;navigation_index_=0;
}
void RawDisplay::OpenEditorLocked(EditTarget target) {
    edit_target_=target;editor_parent_=product_page_;password_reveal_=false;symbols_second_=false;
    switch(target) {
        case EditTarget::WifiSsid: editor_.Begin(selected_ap_.ssid,32);break;
        case EditTarget::WifiPassword: editor_.Begin(wifi_password_,64,true);break;
        case EditTarget::NoteTitle: editor_.Begin(draft_note_.title,notes::Store::kTitleBytes);break;
        case EditTarget::NoteProject: editor_.Begin(draft_note_.project,notes::Store::kProjectBytes);break;
        case EditTarget::NoteBody: editor_.Begin(draft_note_.text,notes::Store::kTextBytes,false,true);break;
        case EditTarget::NoteSearch: editor_.Begin(notes_query_,192);break;
        default: return;
    }
    product_page_=ProductPage::TextEntry;form_active_.store(true);
}
void RawDisplay::FinishEditorLocked(bool accept) {
    if(accept && !editor_.Ready()) {form_message_="请先选择候选字，或删除未完成拼音";return;}
    if(accept) {
        switch(edit_target_) {
            case EditTarget::WifiSsid: selected_ap_.ssid=editor_.Text();break;
            case EditTarget::WifiPassword: input::Wipe(wifi_password_);wifi_password_=editor_.Text();break;
            case EditTarget::NoteTitle: draft_dirty_|=draft_note_.title!=editor_.Text();draft_note_.title=editor_.Text();break;
            case EditTarget::NoteProject: draft_dirty_|=draft_note_.project!=editor_.Text();draft_note_.project=editor_.Text();break;
            case EditTarget::NoteBody: draft_dirty_|=draft_note_.text!=editor_.Text();draft_note_.text=editor_.Text();break;
            case EditTarget::NoteSearch: notes_query_=editor_.Text();notes_page_=navigation_index_=0;break;
            default: break;
        }
    }
    product_page_=editor_parent_;editor_.Clear();form_message_.clear();password_reveal_=false;edit_target_=EditTarget::None;
    form_active_.store(product_page_==ProductPage::NoteCompose || product_page_==ProductPage::WifiCredentials);
}
void RawDisplay::OpenNoteEditorLocked(bool existing) {
    draft_note_={};draft_dirty_=false;form_message_.clear();note_save_operation_=0;
    if(existing) {
        const auto notes=notes::DeviceStore().List();
        const auto it=std::find_if(notes.begin(),notes.end(),[this](const auto& n){return n.id==note_id_;});
        if(it==notes.end()) return;
        draft_note_=*it;
    }
    product_page_=ProductPage::NoteCompose;form_active_.store(true);
}
void RawDisplay::AdvanceFormsLocked() {
    if(product_page_!=ProductPage::NoteCompose || !note_save_operation_ || note_save_operation_==UINT32_MAX) return;
    const auto result=notes::Writer::Instance().Snapshot();
    if(result.operation!=note_save_operation_ || result.busy) return;
    note_save_operation_=0;
    if(result.ok) {
        const uint32_t id=result.id;ClearFormLocked();note_id_=id;note_text_page_=0;product_page_=ProductPage::NoteDetail;
    } else form_message_=result.error.empty() ? "保存失败，草稿仍在" : result.error;
}

bool RawDisplay::HandleSetupTap(int x,int y) {
    enum class Action {None,Scan,Connect,SwitchWifi,Save};Action action=Action::None;
    network::AccessPoint connect_ap;std::string password;notes::Note note;
    {
        DisplayLockGuard lock(this);
        if(!portrait_fb_ || test_console_mode_ || screen_test_mode_ || power_save_ || reminder_alert_.active) return false;
        const auto hit=[&](int l,int t,int w,int h){return input::Inside(x,y,l,t,w,h);};
        const bool owned=product_page_==ProductPage::WifiList || product_page_==ProductPage::WifiCredentials ||
            product_page_==ProductPage::TextEntry || product_page_==ProductPage::NoteCompose;
        if(discard_pending_) {
            if(hit(32,496,200,64)) discard_pending_=false;
            else if(hit(248,496,200,64)) {const auto target=discard_destination_;ClearFormLocked();product_page_=target;}
        } else if(product_page_==ProductPage::Notes && y>=672 && y<720) {
            if(hit(32,672,132,48)) OpenNoteEditorLocked(false);
            else if(hit(174,672,132,48)) OpenEditorLocked(EditTarget::NoteSearch);
            else if(hit(316,672,132,48)) {notes_page_=(notes_page_+1)%notes_pages_;navigation_index_=0;}
        } else if(product_page_==ProductPage::NoteDetail && hit(344,72,104,48)) {
            OpenNoteEditorLocked(true);
        } else if(!owned) return false;
        else if(product_page_==ProductPage::TextEntry) {
            form_message_.clear();
            bool letter=false;
            for(const auto& key:input::LetterKeys(editor_.CurrentMode(),symbols_second_)) {
                if(hit(key.x,key.y,key.w,key.h)) {editor_.Type(key.value);letter=true;break;}
            }
            if(!letter) {
                if(hit(32,592,92,56)) {
                    const auto mode=editor_.CurrentMode();
                    editor_.SetMode(mode==input::Mode::Lower ? input::Mode::Upper : mode==input::Mode::Upper && !editor_.Secret() ? input::Mode::Pinyin : input::Mode::Lower);
                } else if(hit(128,592,80,56)) {
                    if(editor_.CurrentMode()==input::Mode::Symbols) symbols_second_=!symbols_second_;
                    else {symbols_second_=false;editor_.SetMode(input::Mode::Symbols);}
                } else if(hit(212,592,128,56)) editor_.Space();
                else if(hit(344,592,104,56)) editor_.Backspace();
                else if(hit(248,304,56,40)) editor_.Move(-1);
                else if(hit(312,304,56,40)) editor_.Move(1);
                else if(hit(376,304,72,40)) {
                    if(editor_.Secret()) password_reveal_=!password_reveal_;
                    else if(editor_.Multiline() && editor_.Ready()) editor_.Insert("\n");
                    else if(editor_.CurrentMode()==input::Mode::Pinyin) editor_.Type('\'');
                } else if(hit(400,352,48,40)) editor_.NextCandidates();
                else if(hit(32,688,200,48)) FinishEditorLocked(false);
                else if(hit(248,688,200,48)) FinishEditorLocked(true);
                else for(size_t i=0;i<input::Editor::kCandidatesPerPage;++i)
                    if(hit(32+static_cast<int>(i)*92,352,84,40)) {editor_.Choose(i);break;}
            }
        } else if(product_page_==ProductPage::WifiList) {
            const auto status=network::WifiSetup::Instance().Snapshot();
            if(hit(32,688,416,48)) {
                network::WifiSetup::Instance().Cancel();ClearFormLocked();product_page_=ProductPage::More;
            } else if(!GetHAL().IsWifiMode()) {
                if(hit(32,624,416,48)) {
                    if(wifi_switch_confirm_) action=Action::SwitchWifi;
                    else {wifi_switch_confirm_=true;form_message_="再次点击将切换网络并重启";}
                }
            } else if(network::Busy(status.state)) {
                if(hit(32,624,132,48)) network::WifiSetup::Instance().Cancel();
            } else if(hit(32,624,132,48)) {wifi_page_=navigation_index_=0;action=Action::Scan;}
            else if(hit(174,624,132,48)) {selected_ap_={};wifi_manual_=true;input::Wipe(wifi_password_);form_message_.clear();product_page_=ProductPage::WifiCredentials;form_active_.store(true);}
            else if(hit(316,624,132,48)) {const int pages=std::max(1,(static_cast<int>(status.access_points.size())+4)/5);wifi_page_=(wifi_page_+1)%pages;navigation_index_=0;}
            else for(int row=0;row<5;++row) {
                const auto index=static_cast<size_t>(wifi_page_*5+row);
                if(index<status.access_points.size() && hit(32,216+row*80,416,68)) {
                    selected_ap_=status.access_points[index];wifi_manual_=false;input::Wipe(wifi_password_);form_message_.clear();
                    if(selected_ap_.security==network::Security::Unsupported) form_message_="暂不支持此网络认证方式";
                    else {product_page_=ProductPage::WifiCredentials;form_active_.store(true);}
                    break;
                }
            }
        } else if(product_page_==ProductPage::WifiCredentials) {
            const auto status=network::WifiSetup::Instance().Snapshot();
            if(network::Busy(status.state)) {
                if(hit(32,560,416,64)) network::WifiSetup::Instance().Cancel();
            } else if(hit(32,168,416,80) && wifi_manual_) OpenEditorLocked(EditTarget::WifiSsid);
            else if(hit(32,272,416,80) && selected_ap_.security!=network::Security::Open) OpenEditorLocked(EditTarget::WifiPassword);
            else if(hit(32,368,416,48) && wifi_manual_) {
                selected_ap_.security=selected_ap_.security==network::Security::Open ? network::Security::Personal : network::Security::Open;
                input::Wipe(wifi_password_);
            } else if(hit(32,560,416,64)) {
                if(!network::ValidCredentials(selected_ap_.ssid,wifi_password_,selected_ap_.security)) form_message_="名称 1–32 字节；密码 8–63 位或 64 位十六进制";
                else {connect_ap=selected_ap_;password=wifi_password_;action=Action::Connect;}
            } else if(hit(32,648,416,64)) {ClearFormLocked();product_page_=ProductPage::WifiList;}
        } else if(product_page_==ProductPage::NoteCompose) {
            if(!note_save_operation_) {
                if(hit(32,160,416,72)) OpenEditorLocked(EditTarget::NoteTitle);
                else if(hit(32,248,416,72)) OpenEditorLocked(EditTarget::NoteProject);
                else if(hit(32,336,416,160)) OpenEditorLocked(EditTarget::NoteBody);
                else if(hit(32,560,416,64)) {
                    if(draft_note_.title.empty() || draft_note_.text.empty()) form_message_="请填写标题和正文";
                    else {note=draft_note_;note_save_operation_=UINT32_MAX;action=Action::Save;}
                } else if(hit(32,648,416,64)) LeaveFormLocked(ProductPage::Notes);
            }
        }
        DrawHomeScreenLocked();FlushLocked();
    }
    // All radio calls, flash writes and task starts happen after releasing the display lock.
    if(action==Action::Scan && !network::WifiSetup::Instance().Scan()) ShowNotification("扫描未启动，请稍后重试",2500);
    if(action==Action::SwitchWifi && !GetHAL().RequestSwitchNetwork(NetworkType::WIFI)) ShowNotification("切换未启动",2500);
    if(action==Action::Connect) {
        const bool ok=network::WifiSetup::Instance().Connect(connect_ap,password);input::Wipe(password);
        {DisplayLockGuard lock(this);input::Wipe(wifi_password_);form_message_=ok ? "" : "连接未启动，请重新输入";}
        UpdateStatusBar(true);
    }
    if(action==Action::Save) {
        const auto operation=notes::Writer::Instance().Save(note);
        {DisplayLockGuard lock(this);note_save_operation_=operation;if(!operation) form_message_="保存未启动，草稿仍保留";}
        UpdateStatusBar(true);
    }
    return true;
}

bool RawDisplay::HandleSetupKey(HardwareKey key) {
    int tap_x=-1,tap_y=-1;
    {
        DisplayLockGuard lock(this);
        if(test_console_mode_ || screen_test_mode_ || power_save_ || reminder_alert_.active) return false;
        const bool owned=product_page_==ProductPage::WifiList || product_page_==ProductPage::WifiCredentials ||
            product_page_==ProductPage::TextEntry || product_page_==ProductPage::NoteCompose;
        if(!owned && !discard_pending_) return false;
        if(discard_pending_) {if(key==HardwareKey::Back || key==HardwareKey::Previous || key==HardwareKey::Home) discard_pending_=false;}
        else if(key==HardwareKey::Home) {network::WifiSetup::Instance().Cancel();LeaveFormLocked(ProductPage::Home);}
        else if(product_page_==ProductPage::TextEntry) {
            if(key==HardwareKey::Previous || key==HardwareKey::Back) FinishEditorLocked(false);
            else if(key==HardwareKey::Next) {if(editor_.Ready()) editor_.Move(1);else editor_.NextCandidates();}
            else if(key==HardwareKey::Select) {if(editor_.Ready()) FinishEditorLocked(true);else editor_.Choose(0);}
        } else if(product_page_==ProductPage::WifiList) {
            if(key==HardwareKey::Previous || key==HardwareKey::Back) {tap_x=40;tap_y=704;}
            else if(key==HardwareKey::Next) {const int count=static_cast<int>(network::WifiSetup::Instance().Snapshot().access_points.size());
                if(++navigation_index_>=5 || wifi_page_*5+navigation_index_>=count) {navigation_index_=0;wifi_page_=(wifi_page_+1)%std::max(1,(count+4)/5);}}
            else if(key==HardwareKey::Select) {tap_x=40;tap_y=network::WifiSetup::Instance().Snapshot().access_points.empty() ? 640 : 240+navigation_index_*80;}
        } else if(key==HardwareKey::Previous || key==HardwareKey::Back) {tap_x=40;tap_y=680;}
        else if(key==HardwareKey::Select) {tap_x=40;tap_y=590;}
        if(tap_x<0) {DrawHomeScreenLocked();FlushLocked();}
    }
    if(tap_x>=0) HandleSetupTap(tap_x,tap_y);
    return true;
}

void RawDisplay::DrawProductTextEntryLocked() {
    std::memset(portrait_fb_,0xff,portrait_size_);DrawProductStatusBarLocked();
    const char* title="输入文字";
    switch(edit_target_) {
        case EditTarget::WifiSsid:title="网络名称";break;case EditTarget::WifiPassword:title="Wi-Fi 密码";break;
        case EditTarget::NoteTitle:title="笔记标题";break;case EditTarget::NoteProject:title="所属项目";break;
        case EditTarget::NoteBody:title="笔记正文";break;case EditTarget::NoteSearch:title="搜索笔记";break;default:break;
    }
    DrawProductHeadingLocked(title,"输入");StrokeRoundRect(32,136,416,136,12,1);
    auto visible=editor_.VisibleText(password_reveal_);visible.insert(editor_.Cursor(),"|");
    const auto measure=[](uint32_t cp){return raw_font::Lookup(ui_font_small,cp).advance;};
    const auto before=raw_font::Paginate(visible.substr(0,editor_.Cursor()+1),384,0,1,measure);
    const auto page=raw_font::Paginate(visible,384,(before.pages-1)/3,3,measure);
    for(size_t i=0;i<page.lines.size();++i) DrawText(48,152+static_cast<int>(i)*36,page.lines[i].c_str(),ui_font_small);
    char size[48];std::snprintf(size,sizeof(size),"%u / %u 字节",static_cast<unsigned>(editor_.Text().size()),static_cast<unsigned>(editor_.Limit()));
    DrawProductLabelLocked(32,276,416,size,ui_font_small);
    DrawProductLabelLocked(32,310,208,editor_.Composition().empty() ? "点击键盘输入" : editor_.Composition().c_str(),ui_font_small);
    const auto button=[&](int x,int y,int w,int h,const char* label) {StrokeRoundRect(x,y,w,h,8,1);DrawTextCentered(x,y,w,h,label,ui_font_small);};
    button(248,304,56,40,"←");button(312,304,56,40,"→");
    button(376,304,72,40,editor_.Secret() ? (password_reveal_ ? "隐藏" : "显示") : editor_.Multiline() ? "换行" : "分词");
    const auto candidates=editor_.Candidates();
    for(size_t i=0;i<candidates.size();++i) {StrokeRoundRect(32+i*92,352,84,40,8,1);DrawProductLabelLocked(38+i*92,358,72,candidates[i].text.c_str(),ui_font_small);}
    if(!editor_.Composition().empty()) button(400,352,48,40,"›");
    for(const auto& key:input::LetterKeys(editor_.CurrentMode(),symbols_second_)) button(key.x,key.y,key.w,key.h,key.label.c_str());
    const char* mode=editor_.CurrentMode()==input::Mode::Pinyin ? "中文" : editor_.CurrentMode()==input::Mode::Upper ? "ABC" : "abc";
    button(32,592,92,56,mode);button(128,592,80,56,editor_.CurrentMode()==input::Mode::Symbols ? "更多" : "123");
    button(212,592,128,56,"空格");button(344,592,104,56,"退格");
    const auto& error=form_message_.empty() ? editor_.Error() : form_message_;
    DrawProductLabelLocked(32,652,416,error.c_str(),ui_font_small);
    button(32,688,200,48,"取消");button(248,688,200,48,"完成");
    DrawProductControlRailLocked(editor_.Secret() ? "密码仅用于本机联网，不发送给 AI" : "全拼选字 / v 代替 ü / 按字节限制长度");
}
void RawDisplay::DrawProductWifiLocked(bool credentials) {
    std::memset(portrait_fb_,0xff,portrait_size_);DrawProductStatusBarLocked();
    const auto status=network::WifiSetup::Instance().Snapshot();
    const bool busy=network::Busy(status.state);
    const auto button=[&](int x,int y,int w,int h,const char* label) {StrokeRoundRect(x,y,w,h,12,1);DrawTextCentered(x,y,w,h,label,ui_font_small);};
    if(!credentials) {
        DrawProductHeadingLocked("Wi-Fi","设置");
        DrawProductLabelLocked(32,136,416,form_message_.empty() ? status.message.c_str() : form_message_.c_str(),ui_font_small);
        if(!GetHAL().IsWifiMode()) {
            DrawText(32,216,"当前使用 4G 网络",ui_font_body);
            DrawText(32,280,"切换到 Wi-Fi 需要重启设备。",ui_font_small);
            button(32,624,416,48,wifi_switch_confirm_ ? "确认切换并重启" : "切换到 Wi-Fi");
        } else {
            DrawProductLabelLocked(32,176,416,"2.4 GHz 网络 / 选择后输入密码",ui_font_small);
            const int pages=std::max(1,(static_cast<int>(status.access_points.size())+4)/5);wifi_page_=std::clamp(wifi_page_,0,pages-1);
            for(int row=0;row<5;++row) {
                const size_t index=wifi_page_*5+row;if(index>=status.access_points.size()) break;
                const auto& ap=status.access_points[index];char detail[64];
                std::snprintf(detail,sizeof(detail),"%d dBm / %s",ap.rssi,ap.security==network::Security::Open ? "开放网络" : ap.security==network::Security::Personal ? "需要密码" : "暂不支持");
                StrokeRoundRect(32,216+row*80,416,68,12,navigation_index_==row ? 2 : 1);
                DrawProductLabelLocked(48,224+row*80,384,ap.ssid.c_str(),ui_font_small);
                DrawProductLabelLocked(48,254+row*80,384,detail,ui_font_status);
            }
            if(status.access_points.empty() && !busy) DrawProductLabelLocked(32,240,416,"点击扫描，或手动添加隐藏网络。",ui_font_small);
            button(32,624,132,48,busy ? "取消" : "扫描");button(174,624,132,48,"手动添加");
            char page[32];std::snprintf(page,sizeof(page),"%d/%d 下一页",wifi_page_+1,pages);button(316,624,132,48,page);
        }
        button(32,688,416,48,"返回设置");DrawProductControlRailLocked("密码只在连接成功后保存");return;
    }
    DrawProductHeadingLocked("连接网络","Wi-Fi");
    button(32,168,416,80,"");DrawProductLabelLocked(48,176,384,wifi_manual_ ? "名称 · 点击输入" : "网络名称",ui_font_small);
    DrawProductLabelLocked(48,212,384,selected_ap_.ssid.empty() ? "尚未填写" : selected_ap_.ssid.c_str(),ui_font_small);
    button(32,272,416,80,"");DrawText(48,280,"密码 · 点击输入",ui_font_small);
    const std::string masked=selected_ap_.security==network::Security::Open ? "开放网络，无需密码" : wifi_password_.empty() ? "尚未填写" : std::string(wifi_password_.size(),'*');
    DrawProductLabelLocked(48,316,384,masked.c_str(),ui_font_small);
    if(wifi_manual_) button(32,368,416,48,selected_ap_.security==network::Security::Open ? "开放网络 · 点击改为加密" : "加密网络 · 点击改为开放");
    const std::string message=!form_message_.empty() ? form_message_ : status.ssid==selected_ap_.ssid ? status.message : "确认网络名称，输入密码后连接。";
    const auto lines=raw_font::Paginate(message,416,0,2,[](uint32_t cp){return raw_font::Lookup(ui_font_small,cp).advance;});
    for(size_t i=0;i<lines.lines.size();++i) DrawText(32,432+i*36,lines.lines[i].c_str(),ui_font_small);
    if(status.ssid==selected_ap_.ssid && status.state==network::SetupState::Connected) DrawProductLabelLocked(32,510,416,status.ip.c_str(),ui_font_small);
    button(32,560,416,64,status.state==network::SetupState::Saving ? "正在保存" : busy ? "取消连接" : "连接网络");
    button(32,648,416,64,"返回网络列表");DrawProductControlRailLocked("空格和大小写均按原样保留");
}
void RawDisplay::DrawProductNoteComposeLocked() {
    std::memset(portrait_fb_,0xff,portrait_size_);DrawProductStatusBarLocked();DrawProductHeadingLocked(draft_note_.id ? "编辑笔记" : "新建笔记","本地");
    const auto field=[&](int y,int h,const char* label,const std::string& value) {
        StrokeRoundRect(32,y,416,h,12,1);DrawText(48,y+8,label,ui_font_small);
        const auto lines=raw_font::Paginate(value.empty() ? "点击输入" : value,384,0,h>80 ? 3 : 1,[](uint32_t cp){return raw_font::Lookup(ui_font_small,cp).advance;});
        for(size_t i=0;i<lines.lines.size();++i) DrawText(48,y+38+i*32,lines.lines[i].c_str(),ui_font_small);
    };
    field(160,72,"标题",draft_note_.title);field(248,72,"项目（可选）",draft_note_.project);field(336,160,"正文",draft_note_.text);
    DrawProductLabelLocked(32,510,416,form_message_.empty() ? (draft_note_.raw_text.empty() ? "离线编辑，保存到 SD 卡" : "只修改整理版，归档原文保持不变") : form_message_.c_str(),ui_font_small);
    StrokeRoundRect(32,560,416,64,12,1);DrawTextCentered(32,560,416,64,note_save_operation_ ? "正在保存" : "保存笔记",ui_font_small);
    StrokeRoundRect(32,648,416,64,12,1);DrawTextCentered(32,648,416,64,"返回笔记目录",ui_font_small);
    DrawProductControlRailLocked("未保存修改会在退出前确认");
}
void RawDisplay::DrawProductDiscardLocked() {
    std::memset(portrait_fb_,0xff,portrait_size_);DrawProductStatusBarLocked();DrawProductHeadingLocked("放弃修改？","确认");
    DrawText(32,224,"未保存的输入将被丢弃。",ui_font_body);
    DrawText(32,288,"原有笔记与网络配置不会改变。",ui_font_small);
    StrokeRoundRect(32,496,200,64,12,1);DrawTextCentered(32,496,200,64,"继续编辑",ui_font_small);
    StrokeRoundRect(248,496,200,64,12,1);DrawTextCentered(248,496,200,64,"放弃修改",ui_font_small);
    DrawProductControlRailLocked("HOME / PREV 继续编辑");
}
