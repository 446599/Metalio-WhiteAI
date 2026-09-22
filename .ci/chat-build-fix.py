from pathlib import Path
p=Path('main/display/history_view.cc')
s=p.read_text()
assert s.count('#include "raw_display.h"')==1
p.write_text(s.replace('#include "raw_display.h"','#include "raw_display.h"\n#include "input/keyboard_layout.h"',1))
