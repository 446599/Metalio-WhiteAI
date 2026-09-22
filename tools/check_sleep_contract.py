#!/usr/bin/env python3
"""Run actual sleep coordinator with controlled hardware adapters, not a power measurement."""
from pathlib import Path
import os, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='whiteai-sleep-') as name:
    p=Path(name)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread',
        '-I',str(ROOT/'tools/tests/sleep_stubs'),'-I',str(ROOT/'main'),str(ROOT/'main/power/sleep_service.cc'),str(ROOT/'tools/tests/sleep_contract.cc'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test'),str(p)],check=True,env={**os.environ,"UBSAN_OPTIONS":"halt_on_error=1"})
