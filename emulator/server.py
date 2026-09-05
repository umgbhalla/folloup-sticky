#!/usr/bin/env python3
"""Local display lab: real firmware rendering and real driver replay; mocked services."""
import argparse
import json
import struct
import subprocess
import tempfile
import threading
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parent


def read_pbm(path):
    data = Path(path).read_bytes()
    # Native renderer owns this exact P4 format; reject other formats explicitly.
    header = b'P4\n480 800\n'
    if not data.startswith(header) or len(data) != len(header) + 48000:
        raise ValueError('Expected native 480 x 800 P4 framebuffer')
    return data[len(header):]


def png(bits):
    def chunk(kind, body):
        return struct.pack('!I', len(body)) + kind + body + struct.pack('!I', zlib.crc32(kind + body))
    # PBM 1 means black; PNG grayscale 1 means white.
    rows = b''.join(b'\0' + bytes(v ^ 255 for v in bits[y * 60:(y + 1) * 60]) for y in range(800))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('!2I5B', 480, 800, 1, 0, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def pixel_diff(old, new):
    mask = bytes(a ^ b for a, b in zip(old, new))
    changed = sum(x.bit_count() for x in mask)
    positions = [i for i, x in enumerate(mask) if x]
    if not positions:
        return changed, None, mask
    xs, ys = [], []
    for i in positions:
        for bit in range(8):
            if mask[i] & (128 >> bit):
                xs.append(i % 60 * 8 + bit)
                ys.append(i // 60)
    return changed, [min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1], mask


class Lab:
    def __init__(self, renderer, driver):
        self.renderer, self.driver = str(renderer), str(driver)
        self.temp = tempfile.TemporaryDirectory(prefix='followup-display-lab-')
        self.lock = threading.RLock()
        self.screens = json.loads(subprocess.check_output([self.renderer, '--list']))
        if isinstance(self.screens, dict):
            self.screens = self.screens['screens']
        self.counts = {s['id']: s['selections'] for s in self.screens}
        self.reset()

    def reset(self):
        for path in Path(self.temp.name).glob('*.pbm'):
            path.unlink()
        self.history = []
        self.state = dict(screen='home', selection=0, overlay='none', text='Review the display design with the team.')
        self.render()

    def replay(self, changed=False):
        command = [self.driver, '--frames'] + (['--changed'] if changed else []) + [str(p) for p in self.history]
        result = subprocess.run(command, capture_output=True, text=True, check=True, timeout=15)
        records = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
        if not records or records[-1].get('event') != 'totals':
            raise RuntimeError('Driver trace did not produce totals')
        return dict(totals=records[-1], events=records[:-1][-15:])

    def render(self):
        if len(self.history) >= 128:
            raise ValueError('This replay has 128 frames. Reset to start another sequence.')
        path = Path(self.temp.name) / f'{len(self.history):04d}.pbm'
        command = [self.renderer, '--screen', self.state['screen'], '--selection', str(self.state['selection']),
                   '--overlay', self.state['overlay'], '--text', self.state['text'], '--output', str(path)]
        subprocess.run(command, capture_output=True, text=True, check=True, timeout=15)
        bits = read_pbm(path)
        previous = read_pbm(self.history[-1]) if self.history else bytes(48000)
        changed, bounds, mask = pixel_diff(previous, bits)
        self.history.append(path)
        self.image = png(bits)
        self.diff_image = png(mask)
        self.state.update(frame=len(self.history), changed_pixels=changed, dirty_bounds=bounds,
                          direct=self.replay(), changed=self.replay(True))

    def action(self, data):
        action = data.get('action')
        if action != 'reset' and len(self.history) >= 128:
            raise ValueError('This replay has 128 frames. Reset to start another sequence.')
        if action == 'reset':
            self.reset()
            return
        if action == 'screen':
            if data.get('screen') not in self.counts:
                raise ValueError('Unknown screen')
            self.state.update(screen=data['screen'], selection=0, overlay='none')
        elif action in ('up', 'down'):
            count = max(1, self.counts[self.state['screen']])
            self.state['selection'] = (self.state['selection'] + (1 if action == 'down' else -1)) % count
        elif action == 'overlay':
            if data.get('overlay') not in ('none', 'toast', 'keyboard'):
                raise ValueError('Unknown overlay')
            self.state['overlay'] = data['overlay']
        elif action == 'text':
            value = data.get('text')
            if not isinstance(value, str) or len(value) > 500:
                raise ValueError('Fixture text must be at most 500 characters')
            self.state['text'] = value
        elif action == 'same':
            pass
        else:
            raise ValueError('Unknown action')
        self.render()


def handler(lab):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def send(self, status, data, kind='application/json'):
            if not isinstance(data, bytes):
                data = json.dumps(data).encode()
            self.send_response(status)
            self.send_header('Content-Type', kind)
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            path = urlsplit(self.path).path
            if path.startswith('/audio/'):
                name = path.removeprefix('/audio/')
                if name in {p.name for p in (ROOT / 'web/audio').glob('*.wav')}:
                    return self.send(200, (ROOT / 'web/audio' / name).read_bytes(), 'audio/wav')
                return self.send(404, {'error': 'Unknown cue'})
            with lab.lock:
                if path == '/api/state':
                    return self.send(200, dict(lab.state, screens=lab.screens))
                if path == '/frame.png':
                    return self.send(200, lab.image, 'image/png')
                if path == '/diff.png':
                    return self.send(200, lab.diff_image, 'image/png')
                if path == '/frame.pbm':
                    return self.send(200, lab.history[-1].read_bytes(), 'image/x-portable-bitmap')
                if path == '/':
                    return self.send(200, (ROOT / 'web/index.html').read_bytes(), 'text/html; charset=utf-8')
            self.send(404, {'error': 'Not found'})

        def do_POST(self):
            if urlsplit(self.path).path != '/api/action':
                return self.send(404, {'error': 'Not found'})
            origin = self.headers.get('Origin')
            if origin and origin != 'http://' + self.headers.get('Host', ''):
                return self.send(403, {'error': 'Same-origin requests only'})
            try:
                length = int(self.headers.get('Content-Length', '0'))
                if not 0 < length <= 4096:
                    raise ValueError('Invalid request length')
                data = json.loads(self.rfile.read(length))
                if not isinstance(data, dict):
                    raise ValueError('Expected an object')
                with lab.lock:
                    lab.action(data)
                    self.send(200, dict(lab.state, screens=lab.screens))
            except (ValueError, TypeError) as error:
                self.send(400, {'error': str(error)})
            except (subprocess.SubprocessError, OSError, RuntimeError) as error:
                self.send(500, {'error': type(error).__name__ + ': native renderer/driver failed; inspect terminal'})
    return Handler


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, default=8765)
    parser.add_argument('--renderer', type=Path, default=ROOT / 'build/followup-renderer')
    parser.add_argument('--driver', type=Path, default=ROOT / 'build/driver-trace')
    args = parser.parse_args()
    lab = Lab(args.renderer, args.driver)
    print(f'Followup display lab: http://127.0.0.1:{args.port}', flush=True)
    ThreadingHTTPServer(('127.0.0.1', args.port), handler(lab)).serve_forever()
