#!/usr/bin/env python3
"""Build only the native display lab; no ESP-IDF environment required."""
import shutil
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parent.parent
out = root / 'emulator/build'
out.mkdir(exist_ok=True)
compiler = shutil.which('clang++') or shutil.which('c++')
if not compiler:
    raise SystemExit('Install a C++ compiler (Xcode Command Line Tools on macOS).')
includes = ['components/epaper_ui/include', 'components/epaper_ui',
            'components/design_tokens/include', 'components/project_assets']
sources = [root / 'emulator/native/renderer.cpp']
for directory in ('components/epaper_ui', 'components/project_assets'):
    sources += sorted((root / directory).glob('*.cpp'))
subprocess.run([compiler, '-std=c++20', '-O2'] + ['-I' + str(root / x) for x in includes]
               + [str(p) for p in sources] + ['-o', str(out / 'followup-renderer')], check=True)
subprocess.run([compiler, '-std=c++17', '-O2', '-Wall', '-Wextra',
               '-I' + str(root / 'emulator/driver/include'),
               '-I' + str(root / 'components/epaper_panel/include'),
               str(root / 'components/epaper_panel/epaper_panel.cpp'),
               str(root / 'components/epaper_panel/ssd1677_driver.cpp'),
               str(root / 'emulator/driver/driver_trace.cpp'), '-o', str(out / 'driver-trace')], check=True)
print('Built actual UI renderer and SSD1677 driver host binaries.')
