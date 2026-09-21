#pragma once
#include "system_tools.h"

namespace xiaozhi {
size_t MemoryToolCount();
const char* MemoryToolSchema(size_t index);
bool MemoryToolKnows(const char* name);
// Called by the existing MCP worker; no display locks, network or audio work.
ToolReply HandleMemoryTool(const char* name,const cJSON* args,int64_t now,
                          notes::Store& notes,reminders::Store& reminders);
cJSON* MemoryNoteJson(const notes::Note& note,bool full,const reminders::Store& reminders);
}
