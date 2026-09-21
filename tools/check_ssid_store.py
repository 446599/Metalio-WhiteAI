#!/usr/bin/env python3
"""Exercise the production credential store against transactional fake NVS."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
HEADER='''#pragma once
#include <cstddef>
using nvs_handle_t=int;
using esp_err_t=int;
constexpr int ESP_OK=0, ESP_ERR_NVS_NOT_FOUND=1, NVS_READONLY=0, NVS_READWRITE=1;
int nvs_open(const char*,int,int*);void nvs_close(int);
int nvs_get_blob(int,const char*,void*,size_t*);int nvs_get_str(int,const char*,char*,size_t*);
int nvs_set_blob(int,const char*,const void*,size_t);int nvs_commit(int);
'''
with tempfile.TemporaryDirectory(prefix='whiteai-ssid-') as directory:
 p=Path(directory);(p/'nvs.h').write_text(HEADER)
 subprocess.run(['c++','-std=c++17','-O1','-g','-Wall','-Wextra','-fsanitize=address,undefined','-I',str(p),'-I',str(ROOT/'components/esp-wifi-connect/include'),str(ROOT/'tools/tests/ssid_store_contract.cc'),str(ROOT/'components/esp-wifi-connect/ssid_manager.cc'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
