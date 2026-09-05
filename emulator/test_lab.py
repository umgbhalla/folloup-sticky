#!/usr/bin/env python3
"""Host integration checks; no board, network service, or paid API required."""
import json
import struct
import subprocess
import unittest
import zlib
from pathlib import Path

from server import Lab, ROOT, png, read_pbm


class NativeLabTests(unittest.TestCase):
    def setUp(self):
        self.lab = Lab(ROOT / 'build/followup-renderer', ROOT / 'build/driver-trace')
        self.addCleanup(self.lab.temp.cleanup)

    def test_unchanged_requests_run_real_driver(self):
        for _ in range(10):
            self.lab.action({'action': 'same'})
        direct = self.lab.state['direct']['totals']
        changed = self.lab.state['changed']['totals']
        self.assertEqual((direct['full'], direct['partial'], direct['noop']), (2, 9, 0))
        self.assertEqual((changed['full'], changed['partial'], changed['noop']), (1, 0, 10))
        self.assertEqual(self.lab.state['changed_pixels'], 0)

    def test_all_pages_and_focus(self):
        for screen in self.lab.screens:
            with self.subTest(screen=screen['id']):
                self.lab.action({'action': 'screen', 'screen': screen['id']})
                bits = read_pbm(self.lab.history[-1])
                self.assertTrue(any(bits))
                self.assertFalse(all(x == 255 for x in bits))
                if screen['id'] in ('home', 'settings', 'wifi', 'notes', 'todos', 'followup'):
                    self.lab.action({'action': 'down'})
                    self.assertGreater(self.lab.state['changed_pixels'], 0)

    def test_overlay_restores_exact_frame(self):
        base = read_pbm(self.lab.history[-1])
        for overlay in ('toast', 'keyboard'):
            self.lab.action({'action': 'overlay', 'overlay': overlay})
            self.assertNotEqual(base, read_pbm(self.lab.history[-1]))
            self.lab.action({'action': 'overlay', 'overlay': 'none'})
            self.assertEqual(base, read_pbm(self.lab.history[-1]))

    def test_png_preserves_every_pixel(self):
        bits = read_pbm(self.lab.history[-1])
        data = png(bits)
        self.assertTrue(data.startswith(b'\x89PNG\r\n\x1a\n'))
        offset, compressed = 8, b''
        while offset < len(data):
            size = struct.unpack('!I', data[offset:offset + 4])[0]
            kind = data[offset + 4:offset + 8]
            payload = data[offset + 8:offset + 8 + size]
            crc = struct.unpack('!I', data[offset + 8 + size:offset + 12 + size])[0]
            self.assertEqual(crc, zlib.crc32(kind + payload))
            if kind == b'IDAT':
                compressed += payload
            offset += size + 12
        rows = zlib.decompress(compressed)
        self.assertEqual(len(rows), 800 * 61)
        restored = b''.join(bytes(x ^ 255 for x in rows[y * 61 + 1:(y + 1) * 61]) for y in range(800))
        self.assertEqual(bits, restored)

    def test_text_and_invalid_inputs(self):
        self.lab.action({'action': 'screen', 'screen': 'notes'})
        before = read_pbm(self.lab.history[-1])
        self.lab.action({'action': 'text', 'text': 'A "quoted" note\nSecond line. $HOME `literal`'})
        self.assertNotEqual(before, read_pbm(self.lab.history[-1]))
        for data in ({'action': 'screen', 'screen': '../secret'},
                     {'action': 'overlay', 'overlay': 'bad'},
                     {'action': 'text', 'text': 'x' * 501}, {'action': 'unknown'}):
            with self.assertRaises(ValueError):
                self.lab.action(data)

    def test_driver_error_scenarios(self):
        for scenario in ('baseline', 'noop', 'eight_then_full', 'busy_timeout', 'equivalence'):
            completed = subprocess.run([self.lab.driver, scenario], check=True, capture_output=True, text=True)
            self.assertIn('scenario', json.loads(completed.stdout))


if __name__ == '__main__':
    unittest.main(verbosity=2)
