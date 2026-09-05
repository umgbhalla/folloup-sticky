// Run with scripts/test_openrouter.sh after sourcing ESP-IDF export.sh.
#include <cassert>
#include <cstring>
#include "openrouter_protocol.h"

int main()
{
    using namespace openrouter_protocol;
    auto body = TextRequest("test/model", "A \"quoted\" note\nnext line");
    cJSON* root = cJSON_Parse(body.c_str());
    assert(root);
    assert(std::strcmp(cJSON_GetObjectItem(root, "model")->valuestring, "test/model") == 0);
    auto message = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "messages"), 0);
    assert(std::strcmp(cJSON_GetObjectItem(message, "content")->valuestring,
                       "A \"quoted\" note\nnext line") == 0);
    cJSON_Delete(root);
    for (const char* finish : {"stop", "length", "content_filter"}) {
        body = std::string("{\"choices\":[{\"finish_reason\":\"") + finish +
            "\",\"message\":{\"content\":\"summary\"}}]}";
        root = cJSON_Parse(body.c_str());
        assert(CompletionText(root) == (std::string(finish) == "stop" ? "summary" : ""));
        cJSON_Delete(root);
    }
    assert(CompletionText(nullptr).empty());
    root = cJSON_Parse("{\"error\":{\"message\":\"invalid key\"}}");
    assert(CompletionText(root).empty());
    cJSON_Delete(root);
    std::string sent;
    assert(WriteAll("abcdef", 6, [&](const char* bytes, int count) {
        const int n = std::min(count, 2); sent.append(bytes, n); return n;
    }));
    assert(sent == "abcdef");
    assert(!WriteAll("a", 1, [](const char*, int) { return 0; }));
    assert(!WriteAll("a", 1, [](const char*, int) { return -1; }));
    const auto prefix = AudioPrefix("microsoft/mai-transcribe-2");
    assert(prefix.find("name=\"model\"\r\n\r\nmicrosoft/mai-transcribe-2\r\n") != std::string::npos);
    assert(prefix.find("filename=\"recording.wav\"") != std::string::npos);
    assert(AudioSuffix() == std::string("\r\n--") + kBoundary + "--\r\n");
}
