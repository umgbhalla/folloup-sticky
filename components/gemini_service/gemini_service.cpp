#include "gemini_service.h"
#include "openrouter_protocol.h"
#include <climits>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <strings.h>
#include <utility>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "followup_task_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "recording_service.h"
#include "sdkconfig.h"

namespace gemini_service {
namespace {

constexpr const char* kTag = "OpenRouterService";
constexpr const char* kSettingsTag = "OpenRouterSettings";
constexpr const char* kStorageNamespace = "openrouter";
constexpr const char* kStorageApiKey = "api_key";
constexpr const char* kDefaultModelName = CONFIG_FOLLOWUP_OPENROUTER_TEXT_MODEL;
constexpr const char* kOpenRouterApiBaseUrl = "https://openrouter.ai/api/v1/";
constexpr const char* kPortalApiSettingsGeminiUri = "/api/settings/gemini";
constexpr const char* kPortalApiSettingsGeminiResetUri = "/api/settings/gemini/reset";
constexpr const char* kPortalApiRuntimeGeminiUri = "/api/runtime/gemini";
constexpr size_t kMaxPortalPayloadLen = 512;
constexpr int kAuthTimeoutMs = 15000;
constexpr int kGenerateTimeoutMs = 60000;  // text generation can be slow for large prompts
constexpr uint32_t kAuthTaskStackWords = 8192;

// Stream WAV directly as multipart data; no second audio buffer in PSRAM.
constexpr int kTranscribeTimeoutMs = 60000;
constexpr size_t kHttpUploadChunkSamples = 2048;
constexpr size_t kMaxResponseBytes = 256 * 1024;

struct AuthResult {
    bool success = false;
    int http_status = 0;
    std::string model_resource_name;
    std::string model_display_name;
    std::string error_code;
    std::string error_message;
};

struct HttpResponse {
    int status_code = 0;
    uint64_t upload_elapsed_ms = 0;
    std::string body;
    std::string error_code;
    std::string error_message;
};

struct AuthTaskContext {
    std::string api_key;
    std::string model_name;
    uint32_t generation = 0;
};

std::mutex s_mutex;
EventHandler s_event_handler = nullptr;
void* s_event_context = nullptr;
bool s_initialized = false;
bool s_network_connected = false;
bool s_access_point_mode = false;
bool s_request_in_flight = false;
bool s_auth_checked = false;
bool s_authenticated = false;
uint32_t s_auth_generation = 0;
int s_last_http_status = 0;
std::string s_stored_api_key;
std::string s_last_status_message;
std::string s_last_model_resource_name;
std::string s_last_model_display_name;
std::string s_last_error_code;
std::string s_last_error_message;

std::string TrimCopy(std::string value)
{
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string TrimForLog(std::string value, size_t max_len = 96)
{
    value = TrimCopy(std::move(value));
    if (value.size() <= max_len) {
        return value;
    }
    if (max_len <= 3) {
        return value.substr(0, max_len);
    }
    return value.substr(0, max_len - 3) + "...";
}

std::string ReadNvsString(nvs_handle_t handle, const char* key)
{
    size_t size = 0;
    if (nvs_get_str(handle, key, nullptr, &size) != ESP_OK || size == 0) {
        return {};
    }

    std::string value(size, '\0');
    if (nvs_get_str(handle, key, value.data(), &size) != ESP_OK) {
        return {};
    }
    if (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

std::string LoadStoredApiKey()
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kStorageNamespace, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return {};
    }
    if (err != ESP_OK) {
        ESP_LOGW(kSettingsTag, "Failed to open OpenRouter NVS namespace: %s", esp_err_to_name(err));
        return {};
    }

    const std::string api_key = TrimCopy(ReadNvsString(handle, kStorageApiKey));
    nvs_close(handle);
    return api_key;
}

bool SaveStoredApiKey(const std::string& api_key)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kStorageNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(kSettingsTag, "Failed to open OpenRouter NVS namespace for write: %s",
                 esp_err_to_name(err));
        return false;
    }

