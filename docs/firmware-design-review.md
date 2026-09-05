# Firmware design review and experiment results

Reviewed 2026-09-05. Followup baseline: `f0b56e969ba3cf7a9d1bed0a77472337abd07ee4`.
CrossPoint checkout: `233f93ffb01e8e8a4f42b232361cda574263f416` on `develop`.
Its FreeInk SDK: `cb9167d541c0f6e9d57cf8eae1f564a939883ecc`.

## Conclusion

Preserve Followup's native renderer and dedicated display bus. Improve the boundary
between UI state, refresh decisions, and hardware completion. A wholesale rewrite
or a copied waveform is not supported by the evidence.

The work produced a runnable native display lab, protocol tests, and a local
credentials workflow. Refresh-driver production behavior was deliberately retained
so the lab can expose and compare its current entry points.

## What the host experiment proves

The same home frame was submitted ten times after a full base:

- Direct `RefreshPartialFullScreen()`: nine partial activations and one additional
  full activation, despite no pixel change.
- Existing `RefreshChangedRegion()`: all ten requests skipped; no additional
  activation and no SPI image payload.

This uses the real C++ driver with mocked I/O. It establishes a redundant-command
path, not measured ghosting or electrical timing. The display lab shows the exact
native page pixels beside both traces. It also verifies selection changes,
repeatable overlays, pixel-exact PNG export, and all ten supported pages.

## Strong parts of the current design

- The e-paper panel owns SPI3. SD uses SDMMC; a generic shared-SPI diagnosis is
  inappropriate for this board.
- Drawing components already separate page composition, widgets, fonts, and
  assets from panel I/O. They compile on the host without redesigning the UI.
- The driver tracks a prior image, writes both SSD1677 planes, and establishes a
  base after wake/reset.
- UI surface batching and a one-slot display queue already exist. Extend these
  rather than adding another scheduler.

## Confirmed display weaknesses

1. No-op suppression is only in one entry point. Put final image comparison ahead
   of all routine activations and counter increments. Preserve explicit cleanup,
   recovery, and initial-base requirements.
2. Page navigation and large-overlay dismissal frequently request full refresh.
   Select routine partial versus maintenance cleanup centrally instead.
3. `MergeRefreshMode()` returns full when either request is full and partial in
   every other case. This loses a fast request; the mode contract needs a defined
   precedence or fast should be removed from unsupported merge paths.
4. The eight-partial limit counts activations, not changed area. First remove
   no-op activations. Only then calibrate a wider or area-aware cleanup budget.
5. Refresh errors need a defined baseline-validity contract. A partially completed
   drive must not leave later work assuming that the old RAM/glass shadow is valid.

## What to borrow from CrossPoint

The [CrossPoint repository](https://github.com/crosspoint-reader/crosspoint-reader/tree/233f93ffb01e8e8a4f42b232361cda574263f416)
separates activities, rendering, and FreeInk hardware support. Its SDK makes old
frame ownership, initial cleanup, and board-specific waveform choices explicit.
Read the pinned `freeink-sdk` source referenced by the checkout, especially
`libs/hardware/BoardConfig`, `Ssd1677Driver`, and `FreeInkDisplay`.

Useful patterns are semantic navigation actions; an activity-owned view state;
stable list selection/scroll state; explicit redraw readiness; board configuration
outside the common driver; and a completion boundary before reusing a framebuffer.
Its `displayWindow()` also restores the controller baseline after a completed
window update in the relevant single-buffer path.

CrossPoint's Sticky profile is a useful SSD1677 comparison, not the Waveshare
board definition. Pins, power handling, SPI rate, orientation, and LUT voltage tails
must stay board-specific. The X4 incremental `0x1C` shortcut is explicitly unsuitable
as a blanket replacement for the vendor partial sequence. This is why copying an
apparently faster driver can make this panel flash more often.

The [Waveshare reference](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/01_E-Paper_Example/components/epaper_port/epaper_port.c)
uses full `0xF7`, fast `0xD7`, and partial `0xFF` commands. Windowed RAM writes and
optical refresh coverage are separate concerns. Old/new planes must agree outside
pending changes. Temperature and panel-film behavior also constrain custom LUTs.

## Recommended ownership model

Input events update one activity/view state. The compositor draws the latest
complete state. One refresh policy compares against the last confirmed frame and
chooses skip, differential, or maintenance refresh. One driver owns the panel and
its RAM baseline. Completion advances that baseline; failure invalidates it.

Overlay composition belongs in the same final frame. A short bounded batching
window can merge a user action's page/footer/status updates without drawing each
intermediate state. It must not drop final input or defer feedback indefinitely.

Start cleanup accounting with real changed frames, then evaluate changed area and
repeated transitions per tile. Keep a conservative global limit during calibration.
A tile score is a heuristic, not a ghosting sensor. Windowed transfer and custom
LUT experiments should follow correctness, not precede it.

## Broader system review candidates

The source audit identified codec handle lifetime across blocking reads and close,
independent cue/clip ownership of I2S output, abort-on-allocation-failure paths, and
storage recovery coordination as high-value follow-ups. These require focused
reproduction and ownership fixes; the display emulator does not prove those races.
Process-lifetime tasks and bounded queues are not inherently defective. Their
shutdown and saturation contracts need to be stated before introducing reload or
recovery features.

## Audio and local setup

Seven feedback assets now use MIT-licensed Cuelume-derived recipes with explicit
attribution. They are deterministic offline approximations of browser synthesis,
not byte-identical Cuelume recordings. Navigation is a short tick; longer tones
remain for completion and startup. Existing audio runtime ownership is unchanged.

`.env.local` and generated `sdkconfig` remain local and ignored. The setup script
applies specified values without evaluating shell text or clearing unspecified
credentials. No real OpenRouter key is stored in source.

No relevant Potato Mode skill was found in installed skills or the public search.
The similarly named result was a journaling skill, not firmware/emulator tooling.
Ponytail remains the applicable simplicity workflow.

## Remaining physical validation

Use a repeatable key sequence, fixed lighting and camera placement, and driver
source tags. Compare full/partial counts, input-to-display latency, contrast, and
ghosting. Test wake, repeated highlights, distant changed regions, and large
overlay dismissal. The local display lab cannot certify any of these optical or
hardware results. No new firmware was flashed in this work because the USB device
was absent.
