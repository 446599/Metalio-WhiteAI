from pathlib import Path

p=Path('main/display/history_view.cc')
s=p.read_text()
assert s.count('#include "raw_display.h"')==1
p.write_text(s.replace('#include "raw_display.h"','#include "raw_display.h"\n#include "input/keyboard_layout.h"',1))

p=Path('main/chat/history_store.cc')
s=p.read_text()
old='bool Dir(const std::string& path){struct stat st{};return lstat(path.c_str(),&st)==0 && S_ISDIR(st.st_mode);}'
new='''bool Dir(const std::string& path) {
    struct stat st{};
#if defined(ESP_PLATFORM)
    // The product stores history on FatFs, which has no symbolic links.
    // IDF's target libc does not expose lstat; use its supported VFS stat API.
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#else
    // Host tests may use a filesystem with links. Do not follow a substituted
    // history root/session directory when creating or saving local records.
    return ::lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}'''
assert s.count(old)==1
p.write_text(s.replace(old,new,1))

p=Path('tools/check_chat_history.py')
s=p.read_text()
s+='''\n    # Compile the device filesystem branch as well as exercising the host one.
    # This catches a host-only API reappearing without claiming to replace IDF.
    subprocess.run([os.getenv('CXX','c++'),'-std=c++17','-Wall','-Wextra','-Werror',
        '-DESP_PLATFORM=1','-Dlstat=WHITEAI_HOST_ONLY_LSTAT_IS_FORBIDDEN',
        '-I',str(ROOT/'main'),'-I',str(c),'-c',str(ROOT/'main/chat/history_store.cc'),
        '-o',str(p/'store-device-api.o')],check=True)
'''
p.write_text(s)

p=Path('tools/tests/chat_history_contract.cc')
s=p.read_text()
old='    // Fill a separate store to its documented cap without auto-eviction.'
new='''    // A host filesystem can contain links; unlike FatFs it must reject a
    // substituted root before creating a session outside the requested path.
    fs::create_directory_symlink(base/"disk",base/"linked-root");
    chat::Store linked((base/"linked-root").string());uint32_t linked_id=0;
    CHECK(!linked.Create(linked_id,error));CHECK(linked_id==0);
    // Fill a separate store to its documented cap without auto-eviction.'''
assert s.count(old)==1
p.write_text(s.replace(old,new,1))
