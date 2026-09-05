# Local setup

Copy `.env.example` to `.env.local` and fill in the Wi-Fi and OpenRouter values.
Keep `.env.local` private. It is ignored by Git. The helper never creates it.

Run:

```sh
python3 scripts/configure_local.py
```

The helper reads `.env.local`, then applies matching `FOLLOWUP_*` environment
variables. A `CONFIG_FOLLOWUP_*` environment variable has the highest priority.
It updates only the five local settings in the ignored `sdkconfig`, preserves
all other settings, writes with mode `0600`, and replaces the file atomically.

The default text model is `qwen/qwen3-30b-a3b-instruct-2507`. The default
transcription model is `microsoft/mai-transcribe-2`. Empty API and Wi-Fi values
are safe placeholders; set real values only in `.env.local` or the environment.

You can target another pair of files for a test or a separate checkout:

```sh
python3 scripts/configure_local.py --env-file /path/to/.env.local --sdkconfig /path/to/sdkconfig
```