    err = nvs_set_str(handle, kStorageApiKey, api_key.c_str());
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(kSettingsTag, "Failed to save OpenRouter API key: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool ClearStoredApiKeyFromNvs()
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kStorageNamespace, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (err != ESP_OK) {
        ESP_LOGE(kSettingsTag, "Failed to open OpenRouter NVS namespace for clear: %s",
                 esp_err_to_name(err));
        return false;
    }

    err = nvs_erase_key(handle, kStorageApiKey);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(kSettingsTag, "Failed to clear OpenRouter API key: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

std::string GetSdkConfigApiKey()
{
#if defined(CONFIG_FOLLOWUP_OPENROUTER_API_KEY)
    return TrimCopy(CONFIG_FOLLOWUP_OPENROUTER_API_KEY);
#else
    return {};
#endif
}

std::string GetEffectiveApiKeyLocked()
{
    if (!s_stored_api_key.empty()) {
        return s_stored_api_key;
    }
    return GetSdkConfigApiKey();
}

ApiKeySource GetApiKeySourceLocked()
{
    if (!s_stored_api_key.empty()) {
        return ApiKeySource::kNvs;
    }
    if (!GetSdkConfigApiKey().empty()) {
        return ApiKeySource::kSdkConfig;
    }
    return ApiKeySource::kNone;
}

std::string GetApiKeyLast4Locked()
{
    const std::string key = GetEffectiveApiKeyLocked();
    if (key.size() < 4) {
        return {};
    }
    return key.substr(key.size() - 4);
}

void SetLastErrorLocked(const char* error_code, const char* message)
{
    s_last_error_code = error_code != nullptr ? error_code : "";
    s_last_error_message = message != nullptr ? message : "";
}

void ClearLastErrorLocked()
{
    s_last_error_code.clear();
    s_last_error_message.clear();
}

Snapshot BuildSnapshotLocked()
{
    const std::string sdkconfig_api_key = GetSdkConfigApiKey();
    const bool configured = !GetEffectiveApiKeyLocked().empty();

    Snapshot snapshot = {};
    snapshot.settings.configured = configured;
    snapshot.settings.has_stored_api_key = !s_stored_api_key.empty();
    snapshot.settings.has_sdkconfig_api_key = !sdkconfig_api_key.empty();
    snapshot.settings.api_key_source = GetApiKeySourceLocked();
    snapshot.settings.api_key_last4 = GetApiKeyLast4Locked();
    snapshot.settings.model_name = kDefaultModelName;

    snapshot.runtime.initialized = s_initialized;
    snapshot.runtime.ready = configured && s_authenticated;
    snapshot.runtime.request_in_flight = s_request_in_flight;
    snapshot.runtime.auth_checked = s_auth_checked;
    snapshot.runtime.authenticated = s_authenticated;
    snapshot.runtime.supports_audio_understanding = false;
    snapshot.runtime.supports_structured_output = false;
    snapshot.runtime.last_http_status = s_last_http_status;
    snapshot.runtime.last_status_message = s_last_status_message;
    snapshot.runtime.last_model_resource_name = s_last_model_resource_name;
    snapshot.runtime.last_model_display_name = s_last_model_display_name;
    snapshot.runtime.last_error_code = s_last_error_code;
    snapshot.runtime.last_error_message = s_last_error_message;
    return snapshot;
}

void Notify()
{
    EventHandler handler = nullptr;
    void* context = nullptr;
    Event event = {};
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        handler = s_event_handler;
        context = s_event_context;
        event.snapshot = BuildSnapshotLocked();
    }

    if (handler != nullptr) {
        handler(event, context);
    }
}

bool ShouldStartAuthenticationLocked()
{
    return s_initialized &&
           s_network_connected &&
           !s_request_in_flight &&
           !s_authenticated &&
           !GetEffectiveApiKeyLocked().empty();
}

esp_err_t HttpEventHandler(esp_http_client_event_t* event)
{
    if (event == nullptr) {
        return ESP_FAIL;
    }
    auto* response = static_cast<HttpResponse*>(event->user_data);
    if (event->event_id == HTTP_EVENT_ON_DATA &&
        response != nullptr &&
        event->data != nullptr &&
        event->data_len > 0) {
        if (response->body.size() + static_cast<size_t>(event->data_len) > kMaxResponseBytes) {
            response->error_code = "response_too_large";
            response->error_message = "Provider response exceeded 256 KiB";
            return ESP_FAIL;
        }
        if (response->error_code.empty()) {
            response->body.append(static_cast<const char*>(event->data), event->data_len);
        }
    }
    return ESP_OK;
}

std::string JsonStringField(cJSON* root, const char* key)
{
    if (root == nullptr || key == nullptr) {
        return {};
    }

    cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        return {};
    }
    return item->valuestring;
}

void PopulateHttpError(cJSON* root, const HttpResponse& response,
                       std::string* error_code, std::string* error_message)
{
    if (error_code == nullptr || error_message == nullptr) {
        return;
    }

    *error_code = "http_error";
    error_message->clear();
    if (root != nullptr) {
        cJSON* error = cJSON_GetObjectItemCaseSensitive(root, "error");
        if (cJSON_IsObject(error)) {
            const std::string status = JsonStringField(error, "status");
            const std::string message = JsonStringField(error, "message");
            if (!status.empty()) {
                *error_code = status;
            }
            if (!message.empty()) {
                *error_message = message;
            }
        }
    }
    if (error_message->empty()) {
        *error_message =
            response.body.empty() ? "OpenRouter request failed" : response.body;
    }
    *error_message = TrimForLog(std::move(*error_message));
}

HttpResponse PerformOpenRouterKeyGet(const std::string& api_key,
                                   const std::string& model_name)
{
    HttpResponse response = {};
    std::string url = kOpenRouterApiBaseUrl;
    url += "key";
    (void)model_name;

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = kAuthTimeoutMs;
    config.event_handler = &HttpEventHandler;
    config.user_data = &response;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        response.error_code = "http_client_init_failed";
        response.error_message = "Failed to initialize OpenRouter HTTP client";
        return response;
    }

    const std::string authorization = "Bearer " + api_key;
    esp_http_client_set_header(client, "Authorization", authorization.c_str());
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "folloup-sticky");

    const esp_err_t err = esp_http_client_perform(client);
    response.status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK && response.error_code.empty()) {
        response.error_code = "transport_error";
        response.error_message = esp_err_to_name(err);
    }
    return response;
}

