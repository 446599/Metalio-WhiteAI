#include "reader_service.h"
#include "book_text.h"
#include "application.h"
#include "display/font/raw_font.h"
#include "display/font/text_layout.h"
#include "hal/hal.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <new>
#include <sys/stat.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace reader {
namespace {
#ifndef WHITEAI_BOOKS_PATH
#define WHITEAI_BOOKS_PATH "/sdcard/books/"
#endif
constexpr const char* root=WHITEAI_BOOKS_PATH;
struct Bookmark {uint32_t magic=0x57425232,hash=0,page=0;char name[193]{};};
bool LoadBookmark(Bookmark& out) {
    nvs_handle_t nvs=0;if(nvs_open("reader",NVS_READONLY,&nvs)!=ESP_OK)return false;
    size_t bytes=sizeof(out);const auto result=nvs_get_blob(nvs,"position",&out,&bytes);nvs_close(nvs);
    return result==ESP_OK && bytes==sizeof(out) && out.magic==0x57425232 &&
        std::memchr(out.name,0,sizeof(out.name)) && FileName(out.name);
}
}
struct Service::Job {Service* self;Command command;std::string filename;int direction;};
Service& Service::Instance(){static Service service;return service;}
Snapshot Service::Get()const{std::lock_guard<std::mutex> lock(mutex_);return state_;}
uint32_t Service::Revision()const{std::lock_guard<std::mutex> lock(mutex_);return state_.revision;}
bool Service::List(){return Start(Command::List,"",0);}
bool Service::Open(const std::string& name){return FileName(name) && Start(Command::Open,name,0);}
bool Service::Turn(int direction){return Start(Command::Turn,"",direction<0?-1:1);}
bool Service::Start(Command command,const std::string& name,int direction){
    std::lock_guard<std::mutex> lock(mutex_);
    if(state_.busy || (command==Command::Turn && !state_.opened)) return false;
    std::unique_ptr<Job> job(new(std::nothrow) Job{this,command,name,direction});if(!job)return false;
    state_.busy=true;state_.message="正在读取";++state_.revision;
    if(xTaskCreate(Worker,"book_reader",8192,job.get(),1,nullptr)!=pdPASS){state_.busy=false;state_.message="阅读任务未启动，内存不足";++state_.revision;return false;}
    job.release();return true;
}
void Service::ReadList(){
    std::vector<std::string> names;std::string message;
    if(!GetHAL().IsSdMounted()) message="请插入 SD 卡，将 TXT 放入 books 目录";
    else {
        DIR* directory=opendir(root);
        if(!directory) message="请在 SD 卡创建 books 目录并放入 UTF-8 TXT";
        else {
            while(auto* entry=readdir(directory)){
                const std::string name=entry->d_name;if(!FileName(name))continue;
                struct stat st{};const auto path=std::string(root)+name;
                if(stat(path.c_str(),&st)!=0 || !S_ISREG(st.st_mode))continue;
                names.push_back(name);if(names.size()==kBooks)break;
            }
            closedir(directory);std::sort(names.begin(),names.end());
            message=names.empty()?"books 目录中还没有 TXT 文件":"UTF-8 TXT / 单本最大 256 KiB";
        }
    }
    text_.reset();filename_.clear();fingerprint_=0;
    std::lock_guard<std::mutex> lock(mutex_);
    state_.opened=false;state_.files=std::move(names);state_.lines.clear();state_.page=0;state_.pages=1;state_.message=message;
}
void Service::ReadBook(const std::string& name){
    auto fail=[&](const char* error){std::lock_guard<std::mutex> lock(mutex_);state_.message=error;};
    if(!GetHAL().IsSdMounted()){fail("SD 卡不可用");return;}
    const auto path=std::string(root)+name;struct stat st{};
    if(!FileName(name) || stat(path.c_str(),&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<0 || st.st_size>static_cast<off_t>(kBookBytes)){fail("无法读取文件，或文件超过 256 KiB");return;}
    FILE* file=std::fopen(path.c_str(),"rb");if(!file){fail("打开文件失败");return;}
    std::string content(static_cast<size_t>(st.st_size),'\0');
    const bool read=std::fread(content.data(),1,content.size(),file)==content.size() && std::fgetc(file)==EOF && !std::ferror(file);
    std::fclose(file);
    if(!read || !Normalize(content)){fail("读取失败或不是有效 UTF-8 TXT，请先转换编码");return;}
    fingerprint_=Fingerprint(content);filename_=name;text_=std::make_shared<const std::string>(std::move(content));
    Bookmark bookmark;size_t page=0;
    if(LoadBookmark(bookmark) && name==bookmark.name && fingerprint_==bookmark.hash)page=bookmark.page;
    {std::lock_guard<std::mutex> lock(mutex_);state_.opened=true;state_.title=name;}
    Paginate(page);
}
void Service::Paginate(size_t page){
    if(!text_) return;
    const auto result=raw_font::Paginate(*text_,416,page,12,[](uint32_t cp){return raw_font::Lookup(ui_font_body,cp).advance;});
    {std::lock_guard<std::mutex> lock(mutex_);state_.page=result.page;state_.pages=result.pages;state_.lines=result.lines;}
    const bool saved=SaveBookmark();
    std::lock_guard<std::mutex> lock(mutex_);state_.message=saved?"阅读位置已保存":"书签保存失败，本次仍可阅读";
}
bool Service::SaveBookmark(){
    Bookmark bookmark;std::memset(static_cast<void*>(&bookmark),0,sizeof(bookmark));bookmark.magic=0x57425232;bookmark.hash=fingerprint_;
    {std::lock_guard<std::mutex> lock(mutex_);bookmark.page=state_.page;}
    std::snprintf(bookmark.name,sizeof(bookmark.name),"%s",filename_.c_str());
    nvs_handle_t nvs=0;auto error=nvs_open("reader",NVS_READWRITE,&nvs);
    if(error==ESP_OK){error=nvs_set_blob(nvs,"position",&bookmark,sizeof(bookmark));if(error==ESP_OK)error=nvs_commit(nvs);nvs_close(nvs);}
    return error==ESP_OK;
}
void Service::Worker(void* arg){
    {
        std::unique_ptr<Job> job(static_cast<Job*>(arg));auto& service=*job->self;
        if(job->command==Command::List)service.ReadList();
        else if(job->command==Command::Open)service.ReadBook(job->filename);
        else {const auto before=service.Get();service.Paginate(job->direction<0 ? (before.page ? before.page-1 : 0) : before.page+1);}
        std::lock_guard<std::mutex> lock(service.mutex_);service.state_.busy=false;++service.state_.revision;
    }
    Application::GetInstance().RequestStatusUpdate(true);vTaskDelete(nullptr);
}
} // namespace reader
