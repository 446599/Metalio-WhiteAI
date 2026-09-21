#include "raw_display.h"
#include "font/raw_font.h"
#include "font/text_layout.h"
#include "board.h"
#include "notes/note_service.h"
#include "reminders/reminder_service.h"
#include "xiaozhi/xiaozhi_audio.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

RawDisplay::DeviceSnapshot RawDisplay::SystemSnapshot() {
    DisplayLockGuard lock(this);
    const char* name="home";
    switch (product_page_) {
        case ProductPage::Alarm: name="alarm";break;
        case ProductPage::TodayList: name="calendar";break;
        case ProductPage::Recorder: name="recorder";break;
        case ProductPage::AiResult: case ProductPage::AiSteps: name=voice_note_mode_ ? "voice_note" : "assistant";break;
        case ProductPage::NoteCompose: case ProductPage::Notes: case ProductPage::NoteDetail: name="notes";break;
        case ProductPage::QuickNote: name="capsules";break;
        case ProductPage::Reader: name="reader";break;
        case ProductPage::Apps: name="apps";break;
        case ProductPage::More: name="device";break;
        case ProductPage::WifiList: case ProductPage::WifiCredentials: name="wifi";break;
        case ProductPage::TextEntry: name="text_input";break;
        case ProductPage::Workbench: name="status";break;
        case ProductPage::Settings: name="info";break;
        default: break;
    }
    if (test_console_mode_ || screen_test_mode_) name="diagnostics";
    return {name,battery_percent_,charging_,power_save_};
}

bool RawDisplay::OpenSystemApp(const std::string& app,const std::string& date) {
    struct Entry {const char* name;ProductPage page;};
    static constexpr Entry entries[]={
        {"home",ProductPage::Home},{"alarm",ProductPage::Alarm},{"calendar",ProductPage::TodayList},
        {"recorder",ProductPage::Recorder},{"assistant",ProductPage::AiResult},{"voice_note",ProductPage::AiResult},
        {"notes",ProductPage::Notes},{"capsules",ProductPage::QuickNote},{"reader",ProductPage::Reader},
        {"apps",ProductPage::Apps},{"device",ProductPage::More},{"status",ProductPage::Workbench}};
    const auto entry=std::find_if(std::begin(entries),std::end(entries),[&](const auto& item){return app==item.name;});
    if (entry==std::end(entries) || animation_running_ || reminders::Service::Instance().IsActive()) return false;
    int offset=0,day=0;
    if (!date.empty()) {
        int64_t epoch;const auto now=time(nullptr);
        if (app!="calendar" || !reminders::ValidClock(now) || !reminders::ParseLocalTime(date+" 12:00:00",epoch)) return false;
        const time_t target=epoch;struct tm chosen{},current{};localtime_r(&target,&chosen);localtime_r(&now,&current);
        offset=(chosen.tm_year-current.tm_year)*12+chosen.tm_mon-current.tm_mon;day=chosen.tm_mday;
        if (offset < -120 || offset > 120) return false;
    }
    bool wake=false;
    {
        DisplayLockGuard lock(this);
        if (!portrait_fb_ || reminder_alert_.active || form_active_.load() || discard_pending_) return false;
        if (product_page_==ProductPage::Recorder && entry->page!=product_page_) xiaozhi::AudioSession::GetInstance().StopRecorder();
        screen_test_mode_=false;test_console_mode_=false;wake=power_save_;power_save_=false;
        product_page_=entry->page;app_parent_=ProductPage::Apps;navigation_index_=0;
        if (app=="calendar") {calendar_month_=offset;calendar_day_=day;calendar_events_page_=0;}
        if (app=="notes") notes_page_=0;
        if (app=="recorder") xiaozhi::AudioSession::GetInstance().RestoreRecorder();
        if (entry->page==ProductPage::AiResult) {voice_note_mode_=app=="voice_note";ai_show_transcript_=voice_note_mode_;ai_text_page_=0;}
        DrawHomeScreenLocked();FlushLocked();
    }
    if (wake) Board::GetInstance().SetPowerSaveMode(false);
    return true;
}

