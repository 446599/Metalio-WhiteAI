#include "raw_display.h"
#include "input/keyboard_layout.h"
#include "chat/history_service.h"
#include "xiaozhi/xiaozhi_client.h"
#include "font/raw_font.h"
#include "font/text_layout.h"
#include "reminders/reminder_store.h"
#include <algorithm>
#include <esp_timer.h>
#include <cstdio>
#include <cstring>

void RawDisplay::DrawProductHistoryLocked(bool detail){
    std::memset(portrait_fb_,0xff,portrait_size_);DrawProductStatusBarLocked();
    const auto view=chat::History::Instance().Snapshot();
    const auto button=[&](int x,int y,int w,const char* text){StrokeRoundRect(x,y,w,48,12,1);DrawTextCentered(x,y,w,48,text,ui_font_small);};
    if(history_delete_confirm_){
        DrawProductHeadingLocked("删除这段对话？","");
        DrawProductLabelLocked(32,232,416,"从历史列表移除，不影响其他会话。",ui_font_body);
        DrawProductLabelLocked(32,296,416,"SD 卡保留 .deleted 目录用于恢复。",ui_font_small);
        button(32,496,200,"取消");button(248,496,200,"确认删除");
        DrawProductControlRailLocked("");return;
    }
    if(!detail){
        char count[32];std::snprintf(count,sizeof(count),"%u / 64",static_cast<unsigned>(view.sessions.size()));
        DrawProductHeadingLocked("历史对话",count);
        DrawProductLabelLocked(32,144,416,view.busy?"正在读取历史…":view.message.c_str(),ui_font_small);
        const int pages=std::max(1,(static_cast<int>(view.sessions.size())+4)/5);
        history_list_page_=std::clamp(history_list_page_,0,pages-1);
        for(int row=0;row<5&&history_list_page_*5+row<static_cast<int>(view.sessions.size());++row){
            const auto& session=view.sessions[history_list_page_*5+row];const int y=192+row*84;
            DrawProductIconLocked(lucide::Id::MessageSquare,38,y+16,28,true);
            DrawProductLabelLocked(88,y,328,session.title.c_str(),ui_font_body);
            std::string meta=session.updated?reminders::LocalTime(session.updated).substr(5,11):"离线会话";
            meta+=" / "+std::to_string(session.turns)+" 轮";if(session.damaged)meta="部分记录损坏 / 原文件保留";
            DrawProductLabelLocked(88,y+40,328,meta.c_str(),ui_font_small);FillRect(88,y+76,360,1,true);
        }
        if(view.sessions.empty()&&!view.busy){DrawProductIconLocked(lucide::Id::MessageSquare,220,264,40,true);
            DrawTextCentered(32,332,416,48,"还没有历史对话",ui_font_body);DrawTextCentered(32,396,416,40,"插入 SD 卡后自动保存每轮问答",ui_font_small);}
        button(32,624,132,"新建");button(174,624,132,"上一页");button(316,624,132,"下一页");button(32,688,416,"返回小智");
        DrawProductControlRailLocked("");return;
    }
    DrawProductHeadingLocked("对话详情","");button(344,72,104,"返回");
    if(!view.turn){DrawProductLabelLocked(32,192,416,view.busy?"正在读取…":view.message.c_str(),ui_font_body);return;}
    const auto& turn=*view.turn;
    std::string status="第 "+std::to_string(view.turn_index+1)+" / "+std::to_string(view.turn_count)+" 轮";
    if(!turn.action.empty())status+=" / "+turn.action;
    DrawProductLabelLocked(32,144,416,status.c_str(),ui_font_small);
    std::string text="我\n"+turn.user+"\n\n小智\n"+(turn.assistant.empty()?"（尚无回答）":turn.assistant);
    if(turn.status!="complete")text+="\n\n[未完成：中断、错误或仍等待回答]";
    if(turn.truncated)text+="\n[长内容仅保存设备收到的前段]";
    const auto page=raw_font::Paginate(text,416,history_text_page_,10,[](uint32_t cp){return raw_font::Lookup(ui_font_body,cp).advance;});
    history_text_page_=page.page;history_text_pages_=page.pages;
    for(size_t i=0;i<page.lines.size();++i)DrawText(32,190+static_cast<int>(i)*38,page.lines[i].c_str(),ui_font_body);
    char paging[32];std::snprintf(paging,sizeof(paging),"%d / %d",history_text_page_+1,history_text_pages_);
    DrawTextCentered(32,572,416,28,paging,ui_font_small);
    const char* labels[]={"上页","下页","上轮","下轮"};for(int i=0;i<4;++i)button(32+i*108,608,92,labels[i]);
    button(32,680,200,"继续对话");button(248,680,200,"删除对话");
    DrawProductControlRailLocked(view.busy?"正在读取历史":view.failed?view.message.c_str():"最近两轮作为继续聊天的上下文");
}

