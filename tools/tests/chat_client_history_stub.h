#pragma once
#include "chat/history_service.h"
namespace chat {
inline std::vector<std::string> captured_statuses;
inline History& History::Instance(){static History h;return h;}
inline bool History::Capture(const xiaozhi::ConversationSnapshot& snapshot,const std::string&,const char* status){if(!snapshot.transcript.empty())captured_statuses.push_back(status);return true;}
inline bool History::CanCapture()const{return true;}
inline void History::CancelResume(){}
inline bool History::Switch(uint32_t){return true;}
inline bool History::Delete(uint32_t){return true;}
inline View History::Snapshot()const{return {};}
}