AuthResult Authenticate(const std::string& api_key, const std::string& model_name)
{
    AuthResult result = {};
    const HttpResponse http = PerformOpenRouterKeyGet(api_key, model_name);
    result.http_status = http.status_code;

    if (!http.error_code.empty()) {
        result.error_code = http.error_code;
        result.error_message = TrimForLog(http.error_message);
        return result;
    }

    cJSON* root = cJSON_ParseWithLength(http.body.c_str(), http.body.size());
    if (http.status_code >= 200 && http.status_code < 300) {
        result.success = cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(root, "data"));
        result.model_resource_name = model_name;
        result.model_display_name = "OpenRouter";
        if (!result.success) {
            result.error_code = "invalid_response";
            result.error_message = "Invalid OpenRouter key response";
        }
        if (root != nullptr) {
            cJSON_Delete(root);
        }
        return result;
    }

    PopulateHttpError(root, http, &result.error_code, &result.error_message);
    if (root != nullptr) {
        cJSON_Delete(root);
    }
    return result;
}

void CompleteAuthentication(uint32_t generation, const AuthResult& result)
{
    bool stale_result = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (generation != s_auth_generation) {
            stale_result = true;
        }
        if (!stale_result) {
            s_request_in_flight = false;
            s_auth_checked = true;
            s_last_http_status = result.http_status;

            if (!result.success) {
                s_authenticated = false;
                s_last_status_message = "Authentication failed";
                s_last_model_resource_name.clear();
                s_last_model_display_name.clear();
                SetLastErrorLocked(result.error_code.c_str(), result.error_message.c_str());
            } else {
                s_authenticated = true;
                s_last_model_resource_name = result.model_resource_name;
                s_last_model_display_name = result.model_display_name;
                s_last_status_message = !s_last_model_display_name.empty()
                                            ? "Authenticated with " + s_last_model_display_name
                                            : "Authenticated with OpenRouter";
                ClearLastErrorLocked();
            }
        }
    }

    if (stale_result) {
        ESP_LOGI(kTag, "Ignoring stale OpenRouter authentication result for generation %lu",
                 static_cast<unsigned long>(generation));
        return;
    }

    if (!result.success) {
        ESP_LOGW(kTag, "OpenRouter authentication failed: http=%d code=%s message=%s",
                 result.http_status,
                 result.error_code.empty() ? "http_error" : result.error_code.c_str(),
                 result.error_message.empty() ? "unknown" : result.error_message.c_str());
    } else {
        ESP_LOGI(kTag, "OpenRouter authentication succeeded: model=%s display=%s http=%d",
                 result.model_resource_name.empty() ? "unknown"
                                                    : result.model_resource_name.c_str(),
                 result.model_display_name.empty() ? "unknown"
                                                   : result.model_display_name.c_str(),
                 result.http_status);
    }

    Notify();
}

void AuthenticationTask(void* arg)
{
    std::unique_ptr<AuthTaskContext> context(static_cast<AuthTaskContext*>(arg));
    if (context == nullptr) {
        CompleteAuthentication(0, AuthResult{
            .success = false,
            .http_status = 0,
            .model_resource_name = {},
            .model_display_name = {},
            .error_code = "task_context_missing",
            .error_message = "OpenRouter authentication task context missing",
        });
        vTaskDelete(nullptr);
        return;
    }

    const AuthResult result = Authenticate(context->api_key, context->model_name);
    CompleteAuthentication(context->generation, result);
    vTaskDelete(nullptr);
}

void MaybeBeginAuthentication()
{
    bool should_start = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        should_start = ShouldStartAuthenticationLocked();
    }
    if (should_start) {
        (void)BeginAuthentication();
    }
}

std::string ReadRequestBody(httpd_req_t* request)
{
    if (request == nullptr || request->content_len <= 0) {
        return {};
    }

    std::string body(static_cast<size_t>(request->content_len), '\0');
    size_t offset = 0;
    while (offset < body.size()) {
        const int received = httpd_req_recv(request, body.data() + offset, body.size() - offset);
        if (received <= 0) {
            return {};
        }
        offset += static_cast<size_t>(received);
    }
    return body;
}

std::string JsonString(cJSON* root)
{
    if (root == nullptr) {
        return "{}";
    }

    char* raw = cJSON_PrintUnformatted(root);
    if (raw == nullptr) {
        return "{}";
    }
    std::string json(raw);
    cJSON_free(raw);
    return json;
}

esp_err_t SendJsonResponse(httpd_req_t* request, int status_code, cJSON* root)
{
    if (request == nullptr) {
        if (root != nullptr) {
            cJSON_Delete(root);
        }
        return ESP_FAIL;
    }

    const std::string payload = JsonString(root);
    if (root != nullptr) {
        cJSON_Delete(root);
    }

    switch (status_code) {
        case 200:
            httpd_resp_set_status(request, HTTPD_200);
            break;
        case 400:
            httpd_resp_set_status(request, HTTPD_400);
            break;
        case 500:
        default:
            httpd_resp_set_status(request, HTTPD_500);
            break;
    }
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    return httpd_resp_send(request, payload.c_str(), payload.size());
}

