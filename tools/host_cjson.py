"""Locate real cJSON for host tests without requiring a firmware build.

Default: compile the project's managed cJSON source. For an installed host
library set CJSON_INCLUDE_DIR (containing cJSON.h) and CJSON_LIBRARY (full
library path) together. No parser mocks and no implicit network downloads.
"""
from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def cjson_flags(work: Path) -> list[str]:
    include, library = os.environ.get("CJSON_INCLUDE_DIR"), os.environ.get("CJSON_LIBRARY")
    if include or library:
        if not include or not library:
            raise SystemExit("Set both CJSON_INCLUDE_DIR and CJSON_LIBRARY.")
        header, binary = Path(include) / "cJSON.h", Path(library)
        if not header.is_file() or not binary.is_file():
            raise SystemExit("The configured cJSON header/library does not exist.")
        return ["-I", str(header.parent), str(binary)]
    source = ROOT / "managed_components/espressif__cjson/cJSON"
    if not (source / "cJSON.c").is_file() or not (source / "cJSON.h").is_file():
        raise SystemExit("Resolve ESP-IDF components first, or set CJSON_INCLUDE_DIR and CJSON_LIBRARY.")
    obj = work / "cjson.o"
    subprocess.run([os.environ.get("CC", "cc"), "-O1", "-c", str(source / "cJSON.c"),
                    "-I", str(source), "-o", str(obj)], check=True)
    return ["-I", str(source), str(obj)]
