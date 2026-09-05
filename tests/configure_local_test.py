import os
import stat
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from scripts.configure_local import configure, parse_env


class ConfigureLocalTest(unittest.TestCase):
    def test_quotes_escaping_and_preservation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            env = root / ".env.local"
            cfg = root / "sdkconfig"
            env.write_text("FOLLOWUP_OPENROUTER_API_KEY='q\\\"$`\\\\'\nFOLLOWUP_WIFI_STA_SSID=home # note\n")
            cfg.write_text("CONFIG_OTHER=value\nCONFIG_FOLLOWUP_WIFI_STA_SSID=\"old\"\n")
            configure(env, cfg)
            text = cfg.read_text()
            self.assertIn('CONFIG_OTHER=value\n', text)
            self.assertIn('CONFIG_FOLLOWUP_OPENROUTER_API_KEY="q\\\\\\\"$`\\\\\\\\"', text)
            self.assertEqual(stat.S_IMODE(cfg.stat().st_mode), 0o600)

    def test_environment_overrides_file(self):
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {"FOLLOWUP_OPENROUTER_TEXT_MODEL": "env/model"}):
            root = Path(tmp)
            (root / ".env.local").write_text("FOLLOWUP_OPENROUTER_TEXT_MODEL=file/model\n")
            cfg = root / "sdkconfig"
            configure(root / ".env.local", cfg)
            self.assertIn('CONFIG_FOLLOWUP_OPENROUTER_TEXT_MODEL="env/model"', cfg.read_text())

    def test_missing_inputs_preserve_existing_values(self):
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {}, clear=True):
            root = Path(tmp)
            env = root / ".env.local"
            cfg = root / "sdkconfig"
            env.write_text("\n")
            cfg.write_text(
                'CONFIG_FOLLOWUP_WIFI_STA_SSID="kept-wifi"\n'
                'CONFIG_FOLLOWUP_WIFI_STA_PASSWORD="kept-pass"\n'
                'CONFIG_FOLLOWUP_OPENROUTER_API_KEY="kept-key"\n'
            )
            configure(env, cfg)
            text = cfg.read_text()
            self.assertIn('CONFIG_FOLLOWUP_WIFI_STA_SSID="kept-wifi"', text)
            self.assertIn('CONFIG_FOLLOWUP_WIFI_STA_PASSWORD="kept-pass"', text)
            self.assertIn('CONFIG_FOLLOWUP_OPENROUTER_API_KEY="kept-key"', text)

    def test_double_quoted_value_can_end_with_backslash(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / ".env.local"
            path.write_text('FOLLOWUP_WIFI_STA_PASSWORD="ends-with-two\\\\"\n')
            self.assertEqual(parse_env(path)["FOLLOWUP_WIFI_STA_PASSWORD"], "ends-with-two\\")

    def test_parser_error_does_not_write(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            env = root / ".env.local"
            env.write_text("broken\n")
            with self.assertRaises(ValueError):
                parse_env(env)
            self.assertFalse((root / "sdkconfig").exists())


if __name__ == "__main__":
    unittest.main()
