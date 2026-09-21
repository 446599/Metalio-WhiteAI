#include <mutex>
#include <vector>
#include <string>
#define private public
#include "ssid_manager.h"
#undef private
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include "nvs.h"
std::map<std::string,std::string> disk,staged;
bool fail=false;
int commits=0;
int nvs_open(const char*,int,int* h){*h=1;staged=disk;return 0;}
void nvs_close(int){}
int nvs_get_blob(int,const char* key,void* data,size_t* n){auto it=disk.find(key);if(it==disk.end())return ESP_ERR_NVS_NOT_FOUND;if(*n<it->second.size())return 9;*n=it->second.size();memcpy(data,it->second.data(),*n);return 0;}
int nvs_get_str(int h,const char* k,char* data,size_t* n){return nvs_get_blob(h,k,data,n);}
int nvs_set_blob(int,const char* key,const void* d,size_t n){staged[key]=std::string(static_cast<const char*>(d),n);return 0;}
int nvs_commit(int){if(fail)return 8;disk=staged;++commits;return 0;}
int main(){
 disk["ssid"]=std::string("legacy\0",7);disk["password"]=std::string("12345678\0",9);
 SsidManager s;assert(s.GetSsidList().size()==1 && commits==0);
 fail=true;assert(!s.AddSsid("new","wrong-password"));assert(s.GetSsidList()[0].ssid=="legacy");assert(!disk.count("profiles_v2"));
 fail=false;assert(s.AddSsid(std::string(32,'s'),std::string(64,'a')));assert(disk.count("profiles_v2"));assert(commits==1);
 SsidManager reboot;assert(reboot.GetSsidList().size()==2 && reboot.GetSsidList()[0].password.size()==64);
 assert(s.AddSsid("new","12345678"));assert(s.AddSsid("new","updated888"));assert(s.GetSsidList().size()==3);
 auto old=s.GetSsidList();fail=true;s.RemoveSsid(0);assert(s.GetSsidList().size()==old.size());s.Clear();assert(s.GetSsidList().size()==old.size());fail=false;
 for(int i=0;i<20;++i)assert(s.AddSsid("net"+std::to_string(i),""));assert(s.GetSsidList().size()==10);
 assert(!s.AddSsid("", "12345678"));assert(!s.AddSsid(std::string(33,'x'),"12345678"));assert(!s.AddSsid("x",std::string(65,'a')));
 s.SetDefaultSsid(5);auto id=s.GetSsidList()[0].ssid;SsidManager sorted;assert(sorted.GetSsidList()[0].ssid==id);
 s.Clear();SsidManager empty;assert(empty.GetSsidList().empty());assert(disk.count("ssid")); // old keys not consulted after v2 migration
 disk["profiles_v2"]="broken";SsidManager corrupt;assert(corrupt.GetSsidList().empty());assert(!corrupt.AddSsid("x","12345678"));assert(disk["profiles_v2"]=="broken");
 std::puts("SSID store PASS: legacy migration, 32/64-byte boundaries, failed-commit rollback, reboot, replace, reorder, capacity and corrupt-blob protection");
}