void AppendSnapshot(cJSON* root, const Snapshot& snapshot, const char* message)
{
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "message", message != nullptr ? message : "");

    cJSON* settings = cJSON_AddObjectToObject(root, "settings");
    cJSON_AddBoolToObject(settings, "configured", snapshot.settings.configured);
    cJSON_AddBoolToObject(settings, "has_stored_api_key", snapshot.settings.has_stored_api_key);
    cJSON_AddBoolToObject(settings, "has_sdkconfig_api_key",
                          snapshot.settings.has_sdkconfig_api_key);
    cJSON_AddStringToObject(settings, "api_key_source",
                            ApiKeySourceName(snapshot.settings.api_key_source));
    cJSON_AddStringToObject(settings, "api_key_last4",
                            snapshot.settings.api_key_last4.c_str());
    cJSON_AddStringToObject(settings, "model_name", snapshot.settings.model_name.c_str());
    cJSON_AddStringToObject(settings, "transcription_model", CONFIG_FOLLOWUP_OPENROUTER_STT_MODEL);

    cJSON* runtime = cJSON_AddObjectToObject(root, "runtime");
    cJSON_AddBoolToObject(runtime, "initialized", snapshot.runtime.initialized);
    cJSON_AddBoolToObject(runtime, "ready", snapshot.runtime.ready);
    cJSON_AddBoolToObject(runtime, "request_in_flight", snapshot.runtime.request_in_flight);
    cJSON_AddBoolToObject(runtime, "auth_checked", snapshot.runtime.auth_checked);
    cJSON_AddBoolToObject(runtime, "authenticated", snapshot.runtime.authenticated);
    cJSON_AddBoolToObject(runtime, "supports_audio_understanding",
                          snapshot.runtime.supports_audio_understanding);
    cJSON_AddBoolToObject(runtime, "supports_structured_output",
                          snapshot.runtime.supports_structured_output);
    cJSON_AddNumberToObject(runtime, "last_http_status", snapshot.runtime.last_http_status);
    cJSON_AddStringToObject(runtime, "last_status_message",
                            snapshot.runtime.last_status_message.c_str());
    cJSON_AddStringToObject(runtime, "last_model_resource_name",
                            snapshot.runtime.last_model_resource_name.c_str());
    cJSON_AddStringToObject(runtime, "last_model_display_name",
                            snapshot.runtime.last_model_display_name.c_str());
    cJSON_AddStringToObject(runtime, "last_error_code",
                            snapshot.runtime.last_error_code.c_str());
    cJSON_AddStringToObject(runtime, "last_error_message",
                            snapshot.runtime.last_error_message.c_str());
}

bool ParsePatchBody(const std::string& body, SettingsPatch* patch, std::string* error)
{
    if (patch == nullptr) {
        return false;
    }

    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
    if (root == nullptr) {
        if (error != nullptr) {
            *error = "Invalid JSON body";
        }
        return false;
    }

    cJSON* api_key = cJSON_GetObjectItemCaseSensitive(root, "api_key");
    if (cJSON_IsString(api_key) && api_key->valuestring != nullptr) {
        patch->has_api_key = true;
        patch->api_key = api_key->valuestring;
    } else if (api_key != nullptr && !cJSON_IsNull(api_key)) {
        if (error != nullptr) {
            *error = "Invalid api_key";
        }
        cJSON_Delete(root);
        return false;
    }

    cJSON_Delete(root);
    return true;
}

esp_err_t RegisterPortalRoute(httpd_handle_t server, const httpd_uri_t* handler)
{
    if (server == nullptr || handler == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t err = httpd_register_uri_handler(server, handler);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to register OpenRouter portal route %s [%d]: %s",
                 handler->uri != nullptr ? handler->uri : "<null>",
                 static_cast<int>(handler->method),
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t HandlePortalSettingsGet(httpd_req_t* request)
{
    cJSON* root = cJSON_CreateObject();
    AppendSnapshot(root, GetSnapshot(), "OpenRouter settings loaded");
    return SendJsonResponse(request, 200, root);
}

esp_err_t HandlePortalSettingsPatch(httpd_req_t* request)
{
    if (request == nullptr ||
        request->content_len <= 0 ||
        request->content_len > static_cast<int>(kMaxPortalPayloadLen)) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "message", "Invalid OpenRouter settings payload");
        return SendJsonResponse(request, 400, root);
    }

    const std::string body = ReadRequestBody(request);
    SettingsPatch patch = {};
    std::string parse_error;
    if (body.empty() || !ParsePatchBody(body, &patch, &parse_error)) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "message",
                                parse_error.empty() ? "Invalid OpenRouter settings payload"
                                                    : parse_error.c_str());
        return SendJsonResponse(request, 400, root);
    }

    const Result result = ApplySettingsPatch(patch);
    if (!result.success) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "message", result.message.c_str());
        cJSON_AddStringToObject(root, "error_code", result.error_code.c_str());
        cJSON_AddStringToObject(root, "field", result.field.c_str());
        return SendJsonResponse(request, result.status_code, root);
    }

    cJSON* root = cJSON_CreateObject();
    AppendSnapshot(root, GetSnapshot(), "OpenRouter API key stored");
    return SendJsonResponse(request, 200, root);
}

esp_err_t HandlePortalSettingsReset(httpd_req_t* request)
{
    const Result result = ClearStoredApiKey();
    if (!result.success) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "message", result.message.c_str());
        cJSON_AddStringToObject(root, "error_code", result.error_code.c_str());
        cJSON_AddStringToObject(root, "field", result.field.c_str());
        return SendJsonResponse(request, result.status_code, root);
    }

    cJSON* root = cJSON_CreateObject();
    AppendSnapshot(root, GetSnapshot(), "OpenRouter API key cleared");
    return SendJsonResponse(request, 200, root);
}

