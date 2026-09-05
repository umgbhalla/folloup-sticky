#!/usr/bin/env python3
"""Apply local environment values to the ignored ESP-IDF sdkconfig file."""

from __future__ import annotations

import argparse
import json
import os
import re
import tempfile
from pathlib import Path


KEYS = {
    "FOLLOWUP_WIFI_STA_SSID": "CONFIG_FOLLOWUP_WIFI_STA_SSID",
    "FOLLOWUP_WIFI_STA_PASSWORD": "CONFIG_FOLLOWUP_WIFI_STA_PASSWORD",
    "FOLLOWUP_OPENROUTER_API_KEY": "CONFIG_FOLLOWUP_OPENROUTER_API_KEY",
    "FOLLOWUP_OPENROUTER_TEXT_MODEL": "CONFIG_FOLLOWUP_OPENROUTER_TEXT_MODEL",
    "FOLLOWUP_OPENROUTER_STT_MODEL": "CONFIG_FOLLOWUP_OPENROUTER_STT_MODEL",
}
DEFAULTS = {
    "FOLLOWUP_OPENROUTER_TEXT_MODEL": "qwen/qwen3-30b-a3b-instruct-2507",
    "FOLLOWUP_OPENROUTER_STT_MODEL": "microsoft/mai-transcribe-2",
}
_ASSIGNMENT = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)=(.*)$")


def parse_env(path: Path) -> dict[str, str]:
    values = {}
    if not path.exists():
        return values
    for line_number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[7:].lstrip()
        match = _ASSIGNMENT.match(line)
        if not match:
            raise ValueError(f"{path}:{line_number}: expected KEY=VALUE")
        key, value = match.groups()
        values[key] = parse_value(value, path, line_number)
    return values


def parse_value(value: str, path: Path, line_number: int) -> str:
    if not value or value[0] not in "'\"":
        return value.split(" #", 1)[0].rstrip()
    quote = value[0]
    if quote == "'":
        end = value.find(quote, 1)
    else:
        escaped = False
        end = -1
        for index in range(1, len(value)):
            char = value[index]
            if char == quote and not escaped:
                end = index
                break
            if char == "\\" and not escaped:
                escaped = True
            else:
                escaped = False
    trailing = value[end + 1 :].strip() if end >= 0 else ""
    if end < 0 or trailing and not trailing.startswith("#"):
        raise ValueError(f"{path}:{line_number}: unterminated quoted value")
    body = value[1:end]
    if quote == "'":
        return body
    out = []
    escaped = False
    for char in body:
        if escaped:
            out.append({"n": "\n", "r": "\r", "t": "\t"}.get(char, char))
            escaped = False
        elif char == "\\":
            escaped = True
        else:
            out.append(char)
    if escaped:
        raise ValueError(f"{path}:{line_number}: trailing escape")
    return "".join(out)


def kconfig_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\r", "\\r") + '"'


def existing_config(path: Path) -> dict[str, str]:
    if not path.exists():
        return {}
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = re.match(r"^(CONFIG_[A-Z0-9_]+)=(.*)$", line)
        if not match or match.group(1) not in KEYS.values():
            continue
        raw = match.group(2)
        try:
            values[match.group(1)] = json.loads(raw) if raw.startswith('"') else raw
        except json.JSONDecodeError:
            values[match.group(1)] = raw
    return values


def configure(env_file: Path, sdkconfig: Path) -> bool:
    file_values = parse_env(env_file)
    current = existing_config(sdkconfig)
    values = {}
    for name, config_key in KEYS.items():
        if config_key in os.environ:
            value = os.environ[config_key]
        elif name in os.environ:
            value = os.environ[name]
        elif name in file_values:
            value = file_values[name]
        elif config_key in current:
            value = current[config_key]
        else:
            value = DEFAULTS.get(name, "")
        values[config_key] = value

    original = sdkconfig.read_text(encoding="utf-8") if sdkconfig.exists() else ""
    lines = original.splitlines(keepends=True)
    found = set()
    output = []
    for line in lines:
        match = re.match(r"^(CONFIG_[A-Z0-9_]+)=", line)
        if match and match.group(1) in values:
            key = match.group(1)
            output.append(f"{key}={kconfig_string(values[key])}\n")
            found.add(key)
        else:
            output.append(line)
    for key, value in values.items():
        if key not in found:
            output.append(f"{key}={kconfig_string(value)}\n")
    rendered = "".join(output)
    if rendered == original and sdkconfig.exists() and (sdkconfig.stat().st_mode & 0o777) == 0o600:
        return False
    sdkconfig.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=f".{sdkconfig.name}.", dir=sdkconfig.parent)
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            handle.write(rendered)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, sdkconfig)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    return True


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--env-file", type=Path, default=root / ".env.local")
    parser.add_argument("--sdkconfig", type=Path, default=root / "sdkconfig")
    args = parser.parse_args()
    configure(args.env_file, args.sdkconfig)


if __name__ == "__main__":
    main()
