#pragma once
#include "text_input.h"
#include <string>
#include <vector>
namespace input {
struct Key {int x,y,w,h;std::string label;char value=0;};
inline std::vector<Key> LetterKeys(Mode mode,bool symbols_second=false) {
    const char* rows[3]={"qwertyuiop","asdfghjkl","zxcvbnm"};
    if(mode==Mode::Symbols) {
        if(symbols_second) {rows[0]="`~\\|;:'\",.";rows[1]="/?";rows[2]="";}
        else {rows[0]="1234567890";rows[1]="!@#$%^&*()";rows[2]="-_=+[]{}<>";}
    }
    std::vector<Key> keys;keys.reserve(30);
    for(int row=0;row<3;++row) {
        const std::string letters=rows[row];const int left=32+(10-static_cast<int>(letters.size()))*21;
        for(size_t col=0;col<letters.size();++col) {
            const char value=letters[col];const char label=mode==Mode::Upper && value>='a' && value<='z' ? char(value-'a'+'A') : value;
            keys.push_back({left+static_cast<int>(col)*42,400+row*64,38,56,std::string(1,label),value});
        }
    }
    return keys;
}
inline bool Inside(int x,int y,int l,int t,int w,int h) {return x>=l && x<l+w && y>=t && y<t+h;}
}