esp_err_t HandlePortalRuntimeGet(httpd_req_t* request)
{
    cJSON* root = cJSON_CreateObject();
    AppendSnapshot(root, GetSnapshot(), "OpenRouter runtime loaded");
    return SendJsonResponse(request, 200, root);
}

// Synchronous JSON POST to OpenRouter.
HttpResponse PerformOpenRouterPost(const std::string& url, const std::string& api_key,
                               const std::string& body)
{
    HttpResponse response = {};
    if (body.empty()) {
        response.error_code = "request_build_failed";
        response.error_message = "Failed to allocate request JSON";
        return response;
    }

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = kGenerateTimeoutMs;
    config.event_handler = &HttpEventHandler;
    config.user_data = &response;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        response.error_code = "http_client_init_failed";
        response.error_message = "Failed to initialize OpenRouter HTTP client";
        return response;
    }

    const std::string authorization = "Bearer " + api_key;
    esp_http_client_set_header(client, "Authorization", authorization.c_str());
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "folloup-sticky");
    esp_http_client_set_post_field(client, body.c_str(), static_cast<int>(body.size()));

    const esp_err_t err = esp_http_client_perform(client);
    response.status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK && response.error_code.empty()) {
        response.error_code = "transport_error";
        response.error_message = esp_err_to_name(err);
    }
    return response;
}

void AppendLe16(uint16_t value, std::array<uint8_t, 44>* out, size_t* offset)
{
    if (out == nullptr || offset == nullptr || *offset + 2U > out->size()) {
        return;
    }
    (*out)[(*offset)++] = static_cast<uint8_t>(value & 0xFF);
    (*out)[(*offset)++] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void AppendLe32(uint32_t value, std::array<uint8_t, 44>* out, size_t* offset)
{
    if (out == nullptr || offset == nullptr || *offset + 4U > out->size()) {
        return;
    }
    (*out)[(*offset)++] = static_cast<uint8_t>(value & 0xFF);
    (*out)[(*offset)++] = static_cast<uint8_t>((value >> 8) & 0xFF);
    (*out)[(*offset)++] = static_cast<uint8_t>((value >> 16) & 0xFF);
    (*out)[(*offset)++] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

std::array<uint8_t, 44> BuildWavHeaderPcm16Mono(size_t sample_count, uint32_t sample_rate_hz)
{
    constexpr uint16_t kChannels = 1;
    constexpr uint16_t kBitsPerSample = 16;
    constexpr uint16_t kBlockAlign = kChannels * (kBitsPerSample / 8U);
    const uint32_t data_bytes = static_cast<uint32_t>(sample_count * sizeof(int16_t));
    const uint32_t byte_rate = sample_rate_hz * kBlockAlign;

    std::array<uint8_t, 44> header = {};
    size_t offset = 0;
    header[offset++] = 'R';
    header[offset++] = 'I';
    header[offset++] = 'F';
    header[offset++] = 'F';
    AppendLe32(36U + data_bytes, &header, &offset);
    header[offset++] = 'W';
    header[offset++] = 'A';
    header[offset++] = 'V';
    header[offset++] = 'E';
    header[offset++] = 'f';
    header[offset++] = 'm';
    header[offset++] = 't';
    header[offset++] = ' ';
    AppendLe32(16U, &header, &offset);
    AppendLe16(1U, &header, &offset);
    AppendLe16(kChannels, &header, &offset);
    AppendLe32(sample_rate_hz, &header, &offset);
    AppendLe32(byte_rate, &header, &offset);
    AppendLe16(kBlockAlign, &header, &offset);
    AppendLe16(kBitsPerSample, &header, &offset);
    header[offset++] = 'd';
    header[offset++] = 'a';
    header[offset++] = 't';
    header[offset++] = 'a';
    AppendLe32(data_bytes, &header, &offset);
    return header;
}

bool ReadHttpResponseBody(esp_http_client_handle_t client, HttpResponse* response)
{
    if (client == nullptr || response == nullptr) {
        return false;
    }
    std::array<char, 512> buffer = {};
    while (true) {
        const int read = esp_http_client_read(client, buffer.data(), buffer.size());
        if (read < 0) {
            response->error_code = "transport_error";
            response->error_message = "Failed reading HTTP response body";
            return false;
        }
        if (read == 0) {
            break;
        }
        if (response->body.size() + static_cast<size_t>(read) > kMaxResponseBytes) {
            response->error_code = "response_too_large";
            response->error_message = "Provider response exceeded 256 KiB";
            return false;
        }
        response->body.append(buffer.data(), static_cast<size_t>(read));
    }
    return true;
}

HttpResponse PerformAudioTranscription(const std::string& api_key,
                                      const recording_service::RecordedClip& clip)
{
    HttpResponse response = {};
    const std::string prefix = openrouter_protocol::AudioPrefix(CONFIG_FOLLOWUP_OPENROUTER_STT_MODEL);
    const std::string suffix = openrouter_protocol::AudioSuffix();
    const size_t total_bytes = prefix.size() + clip.wav_byte_count() + suffix.size();
    if (total_bytes > INT_MAX) {
        response.error_code = "audio_too_large";
        response.error_message = "Recording exceeds upload size limit";
        return response;
    }
    esp_http_client_config_t config = {};
    config.url = "https://openrouter.ai/api/v1/audio/transcriptions";
    config.method = HTTP_METHOD_POST;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = kTranscribeTimeoutMs;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        response.error_code = "http_client_init_failed";
        response.error_message = "Failed to initialize audio HTTP client";
        return response;
    }
    const std::string authorization = "Bearer " + api_key;
    esp_http_client_set_header(client, "Authorization", authorization.c_str());
    esp_http_client_set_header(client, "Content-Type", openrouter_protocol::kAudioContentType);
    esp_http_client_set_header(client, "Accept", "application/json");
    const int64_t upload_started_us = esp_timer_get_time();
    const esp_err_t err = esp_http_client_open(client, static_cast<int>(total_bytes));
    if (err != ESP_OK && response.error_code.empty()) {
        response.error_code = "transport_error";
        response.error_message = esp_err_to_name(err);
        esp_http_client_cleanup(client);
        return response;
    }
    auto write_all = [&](const void* data, size_t size) {
        return openrouter_protocol::WriteAll(static_cast<const char*>(data), size,
            [&](const char* bytes, int count) { return esp_http_client_write(client, bytes, count); });
    };
    const auto header = BuildWavHeaderPcm16Mono(clip.sample_count(), clip.sample_rate_hz());
    bool ok = write_all(prefix.data(), prefix.size()) && write_all(header.data(), header.size());
    clip.ForEachChunk([&](const int16_t* data, size_t size) {
        if (ok && size != 0) ok = data != nullptr && write_all(data, size * sizeof(int16_t));
    });
    ok = ok && write_all(suffix.data(), suffix.size());
    response.upload_elapsed_ms = (esp_timer_get_time() - upload_started_us) / 1000ULL;
    if (!ok) {
        response.error_code = "transport_error";
        response.error_message = "Failed streaming audio upload";
    } else if (esp_http_client_fetch_headers(client) < 0) {
        response.error_code = "transport_error";
        response.error_message = "Failed reading audio response headers";
    } else {
        response.status_code = esp_http_client_get_status_code(client);
        ReadHttpResponseBody(client, &response);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return response;
}

uint32_t ResolveUploadChunkCount(const recording_service::RecordedClip& clip)
{
    return static_cast<uint32_t>((clip.sample_count() + kHttpUploadChunkSamples - 1U) /
                                 kHttpUploadChunkSamples);
}

}  // namespace

esp_err_t Init()
{
    Snapshot snapshot = {};
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_initialized) {
            return ESP_OK;
        }

        s_stored_api_key = LoadStoredApiKey();
        s_last_status_message =
            GetEffectiveApiKeyLocked().empty()
                ? "No OpenRouter API key configured"
                : "OpenRouter API key available";
        ClearLastErrorLocked();
        s_initialized = true;
        snapshot = BuildSnapshotLocked();
    }

    ESP_LOGI(kTag, "OpenRouter service initialized: configured=%d source=%s key_last4=%s",
             snapshot.settings.configured ? 1 : 0,
             ApiKeySourceName(snapshot.settings.api_key_source),
             snapshot.settings.api_key_last4.empty()
                 ? "none"
                 : snapshot.settings.api_key_last4.c_str());
    Notify();
    MaybeBeginAuthentication();
    return ESP_OK;
}

