#!/usr/bin/env python3
"""Run production font, reader, controls and archive code with host hardware adapters."""
from pathlib import Path
import os, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='whiteai-mono-') as folder:
    p=Path(folder); cjson=ROOT/'managed_components/espressif__cjson/cJSON'
    subprocess.run(['cc','-c',str(cjson/'cJSON.c'),'-I',str(cjson),'-o',str(p/'cjson.o')],check=True)
    files=['tools/tests/mono_controls_contract.cc','tools/tests/mono_font_stub.cc',
           'main/display/font/raw_font.cc','main/input/text_input.cc','main/notes/note_store.cc',
           'main/notes/note_writer.cc','main/reminders/reminder_store.cc',
           'main/xiaozhi/conversation.cc','main/reader/reader_service.cc','main/system/quick_controls.cc']
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-O1','-g','-Wall','-Wextra','-Werror',
       '-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread',
       '-I',str(ROOT/'tools/tests/mono_stubs'),'-I',str(ROOT/'main'),'-I',str(cjson),
       '-DWHITEAI_BOOKS_PATH="'+str(p/'books')+'/"',*[str(ROOT/f) for f in files],str(p/'cjson.o'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test'),str(p/'books')],check=True)
