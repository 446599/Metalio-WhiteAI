#!/usr/bin/env python3
"""Compile real weather parser, mailbox and cache with sanitizers; no live API."""
import os
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='whiteai-weather-') as temporary:
    p=Path(temporary);cjson=ROOT/'managed_components/espressif__cjson/cJSON'
    subprocess.run(['cc','-c',str(cjson/'cJSON.c'),'-I',str(cjson),'-o',str(p/'json.o')],check=True)
    files=['tools/tests/weather_contract.cc','main/dashboard/weather_provider.cc','main/dashboard/dashboard_data.cc','main/input/text_input.cc']
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread','-I',str(ROOT/'main'),'-I',str(cjson),*[str(ROOT/f) for f in files],str(p/'json.o'),'-o',str(p/'weather')],check=True)
    subprocess.run([str(p/'weather')],check=True)

    files=['tools/tests/weather_service_contract.cc','main/dashboard/dashboard_service.cc','main/xiaozhi/conversation.cc','main/dashboard/weather_provider.cc','main/dashboard/dashboard_data.cc','main/input/text_input.cc']
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-variable','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread','-I',str(ROOT/'tools/tests/weather_stubs'),'-I',str(ROOT/'main'),'-I',str(cjson),*[str(ROOT/f) for f in files],str(p/'json.o'),'-o',str(p/'service')],check=True)
    subprocess.run([str(p/'service')],check=True)