void RawDisplay::DrawProductNotesLocked(bool detail) {
    std::memset(portrait_fb_,0xff,portrait_size_);
    DrawProductStatusBarLocked();
    const auto items=detail ? notes::DeviceStore().List() : notes::DeviceStore().Search(notes_query_,"");
    if (detail) {
        const auto it=std::find_if(items.begin(),items.end(),[this](const auto& item){return item.id==note_id_;});
        DrawProductHeadingLocked("笔记详情","");
        DrawProductLabelLocked(32,120,416,it==items.end() ? "笔记已删除" : it->title.c_str(),ui_font_small);
        if(it!=items.end()) {StrokeRoundRect(344,72,104,48,12,1);DrawTextCentered(344,72,104,48,"编辑",ui_font_small);}
        const std::string body=it==items.end() ? "返回笔记目录查看其他内容。" : notes::DisplayBody(*it);
        const auto text_page=raw_font::Paginate(body,416,
            static_cast<size_t>(std::max(0,note_text_page_)),12,
            [](uint32_t cp){return raw_font::Lookup(ui_font_body,cp).advance;});
        note_text_pages_=static_cast<int>(text_page.pages);
        note_text_page_=static_cast<int>(text_page.page);
        for (size_t i=0;i<text_page.lines.size();++i)
            DrawText(32,152+static_cast<int>(i)*40,text_page.lines[i].c_str(),ui_font_body);
        char page[32];std::snprintf(page,sizeof(page),"%d / %d",note_text_page_+1,note_text_pages_);
        DrawTextCentered(32,640,416,32,page,ui_font_small);
        for (int i=0;i<2;++i) {
            StrokeRoundRect((i ? 248 : 32),672,200,48,12,1);
            DrawTextCentered((i ? 248 : 32),672,200,48,i ? "下一页" : "上一页",ui_font_small);
        }
        DrawProductControlRailLocked("返回目录 / 语音整理，保留原文");
        return;
    }
    notes_pages_=std::max(1,(static_cast<int>(items.size())+5)/6);
    notes_page_=std::clamp(notes_page_,0,notes_pages_-1);
    std::fill(std::begin(note_ids_),std::end(note_ids_),0);
    char count[24];std::snprintf(count,sizeof(count),"%u / 8",static_cast<unsigned>(items.size()));
    DrawProductHeadingLocked(notes_query_.empty() ? "AI 笔记" : "搜索结果",count);
    for (int row=0;row<6 && notes_page_*6+row<static_cast<int>(items.size());++row) {
        const auto& note=items[notes_page_*6+row];note_ids_[row]=note.id;
        std::string stamp=note.updated ? reminders::LocalTime(note.updated).substr(5,11) : "时间未同步";
        if (!note.project.empty()) stamp=note.project+" / "+stamp;
        if (note.done) stamp="已处理 / "+stamp;
        DrawProductIconRowLocked(144+row*80,lucide::Id::NotebookPen,note.title.c_str(),stamp.c_str(),navigation_index_==row);
    }
    if (items.empty()) {
        DrawProductIconLocked(lucide::Id::NotebookPen,220,216,40,true);
        DrawTextCentered(32,300,416,48,notes::DeviceStore().Ready() ? (notes_query_.empty() ? "点击新建，或让小智记录" : "没有匹配的笔记") : "请检查 SD 卡",ui_font_body);
        DrawTextCentered(32,364,416,40,notes_query_.empty() ? "支持离线拼音输入" : "搜索中清空文字可显示全部",ui_font_small);
    }
    const char* controls[]={"新建", "搜索", "下一页"};
    const int xs[]={32,174,316};
    for(int i=0;i<3;++i) {
        StrokeRoundRect(xs[i],672,132,48,12,1);
        DrawTextCentered(xs[i],672,132,48,controls[i],ui_font_small);
    }
    DrawProductControlRailLocked("本地输入 / 按住 AI 键语音记录");
}