void SetEventHandler(EventHandler handler, void* context)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_event_handler = handler;
    s_event_context = context;
}

Snapshot GetSnapshot()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return BuildSnapshotLocked();
}

Result ApplySettingsPatch(const SettingsPatch& patch)
{
    bool should_start_auth = false;
    bool save_failed = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_initialized) {
            s_stored_api_key = LoadStoredApiKey();
            s_initialized = true;
        }

        if (!patch.has_api_key) {
            return {
                .success = false,
                .validation_error = true,
                .status_code = 400,
                .field = "api_key",
                .error_code = "missing_api_key",
                .message = "OpenRouter API key is required",
            };
        }

        const std::string trimmed = TrimCopy(patch.api_key);
        if (trimmed.empty() || trimmed.size() > 256 ||
            trimmed.find_first_of("\r\n") != std::string::npos) {
            return {
                .success = false,
                .validation_error = true,
                .status_code = 400,
                .field = "api_key",
                .error_code = "invalid_api_key",
                .message = "OpenRouter API key is required",
            };
        }

        if (!SaveStoredApiKey(trimmed)) {
            SetLastErrorLocked("nvs_write_failed", "Failed to store OpenRouter API key");
            save_failed = true;
        } else {
            s_stored_api_key = trimmed;
            ++s_auth_generation;
            s_request_in_flight = false;
            s_auth_checked = false;
            s_authenticated = false;
            s_last_http_status = 0;
            s_last_status_message = "OpenRouter API key stored";
            s_last_model_resource_name.clear();
            s_last_model_display_name.clear();
            ClearLastErrorLocked();
            should_start_auth = ShouldStartAuthenticationLocked();
        }
    }

    if (save_failed) {
        Notify();
        return {
            .success = false,
            .validation_error = false,
            .status_code = 500,
            .field = "api_key",
            .error_code = "nvs_write_failed",
            .message = "Failed to store OpenRouter API key",
        };
    }

    Notify();
    if (should_start_auth) {
        (void)BeginAuthentication();
    }
    return {
        .success = true,
        .validation_error = false,
        .status_code = 200,
        .field = {},
        .error_code = {},
        .message = "OpenRouter API key stored",
    };
}

