# Followup display lab

Run locally with Python 3.10+ and a C++ compiler (Xcode Command Line Tools on macOS):

```sh
python3 emulator/build.py
python3 emulator/test_lab.py
python3 emulator/server.py
```

Open http://127.0.0.1:8765. The server binds only to loopback. No ESP-IDF environment,
Python package, Node package, API key, or device is required. An optional CMake
target builds the renderer alone; `build.py` builds both host binaries.

## What runs

- Actual `components/epaper_ui/*.cpp`, design tokens, fonts, and project assets.
- Ten deterministic screen fixtures: home, onboarding, settings, Wi-Fi, notes,
  todos, follow-up, summarize, vibe-check, and details.
- Actual toast and keyboard drawing functions on top of each page.
- Actual `epaper_panel.cpp` and `ssd1677_driver.cpp`, compiled with fake ESP-IDF,
  GPIO, SPI, memory, and time interfaces.
- Exact portrait PBM-to-native mapping: `(x,y)` becomes `(y,479-x)`. Native RAM is
  800 x 480, MSB first, 1 white; exported PBM is 480 x 800, 1 black.

The two replay columns feed identical pixel sequences through the existing direct
partial path and the existing changed-image path. Each replay begins with a full
base. The columns represent driver entry points, not a reconstruction of the
application's complete scheduling behavior. All trace records come from the C++
driver, not a Python copy of its refresh rules.

`Replay identical x10` demonstrates the current policy defect: after the initial
base, the direct path performs nine partial activations and one full activation;
the changed-image path skips all ten requests. GPIO/SPI mocks count submitted
commands, so these are host command counts, not observed physical refreshes.

## Controls

Choose a fixture page, use the arrow keys or Previous/Next to move its selection,
and try a toast or keyboard overlay. Sample text applies to supported note/detail
fixtures. Save PNG exports exact rendered pixels. Show changed pixels displays the
XOR of the last two frames. Experiments are limited to 128 frames before reset.

Cuelume-derived WAV previews are available in the left panel. Audio is off by
default; preview playback requires a user gesture. The navigation option uses a
69 ms tick. Firmware MP3 assets and attribution are under
`components/system_sound_service/sounds/`.

## Scope of proof

This is a native display/driver emulator, not a CPU emulator or a full ESP-IDF app
emulator. Fixture controls do not execute `app_shell` or the production input
coordinator. Clock, Wi-Fi, battery, storage, and note content are fake. Select,
card, and sticky overlays are not yet fixture controls. Hardware sound, microphone,
SD filesystem, network requests, sleep/rails, optical ghosting, contrast, waveform
duration and physical panel behavior require separate device tests.

Host tests cover every listed page, visible selection changes, restoring exact
pixels after supported overlays, PNG bit preservation, malformed input, driver
no-op/budget behavior, and a mocked BUSY timeout. Native renderer sanitizer checks
also exercised all ten pages with the three supported overlay modes.
