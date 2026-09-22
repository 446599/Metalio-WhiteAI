#!/usr/bin/env python3
"""Execute actual SD history and task-mailbox code with only hardware stubs."""
import os, pathlib, subprocess, tempfile
ROOT=pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='whiteai-chat-') as folder:
    p=pathlib.Path(folder); c=ROOT/'managed_components/espressif__cjson/cJSON'
    subprocess.run(['cc','-c',str(c/'cJSON.c'),'-I',str(c),'-o',str(p/'json.o')],check=True)
    files=['tools/tests/chat_history_contract.cc','main/chat/history_store.cc','main/chat/history_service.cc','main/notes/note_store.cc']
    subprocess.run([os.getenv('CXX','c++'),'-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread','-I',str(ROOT/'tools/tests/mono_stubs'),'-I',str(ROOT/'main'),'-I',str(c),'-DWHITEAI_CHAT_PATH="'+str(p/'service')+'"',*[str(ROOT/f) for f in files],str(p/'json.o'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test'),str(p/'disk-tests')],check=True)

    # Compile the device filesystem branch as well as exercising the host one.
    # This catches a host-only API reappearing without claiming to replace IDF.
    subprocess.run([os.getenv('CXX','c++'),'-std=c++17','-Wall','-Wextra','-Werror',
        '-DESP_PLATFORM=1','-Dlstat=WHITEAI_HOST_ONLY_LSTAT_IS_FORBIDDEN',
        '-I',str(ROOT/'main'),'-I',str(c),'-c',str(ROOT/'main/chat/history_store.cc'),
        '-o',str(p/'store-device-api.o')],check=True)