Result ClearStoredApiKey()
{
    bool should_start_auth = false;
    bool clear_failed = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_initialized) {
            s_stored_api_key = LoadStoredApiKey();
            s_initialized = true;
        }

        if (!ClearStoredApiKeyFromNvs()) {
            SetLastErrorLocked("nvs_clear_failed", "Failed to clear OpenRouter API key");
            clear_failed = true;
        } else {
            s_stored_api_key.clear();
            ++s_auth_generation;
            s_request_in_flight = false;
            s_auth_checked = false;
            s_authenticated = false;
            s_last_http_status = 0;
            s_last_status_message = "OpenRouter API key cleared";
            s_last_model_resource_name.clear();
            s_last_model_display_name.clear();
            ClearLastErrorLocked();
            should_start_auth = ShouldStartAuthenticationLocked();
        }
    }

    Notify();
    if (clear_failed) {
        return {
            .success = false,
            .validation_error = false,
            .status_code = 500,
            .field = "api_key",
            .error_code = "nvs_clear_failed",
            .message = "Failed to clear OpenRouter API key",
        };
    }

    if (should_start_auth) {
        (void)BeginAuthentication();
    }
    return {
        .success = true,
        .validation_error = false,
        .status_code = 200,
        .field = {},
        .error_code = {},
        .message = "OpenRouter API key cleared",
    };
}

bool HasApiKey()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return !GetEffectiveApiKeyLocked().empty();
}

std::string GetEffectiveApiKey()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return GetEffectiveApiKeyLocked();
}

std::string GetEffectiveModelName()
{
    return kDefaultModelName;
}

TextResult GenerateText(const std::string& prompt)
{
    TextResult result = {};
    const std::string api_key = GetEffectiveApiKey();
    const std::string model_name = GetEffectiveModelName();
    if (api_key.empty() || model_name.empty()) {
        result.error_code = "not_configured";
        result.error_message = "No OpenRouter API key configured";
        return result;
    }
    if (prompt.empty()) {
        result.error_code = "empty_prompt";
        result.error_message = "Prompt was empty";
        return result;
    }

    const std::string url = std::string(kOpenRouterApiBaseUrl) + "chat/completions";
    const HttpResponse http = PerformOpenRouterPost(url, api_key, openrouter_protocol::TextRequest(model_name, prompt));
    result.http_status = http.status_code;
    if (!http.error_code.empty()) {
        result.error_code = http.error_code;
        result.error_message = TrimForLog(http.error_message);
    } else {
        cJSON* root = cJSON_ParseWithLength(http.body.c_str(), http.body.size());
        if (http.status_code >= 200 && http.status_code < 300) {
            result.text = openrouter_protocol::CompletionText(root);
            result.success = !result.text.empty();
            if (!result.success) {
                result.error_code = "empty_response";
                result.error_message = "OpenRouter returned no text";
            }
        } else {
            PopulateHttpError(root, http, &result.error_code, &result.error_message);
        }
        if (root != nullptr) {
            cJSON_Delete(root);
        }
    }

    if (result.success) {
        ESP_LOGI(kTag, "OpenRouter chat completion succeeded: http=%d chars=%u", result.http_status,
                 static_cast<unsigned>(result.text.size()));
    } else {
        ESP_LOGW(kTag, "OpenRouter chat completion failed: http=%d code=%s message=%s",
                 result.http_status,
                 result.error_code.empty() ? "<none>" : result.error_code.c_str(),
                 result.error_message.empty() ? "<none>" : result.error_message.c_str());
    }
    return result;
}

TranscriptionResult Transcribe(const recording_service::RecordedClip& clip)
{
    TranscriptionResult result = {};
    result.clip_duration_ms = clip.duration_ms();
    result.wav_bytes = clip.wav_byte_count();
    result.upload_chunk_count = ResolveUploadChunkCount(clip);

    const std::string api_key = GetEffectiveApiKey();
    const std::string model_name = GetEffectiveModelName();
    if (api_key.empty() || model_name.empty()) {
        result.error_code = "not_configured";
        result.error_message = "No OpenRouter API key configured";
        return result;
    }
    if (clip.empty()) {
        result.error_code = "empty_audio";
        result.error_message = "No recorded audio available";
        return result;
    }

    const int64_t task_started_us = esp_timer_get_time();

    const HttpResponse http = PerformAudioTranscription(api_key, clip);
    result.upload_elapsed_ms = http.upload_elapsed_ms;
    result.http_status = http.status_code;
    result.total_elapsed_ms =
        static_cast<uint64_t>((esp_timer_get_time() - task_started_us) / 1000ULL);
    if (!http.error_code.empty()) {
        result.error_code = http.error_code;
        result.error_message = TrimForLog(http.error_message);
        return result;
    }

    cJSON* root = cJSON_ParseWithLength(http.body.c_str(), http.body.size());
    if (http.status_code >= 200 && http.status_code < 300) {
        result.transcript = TrimCopy(JsonStringField(root, "text"));
        result.success = !result.transcript.empty();
        if (!result.success) {
            result.error_code = "empty_transcript";
            result.error_message = "OpenRouter returned no transcript text";
        }
    } else {
        PopulateHttpError(root, http, &result.error_code, &result.error_message);
    }
    if (root != nullptr) {
        cJSON_Delete(root);
    }
    return result;
}

