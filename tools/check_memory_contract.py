#!/usr/bin/env python3
"""Compile and exercise real memory/notes/conversation/MCP code on the host."""
from pathlib import Path
import os
import subprocess
import tempfile
from host_cjson import cjson_flags

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ["notes/note_store.cc", "reminders/reminder_store.cc",
           "xiaozhi/conversation.cc", "xiaozhi/memory_tools.cc",
           "xiaozhi/system_tools.cc", "xiaozhi/mcp_server.cc"]


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="whiteai-memory-") as directory:
        work = Path(directory)
        flags = ["-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                 "-Wno-missing-field-initializers", "-pthread"]
        sanitizers = os.environ.get("HOST_SANITIZERS", "address,undefined")
        if sanitizers:
            flags += [f"-fsanitize={sanitizers}", "-fno-omit-frame-pointer"]
        command = [os.environ.get("CXX", "c++"), *flags, "-I", str(ROOT / "main"),
                   str(ROOT / "tools/tests/memory_contract.cc"),
                   *(str(ROOT / "main" / source) for source in SOURCES),
                   *cjson_flags(work), "-o", str(work / "test")]
        subprocess.run(command, check=True)
        subprocess.run([str(work / "test")], check=True)


if __name__ == "__main__":
    main()
