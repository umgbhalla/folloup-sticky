# OpenRouter service

The existing `gemini_service` component now uses OpenRouter. Its internal C++
name and `/api/settings/gemini`, `/api/settings/gemini/reset`, and
`/api/runtime/gemini` routes remain compatible with current callers. These
routes now accept OpenRouter keys only. The NVS namespace is `openrouter`, so
previous Google keys cannot be sent to OpenRouter.

## Models and configuration

- `CONFIG_FOLLOWUP_OPENROUTER_API_KEY`: optional local build key, empty by default.
- `CONFIG_FOLLOWUP_OPENROUTER_STT_MODEL`: `microsoft/mai-transcribe-2`.
- `CONFIG_FOLLOWUP_OPENROUTER_TEXT_MODEL`: `qwen/qwen3-30b-a3b-instruct-2507`.

An OpenRouter key saved through the existing settings route overrides the build
key. Raw keys are never returned by the settings API. Configure models with
`idf.py menuconfig`. Keep keys in ignored `sdkconfig` or device NVS, not tracked
source. Reconfigure and build after changing Kconfig settings.

## Requests

All requests use HTTPS with the ESP-IDF certificate bundle and Bearer auth.

- Authentication: `GET https://openrouter.ai/api/v1/key`. This validates the key,
  not model availability or remaining funds. Inference errors are returned to
  the caller.
- Transcription: `POST /api/v1/audio/transcriptions`. The existing WAV header and
  PCM chunks stream as multipart `file` data with a separate `model` field.
  Partial socket writes are retried until all bytes are sent or a write fails.
  No full-size base64/audio copy is allocated. The response's `text` is saved.
- Summaries: `POST /api/v1/chat/completions` with one user message, temperature 0,
  no streaming, and a 2048-token output limit. Only a completed `stop` response
  is accepted; truncated and filtered outputs are rejected.

Responses are capped at 256 KiB. Transport and provider errors remain failures.
The existing asynchronous recording, summary, archive, and screen flows remain.
Summary chunk budgets use UTF-8 bytes as a conservative token estimate because
this integration has no exact tokenizer. The default text model has a 262K
context; choose a model with at least 128K context or lower the summary budgets.

## Verification

Source ESP-IDF `export.sh`, then run `scripts/test_openrouter.sh` and
`idf.py build`. The host check exercises request JSON, completion parsing,
partial writes, and multipart framing. It does not prove microphone recording,
live OpenRouter requests, or physical display behavior.

References: https://openrouter.ai/docs/guides/overview/multimodal/stt
and https://openrouter.ai/blog/announcements/announcing-audio-apis/