bool BeginAuthentication()
{
    std::string api_key;
    std::string model_name;
    ApiKeySource api_key_source = ApiKeySource::kNone;
    std::string api_key_last4;
    uint32_t auth_generation = 0;
    bool missing_api_key = false;
    bool task_alloc_failed = false;
    bool task_start_failed = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_initialized) {
            s_stored_api_key = LoadStoredApiKey();
            s_initialized = true;
        }
        if (s_request_in_flight) {
            return false;
        }

        api_key = GetEffectiveApiKeyLocked();
        if (api_key.empty()) {
            s_request_in_flight = false;
            s_auth_checked = false;
            s_authenticated = false;
            s_last_http_status = 0;
            s_last_status_message = "Authentication skipped";
            s_last_model_resource_name.clear();
            s_last_model_display_name.clear();
            SetLastErrorLocked("not_configured", "No OpenRouter API key configured");
            missing_api_key = true;
        } else if (!s_network_connected) {
            return false;
        }

        if (!missing_api_key) {
            s_request_in_flight = true;
            s_auth_checked = false;
            s_authenticated = false;
            s_last_http_status = 0;
            s_last_status_message = "Authenticating with OpenRouter";
            s_last_model_resource_name.clear();
            s_last_model_display_name.clear();
            ClearLastErrorLocked();
            model_name = GetEffectiveModelName();
            api_key_source = GetApiKeySourceLocked();
            api_key_last4 = GetApiKeyLast4Locked();
            auth_generation = ++s_auth_generation;
        }
    }

    if (missing_api_key) {
        Notify();
        return false;
    }

    Notify();

    std::unique_ptr<AuthTaskContext> context(new (std::nothrow) AuthTaskContext{
        .api_key = std::move(api_key),
        .model_name = std::move(model_name),
        .generation = auth_generation,
    });
    if (context == nullptr) {
        task_alloc_failed = true;
    } else {
        TaskHandle_t task_handle = nullptr;
        const BaseType_t created = xTaskCreatePinnedToCore(
            AuthenticationTask,
            "gemini_auth",
            kAuthTaskStackWords,
            context.get(),
            followup_task_config::kPriorityGemini,
            &task_handle,
            followup_task_config::kSystemCore);
        if (created != pdPASS || task_handle == nullptr) {
            task_start_failed = true;
        } else {
            context.release();
        }
    }

    if (task_alloc_failed || task_start_failed) {
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_request_in_flight = false;
            s_last_status_message = "Failed to start OpenRouter authentication";
            SetLastErrorLocked(task_alloc_failed ? "task_alloc_failed" : "task_start_failed",
                               task_alloc_failed
                                   ? "Failed to allocate OpenRouter task context"
                                   : "Failed to start OpenRouter authentication task");
        }
        Notify();
        return false;
    }

    ESP_LOGI(kTag, "Starting OpenRouter authentication (model=%s, source=%s, key_last4=%s)",
             GetEffectiveModelName().c_str(),
             ApiKeySourceName(api_key_source),
             api_key_last4.empty() ? "none" : api_key_last4.c_str());
    return true;
}

void SetNetworkState(bool connected, bool access_point_mode)
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_network_connected = connected;
        s_access_point_mode = access_point_mode;
    }
    MaybeBeginAuthentication();
}

void RegisterPortalRoutes(httpd_handle_t server)
{
    if (server == nullptr) {
        return;
    }

    httpd_uri_t settings_get = {
        .uri = kPortalApiSettingsGeminiUri,
        .method = HTTP_GET,
        .handler = HandlePortalSettingsGet,
        .user_ctx = nullptr,
    };
    httpd_uri_t settings_patch = {
        .uri = kPortalApiSettingsGeminiUri,
        .method = HTTP_PATCH,
        .handler = HandlePortalSettingsPatch,
        .user_ctx = nullptr,
    };
    httpd_uri_t settings_reset = {
        .uri = kPortalApiSettingsGeminiResetUri,
        .method = HTTP_POST,
        .handler = HandlePortalSettingsReset,
        .user_ctx = nullptr,
    };
    httpd_uri_t runtime_get = {
        .uri = kPortalApiRuntimeGeminiUri,
        .method = HTTP_GET,
        .handler = HandlePortalRuntimeGet,
        .user_ctx = nullptr,
    };

    if (RegisterPortalRoute(server, &settings_get) != ESP_OK ||
        RegisterPortalRoute(server, &settings_patch) != ESP_OK ||
        RegisterPortalRoute(server, &settings_reset) != ESP_OK ||
        RegisterPortalRoute(server, &runtime_get) != ESP_OK) {
        ESP_LOGW(kTag, "OpenRouter portal routes are incomplete");
    }
}

const char* ApiKeySourceName(ApiKeySource source)
{
    switch (source) {
        case ApiKeySource::kSdkConfig:
            return "sdkconfig";
        case ApiKeySource::kNvs:
            return "nvs";
        case ApiKeySource::kNone:
        default:
            return "none";
    }
}

}  // namespace gemini_service
