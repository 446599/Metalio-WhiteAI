#!/usr/bin/env python3
"""Exercise real history storage, then guard the device-only filesystem API."""
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory(prefix='whiteai-chat-') as folder:
    p = pathlib.Path(folder)
    cjson = ROOT / 'managed_components/espressif__cjson/cJSON'
    compiler = os.environ.get('CXX', 'c++')
    subprocess.run(['cc', '-c', str(cjson / 'cJSON.c'), '-I', str(cjson),
                    '-o', str(p / 'json.o')], check=True)
    files = ['tools/tests/chat_history_contract.cc', 'main/chat/history_store.cc',
             'main/chat/history_service.cc', 'main/notes/note_store.cc']
    subprocess.run([compiler, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-pthread',
                    '-I', str(ROOT / 'tools/tests/mono_stubs'), '-I', str(ROOT / 'main'),
                    '-I', str(cjson), '-DWHITEAI_CHAT_PATH="' + str(p / 'service') + '"',
                    *[str(ROOT / f) for f in files], str(p / 'json.o'),
                    '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test'), str(p / 'disk-tests')], check=True)

    # Include the host declaration BEFORE poisoning the unavailable API. Merely
    # using -Dlstat=another_name renames the declaration too and misses the bug.
    guard = p / 'fatfs-api-guard.h'
    guard.write_text('#include <sys/stat.h>\n#pragma GCC poison lstat\n')
    flags = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-DESP_PLATFORM=1',
             '-include', str(guard), '-I', str(ROOT / 'main'),
             '-I', str(ROOT / 'main/chat'), '-I', str(cjson)]
    source = ROOT / 'main/chat/history_store.cc'
    subprocess.run([*flags, '-c', str(source), '-o', str(p / 'device-api.o')], check=True)
    # Verify that this guard actually rejects the regression, not just that the
    # selected branch compiles on a POSIX host. Full IDF build remains required.
    text = source.read_text()
    assert text.count('return ::stat(') == 1
    mutant = p / 'forbidden-history-store.cc'
    mutant.write_text(text.replace('return ::stat(', 'return ::lstat(', 1))
    rejected = subprocess.run([*flags, '-c', str(mutant), '-o', str(p / 'bad.o')],
                              capture_output=True, text=True)
    assert rejected.returncode != 0 and 'poisoned' in rejected.stderr and 'lstat' in rejected.stderr, rejected.stderr
    print('Device filesystem API guard PASS: supported branch compiles; injected lstat regression rejected')