bool RawDisplay::HandleHistoryTap(int x,int y){
    enum class Action {None,List,Open,Move,New,Resume,Delete,Text};Action action=Action::None;
    uint32_t id=0;int direction=0;std::string text;
    {
        DisplayLockGuard lock(this);
        if(!portrait_fb_||test_console_mode_||screen_test_mode_||power_save_||reminder_alert_.active||quick_controls_open_.load()||discard_pending_)return false;
        const auto hit=[&](int l,int t,int w,int h){return input::Inside(x,y,l,t,w,h);};
        if(product_page_==ProductPage::TextEntry&&edit_target_==EditTarget::ChatMessage&&hit(248,688,200,48)){
            if(!editor_.Ready()||editor_.Text().empty()){form_message_="请完成输入并选择候选字";DrawHomeScreenLocked();FlushLocked();return true;}
            text=editor_.Text();FinishEditorLocked(true);action=Action::Text;
        }else if(product_page_==ProductPage::AiResult||product_page_==ProductPage::AiSteps){
            const auto state=xiaozhi::Conversation::GetInstance().Snapshot();
            const bool busy=state.state==xiaozhi::TurnState::Connecting||state.state==xiaozhi::TurnState::Listening||state.state==xiaozhi::TurnState::Transcribing||state.state==xiaozhi::TurnState::Thinking||state.state==xiaozhi::TurnState::Speaking;
            if(busy||y<72||y>=120)return false;
            if(hit(188,72,84,48)){OpenEditorLocked(EditTarget::ChatMessage);}
            else if(hit(276,72,84,48)){history_list_page_=0;product_page_=ProductPage::ChatList;action=Action::List;}
            else if(hit(364,72,84,48))action=Action::New;
            else return false;
        }else if(product_page_!=ProductPage::ChatList&&product_page_!=ProductPage::ChatDetail)return false;
        else{
            const auto view=chat::History::Instance().Snapshot();
            if(history_delete_confirm_){
                if(hit(32,496,200,48))history_delete_confirm_=false;
                else if(hit(248,496,200,48)){action=Action::Delete;id=view.selected;history_delete_confirm_=false;}
            }else if(product_page_==ProductPage::ChatList){
                if(hit(32,688,416,48)){product_page_=ProductPage::AiResult;}
                else if(hit(32,624,132,48))action=Action::New;
                else if(hit(174,624,132,48))history_list_page_=std::max(0,history_list_page_-1);
                else if(hit(316,624,132,48))++history_list_page_;
                else if(hit(32,136,416,44)){chat::History::Instance().Retry();action=Action::List;}
                else if(!view.busy)for(int row=0;row<5;++row){size_t index=history_list_page_*5+row;
                    if(index<view.sessions.size()&&hit(32,192+row*84,416,76)){id=view.sessions[index].id;action=Action::Open;history_text_page_=0;break;}}
            }else{
                if(hit(344,72,104,48)){product_page_=ProductPage::ChatList;action=Action::List;}
                else if(!view.busy){
                    if(hit(32,608,92,48))history_text_page_=std::max(0,history_text_page_-1);
                    else if(hit(140,608,92,48))history_text_page_=std::min(history_text_page_+1,history_text_pages_-1);
                    else if(hit(248,608,92,48)){action=Action::Move;direction=-1;history_text_page_=0;}
                    else if(hit(356,608,92,48)){action=Action::Move;direction=1;history_text_page_=0;}
                    else if(hit(32,680,200,48)){action=Action::Resume;id=view.selected;}
                    else if(hit(248,680,200,48))history_delete_confirm_=true;
                }
            }
        }
        DrawHomeScreenLocked();FlushLocked();
    }
    auto& history=chat::History::Instance();auto& client=xiaozhi::Client::GetInstance();bool ok=true;
    if(action==Action::List)ok=history.List();
    else if(action==Action::Open)ok=history.Open(id);
    else if(action==Action::Move)ok=history.Move(direction);
    else if(action==Action::New||action==Action::Resume)ok=client.SwitchChat(id);
    else if(action==Action::Delete)ok=client.DeleteChat(id);
    else if(action==Action::Text)ok=client.SubmitText(text);
    if(action!=Action::None){
        DisplayLockGuard lock(this);
        if(ok){
            if(action==Action::Open)product_page_=ProductPage::ChatDetail;
            else if(action==Action::New||action==Action::Resume||action==Action::Text){product_page_=ProductPage::AiResult;ai_text_page_=0;if(action==Action::Text)chat_draft_.clear();}
            else if(action==Action::Delete)product_page_=ProductPage::ChatList;
        }else if(action==Action::Text){OpenEditorLocked(EditTarget::ChatMessage);form_message_="发送未完成，文字保留；检查连接或稍后重试";}
        else{std::snprintf(notification_text_,sizeof(notification_text_),"操作未启动，请先结束当前对话并完成历史保存");notification_deadline_ms_=esp_timer_get_time()/1000+4000;}
        DrawHomeScreenLocked();FlushLocked();
    }
    return true;
}
bool RawDisplay::HandleHistoryKey(HardwareKey key){
    int x=-1,y=-1;
    {
        DisplayLockGuard lock(this);
        if(test_console_mode_||screen_test_mode_||power_save_||reminder_alert_.active||quick_controls_open_.load()||discard_pending_)return false;
        if(product_page_==ProductPage::TextEntry&&edit_target_==EditTarget::ChatMessage&&key==HardwareKey::Select&&editor_.Ready()){x=260;y=704;}
        else if(product_page_!=ProductPage::ChatList&&product_page_!=ProductPage::ChatDetail)return false;
        else if(history_delete_confirm_){history_delete_confirm_=false;DrawHomeScreenLocked();FlushLocked();return true;}
        else if(key==HardwareKey::Home||key==HardwareKey::Back){product_page_=key==HardwareKey::Home?ProductPage::Home:ProductPage::AiResult;DrawHomeScreenLocked();FlushLocked();return true;}
        else if(product_page_==ProductPage::ChatList){
            if(key==HardwareKey::Previous){x=190;y=640;}else if(key==HardwareKey::Next){x=330;y=640;}else if(key==HardwareKey::Select){x=80;y=220;}
        }else{
            if(key==HardwareKey::Previous){x=history_text_page_?50:260;y=624;}
            else if(key==HardwareKey::Next){x=history_text_page_+1<history_text_pages_?150:370;y=624;}
            else if(key==HardwareKey::Select){x=80;y=700;}
        }
    }
    if(x>=0)return HandleHistoryTap(x,y);
    return true;
}
