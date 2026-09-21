#include "conversation.h"

#include <cJSON.h>
#include <nvs.h>
#include <vector>
#include <cstdlib>
#include <cstring>

namespace xiaozhi {
// A single bounded NVS blob is committed atomically. This keeps existing device
// settings intact and never claims success on NVS full or a failed commit.
bool SaveLastCapsule(const ConversationSnapshot& snapshot) {
    if (snapshot.transcript.empty() || snapshot.transcript.size() > Conversation::kTranscriptBytes ||
        snapshot.answer.size() > Conversation::kAnswerBytes) return false;
    cJSON* root = cJSON_CreateObject();
    if (!root) return false;
    if (!cJSON_AddNumberToObject(root, "schema", 1) ||
        !cJSON_AddStringToObject(root, "text", snapshot.transcript.c_str()) ||
        !cJSON_AddStringToObject(root, "answer", snapshot.answer.c_str())) {
        cJSON_Delete(root); return false;
    }
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return false;
    if (std::strlen(json) + 1 > 16384) { cJSON_free(json); return false; }
    nvs_handle_t handle;
    bool ok = false;
    if (nvs_open("capsule", NVS_READWRITE, &handle) == ESP_OK) {
        ok = nvs_set_blob(handle, "last", json, std::strlen(json) + 1) == ESP_OK && nvs_commit(handle) == ESP_OK;
        nvs_close(handle);
    }
    cJSON_free(json);
    return ok;
}
bool LoadLastCapsule() {
    nvs_handle_t handle;
    if (nvs_open("capsule", NVS_READONLY, &handle) != ESP_OK) return false;
    size_t length = 0;
    if (nvs_get_blob(handle, "last", nullptr, &length) != ESP_OK || length == 0 || length > 16384) {
        nvs_close(handle); return false;
    }
    std::vector<char> json(length);
    const bool read = nvs_get_blob(handle, "last", json.data(), &length) == ESP_OK;
    nvs_close(handle);
    if (!read || json.back() != '\0') return false;
    cJSON* root = cJSON_ParseWithLength(json.data(), length);
    if (!root) return false;
    const auto* text = cJSON_GetObjectItemCaseSensitive(root, "text");
    const auto* answer = cJSON_GetObjectItemCaseSensitive(root, "answer");
    const auto* schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    // Read the existing unversioned format without rewriting unrelated NVS.
    const bool valid = (!schema || (cJSON_IsNumber(schema) && schema->valuedouble == 1)) &&
                       cJSON_IsString(text) && text->valuestring[0] && cJSON_IsString(answer) &&
                       std::strlen(text->valuestring) <= Conversation::kTranscriptBytes &&
                       std::strlen(answer->valuestring) <= Conversation::kAnswerBytes;
    if (valid) Conversation::GetInstance().Restore(text->valuestring, answer->valuestring);
    cJSON_Delete(root);
    return valid;
}
}  // namespace xiaozhi
