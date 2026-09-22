#pragma once
#include "note_store.h"
#include "xiaozhi/conversation.h"
#include <cstdio>
namespace notes {
inline bool PrepareConversationNote(const xiaozhi::ConversationSnapshot& source,Note& draft,std::string& message) {
    if(source.state!=xiaozhi::TurnState::Done || source.truncated || !source.content_revision ||
       !ValidText(source.transcript,Store::kRawBytes,false) || !ValidText(source.answer,xiaozhi::Conversation::kAnswerBytes)) {
        message="请先完成一轮对话；取消或截断的内容不能归档";return false;
    }
    // Same length-delimited fingerprint as the MCP archive. This is a stable
    // deduplication hint, not authentication; Store also compares raw_text.
    uint64_t hash=14695981039346656037ULL;
    const auto byte=[&](uint8_t v){hash=(hash^v)*1099511628211ULL;};
    for(const auto* text:{&source.transcript,&source.answer}){
        const uint64_t size=text->size();for(int i=0;i<8;++i)byte(static_cast<uint8_t>(size>>(8*i)));
        for(unsigned char c:*text)byte(c);
    }
    char id[17];std::snprintf(id,sizeof(id),"%016llx",static_cast<unsigned long long>(hash));
    draft={};draft.title="语音记忆";draft.raw_text=source.transcript;draft.source_id=id;draft.source_revision=source.content_revision;
    const bool long_answer=source.answer.size()>Store::kTextBytes;
    draft.text=source.answer.empty() || long_answer ? source.transcript : source.answer;
    message=long_answer ? "回答超过上限，草稿保留原文；请编辑整理版" : "核对后保存；原文独立保留";
    return true;
}
} // namespace notes
