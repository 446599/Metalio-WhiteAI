#pragma once
namespace notes_ui {
constexpr int kRows=5,kY=160,kPitch=92,kHeight=80;
constexpr int kDetailTitleY=148,kBodyY=212,kBodyRows=10,kBodyPitch=40;
inline int RowAt(int x,int y){if(x<32||x>=448||y<kY)return -1;int row=(y-kY)/kPitch;return row<kRows&&(y-kY)%kPitch<kHeight?row:-1;}
static_assert(kY+(kRows-1)*kPitch+kHeight<672);
static_assert(kBodyY+kBodyRows*kBodyPitch<=640);
}
