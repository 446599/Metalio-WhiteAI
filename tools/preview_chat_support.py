"""History UI fixtures: real rendering/routing, simulated storage/radio ownership."""
HEADERS=r'''
#include "chat/history_service.h"
#include "display/notes_layout.h"
namespace chat {
View preview_history;
History& History::Instance(){static History h;return h;}
View History::Snapshot()const{return preview_history;}
uint32_t History::Revision()const{return preview_history.revision;}
void History::Retry(){}
bool History::List(){assert(ui_lock_depth==0);preview_history.sessions={{1,3,1790035200,"磁铁卡框设计"},{2,1,0,"离线记录的想法"}};preview_history.message="选择历史会话，或新建对话";preview_history.busy=false;++preview_history.revision;return true;}
bool History::Open(uint32_t id){assert(ui_lock_depth==0);preview_history.selected=id;preview_history.turn_index=2;preview_history.turn_count=3;preview_history.turn=std::make_shared<const Turn>(Turn{3,1790035200,"下一步应该怎么做？","先做一个试装件，再记录公差。\n这段历史在本地 SD 卡保存，断网仍可以查看。","","complete",false});++preview_history.revision;return true;}
bool History::Move(int d){assert(ui_lock_depth==0);if(d<0&&preview_history.turn_index)--preview_history.turn_index;else if(d>0&&preview_history.turn_index+1<preview_history.turn_count)++preview_history.turn_index;++preview_history.revision;return true;}
bool History::Switch(uint32_t id){assert(ui_lock_depth==0);preview_history.active=id;++preview_history.revision;return true;}
bool History::Delete(uint32_t id){assert(ui_lock_depth==0);preview_history.sessions.erase(std::remove_if(preview_history.sessions.begin(),preview_history.sessions.end(),[id](const auto& s){return s.id==id;}),preview_history.sessions.end());++preview_history.revision;return true;}
}
namespace xiaozhi {
class Client {
public:
    bool submit_ok=true;
    std::string submitted;
    static Client& GetInstance(){static Client c;return c;}
    bool SubmitText(const std::string& s){assert(ui_lock_depth==0);submitted=s;return submit_ok;}
    bool SwitchChat(uint32_t id){assert(ui_lock_depth==0);return chat::History::Instance().Switch(id);}
    bool DeleteChat(uint32_t id){assert(ui_lock_depth==0);return chat::History::Instance().Delete(id);}
};
}
'''
FIELDS=r'''
    uint32_t last_history_revision_=0;
    int history_list_page_=0,history_text_page_=0,history_text_pages_=1;
    bool history_delete_confirm_=false;
    std::string chat_draft_;
'''
METHODS=['HandleHistoryTap','HandleHistoryKey']
EXERCISE=r'''
    display.ClearFormLocked();display.reminder_alert_.active=false;display.power_save_=false;
    display.product_page_=RawDisplay::ProductPage::AiResult;conversation.Restore("历史对话测试原文","这一轮已保存。");
    save("ai-history-toolbar");
    assert(display.HandleHistoryTap(300,90));assert(display.product_page_==RawDisplay::ProductPage::ChatList);save("chat-history-list");
    assert(display.HandleHistoryTap(120,220));assert(display.product_page_==RawDisplay::ProductPage::ChatDetail);save("chat-history-detail");
    assert(display.HandleHistoryTap(260,625));assert(chat::preview_history.turn_index==1);save("chat-history-previous-turn");
    assert(display.HandleHistoryTap(300,705));assert(display.history_delete_confirm_);save("chat-delete-confirm");
    assert(display.HandleHistoryTap(60,520));assert(!display.history_delete_confirm_);
    assert(display.HandleHistoryTap(100,705));assert(display.product_page_==RawDisplay::ProductPage::AiResult&&chat::preview_history.active==1);
    assert(display.HandleHistoryTap(395,90));assert(chat::preview_history.active==0);
    assert(display.HandleHistoryTap(220,90));assert(display.edit_target_==RawDisplay::EditTarget::ChatMessage);
    assert(display.editor_.Insert("你好，帮我整理今天的安排。"));save("chat-text-entry");
    xiaozhi::Client::GetInstance().submit_ok=false;
    assert(display.HandleHistoryTap(300,710));assert(display.product_page_==RawDisplay::ProductPage::TextEntry&&!display.editor_.Text().empty());save("chat-text-retry");
    xiaozhi::Client::GetInstance().submit_ok=true;
    assert(display.HandleHistoryKey(RawDisplay::HardwareKey::Select));assert(display.product_page_==RawDisplay::ProductPage::AiResult&&display.chat_draft_.empty());
    assert(!xiaozhi::Client::GetInstance().submitted.empty());
    display.ClearFormLocked();display.product_page_=RawDisplay::ProductPage::Notes;save("notes-spaced-list");
    const auto items=notes::DeviceStore().List();if(!items.empty()){display.note_id_=items[0].id;display.note_text_page_=0;display.product_page_=RawDisplay::ProductPage::NoteDetail;save("note-title-below-divider");}
    assert(notes_ui::kDetailTitleY>128);assert(notes_ui::kBodyY>=notes_ui::kDetailTitleY+ui_font_body.height);
    assert(notes_ui::kHeight>=ui_font_body.height+44); // title / summary / rule must not overlap.
    display.product_page_=RawDisplay::ProductPage::Home;save("home-notes-shortcut");
    std::puts("Chat UI PASS: actual history/text/notes routing; mocked I/O outside display lock");
'''
