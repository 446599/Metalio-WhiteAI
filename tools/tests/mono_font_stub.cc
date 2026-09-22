#include "display/font/ai_ui_assets.h"
#include "display/font/font_loader.h"
// Deliberately tiny synthetic fixture, not a font distributed with the device.
static const uint8_t bitmap[]={0x1b,0xe4}; // 0,1,2,3 / 3,2,1,0 coverage
static const uint16_t cp[]={65};
static const ui_glyph_t glyph[]={{0,4}};
const ui_font_t ui_font_body={bitmap,glyph,cp,1,2,2,2};
const ui_font_t ui_font_small=ui_font_body,ui_font_status=ui_font_body,ui_font_title=ui_font_body,ui_font_h1=ui_font_body,ui_font_clock=ui_font_body;
extern "C" {
bool font_loader_is_ready(){return true;}
int font_loader_init_flash(const char*){return FONT_LOADER_OK;}
int font_loader_init_memory(const uint8_t*,size_t){return FONT_LOADER_OK;}
uint32_t font_loader_item_count(){return 1;}
bool get_glyph(uint32_t cp,uint16_t,uint16_t,GlyphInfo* info){
 static const uint8_t full[]={0xf0,0x90,0x90,0xf0};
 if(cp!=65 && cp!=0x4e00)return false;
 *info={4,4,0,0,4,4,full};return true;
}
}
