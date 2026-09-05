#pragma once

#include <algorithm>
#include <climits>
#include <string>
#include "cJSON.h"

namespace openrouter_protocol {
inline constexpr const char* kBoundary = "folloup-audio-7c43829e";
inline constexpr const char* kAudioContentType =
    "multipart/form-data; boundary=folloup-audio-7c43829e";

inline std::string AudioPrefix(const std::string& model)
{
    return std::string("--") + kBoundary +
        "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" + model +
        "\r\n--" + kBoundary +
        "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"recording.wav\""
        "\r\nContent-Type: audio/wav\r\n\r\n";
}

inline std::string AudioSuffix() { return std::string("\r\n--") + kBoundary + "--\r\n"; }

// HTTP writes may accept only part of a PCM chunk.
template <typename Writer>
bool WriteAll(const char* data, size_t size, Writer write)
{
    while (size != 0) {
        const int count = static_cast<int>(std::min(size, static_cast<size_t>(INT_MAX)));
        const int written = write(data, count);
        if (written <= 0 || written > count) return false;
        data += written;
        size -= static_cast<size_t>(written);
    }
    return true;
}

inline std::string TextRequest(const std::string& model, const std::string& prompt)
{
    cJSON* root = cJSON_CreateObject();
    cJSON* messages = cJSON_AddArrayToObject(root, "messages");
    cJSON* message = cJSON_CreateObject();
    if (!root || !messages || !message) {
        cJSON_Delete(message);
        cJSON_Delete(root);
        return {};
    }
    cJSON_AddItemToArray(messages, message);
    const bool ok = cJSON_AddStringToObject(root, "model", model.c_str()) &&
        cJSON_AddStringToObject(message, "role", "user") &&
        cJSON_AddStringToObject(message, "content", prompt.c_str()) &&
        cJSON_AddNumberToObject(root, "temperature", 0) &&
        cJSON_AddNumberToObject(root, "max_tokens", 2048) &&
        cJSON_AddBoolToObject(root, "stream", false);
    char* raw = ok ? cJSON_PrintUnformatted(root) : nullptr;
    std::string body = raw ? raw : "";
    cJSON_free(raw);
    cJSON_Delete(root);
    return body;
}

inline std::string CompletionText(cJSON* root)
{
    cJSON* choice = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "choices"), 0);
    cJSON* finish = cJSON_GetObjectItemCaseSensitive(choice, "finish_reason");
    // A truncated or filtered summary must not be saved as a complete result.
    if (!cJSON_IsString(finish) || std::string(finish->valuestring) != "stop") return {};
    cJSON* message = cJSON_GetObjectItemCaseSensitive(choice, "message");
    cJSON* text = cJSON_GetObjectItemCaseSensitive(message, "content");
    return cJSON_IsString(text) ? text->valuestring : "";
}
}  // namespace openrouter_protocol
