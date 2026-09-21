#!/usr/bin/env python3
"""Compile real text input and Wi-Fi setup state logic; no radio is accessed."""
from pathlib import Path
import os, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='whiteai-input-') as directory:
    out=Path(directory)/'test'
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-O1','-g','-Wall','-Wextra',
        '-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread','-I',str(ROOT/'main'),
        str(ROOT/'tools/tests/input_network_contract.cc'),str(ROOT/'main/input/text_input.cc'),
        str(ROOT/'main/network/setup_model.cc'),'-o',str(out)],check=True)
    subprocess.run([str(out)],check=True)
