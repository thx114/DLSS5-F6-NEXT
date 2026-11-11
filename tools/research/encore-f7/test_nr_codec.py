import json
import math
from pathlib import Path
import struct
import unittest

import nr_codec as n


class NrCodecTests(unittest.TestCase):
    def test_sdr_is_clamped_display_color_not_gamma_conversion(self):
        self.assertEqual(n.encode_color((-0.5, 0.5, 2.0), hdr=False), (0.0, 0.5, 1.0))

    def test_nonfinite_original_components_become_zero(self):
        self.assertEqual(n.encode_color((math.nan, math.inf, -math.inf), hdr=False), (0, 0, 0))

    def test_gamma_roundtrip_and_low_branch(self):
        for value in (0, 0.001, n.LINEAR_SWITCH, 0.1, 0.75, 1):
            self.assertAlmostEqual(n.encoded_to_linear(n.linear_to_encoded(value)), value, delta=3e-7)
        self.assertEqual(n.linear_to_encoded(0.001), 0.001*n.LINEAR_SCALE)

    def test_hdr_knee_preserves_chromatic_ratios(self):
        encoded = n.encode_color((4, 2, 1), hdr=True)
        linear = tuple(n.encoded_to_linear(c) for c in encoded)
        self.assertGreater(linear[0], 0.99)
        self.assertLessEqual(linear[0], 1.000001)
        self.assertAlmostEqual(linear[0]/linear[1], 2, delta=2e-6)
        self.assertAlmostEqual(linear[1]/linear[2], 2, delta=2e-6)

    def test_hdr_zero_residual_preserves_original_highlights(self):
        self.assertEqual(n.reconstruct_color((8, 2, 0.25), (0, 0, 0), hdr=True), (8, 2, 0.25))

    def test_hdr_gain_limits(self):
        high = n.reconstruct_color((0.01, 0.01, 0.01), (1, 1, 1), hdr=True)
        low = n.reconstruct_color((0.01, 0.01, 0.01), (-1, -1, -1), hdr=True)
        self.assertEqual(high, (0.04,)*3)
        self.assertEqual(low, (0.0025,)*3)

    def test_sdr_residual_is_added_then_saturated(self):
        self.assertEqual(n.reconstruct_color((0.4, 0.6, 0.1), (0.1, -1, 2), hdr=False), (0.5, 0, 1))

    def test_zero_residual_donors(self):
        taps = ((0.1, 0.2, 0.3),)*4
        self.assertEqual(n.weighted_residual(taps[0], taps, taps, (0.3, 0.7)), (0, 0, 0))

    def test_same_raster_sdr_reconstruction_equals_clamped_neural_rgb(self):
        original = (0.1, 0.5, 0.9)
        proxy = n.encode_color(original, hdr=False)
        neural = (0.2, 0.3, 1.1)
        delta = n.weighted_residual(proxy, (proxy,)*4, (neural,)*4, (0, 0))
        actual = n.reconstruct_color(original, delta, hdr=False)
        for a, b in zip(actual, (0.2, 0.3, 1.0), strict=True):
            self.assertAlmostEqual(a, b, places=14)

    def test_edge_affinity_suppresses_dissimilar_donor(self):
        proxy = ((0, 0, 0), (1, 1, 1), (0, 0, 0), (0, 0, 0))
        neural = ((0.1,)*3, (1.9,)*3, (0, 0, 0), (0, 0, 0))
        for channel in n.weighted_residual((0, 0, 0), proxy, neural, (0.5, 0)):
            self.assertGreater(channel, 0.1)
            self.assertLess(channel, 0.102)

    def test_exposure_image_validation_and_floor(self):
        self.assertEqual(n.effective_exposure(2, 0.25), 0.5)
        for sample in (None, 0, -1, math.nan, math.inf):
            self.assertEqual(n.effective_exposure(2, sample), 2)
        self.assertEqual(n.effective_exposure(1e-10), n.EXPOSURE_FLOOR)

    def test_box_footprint_and_coverage(self):
        self.assertEqual(n.box_axis(4, 2, 0), ((0, 1), (1, 1)))
        self.assertEqual(n.box_axis(3, 2, 1), ((1, 0.5), (2, 1)))
        self.assertEqual(n.box_axis(2, 4, 0), ((0, 0.5),))
        for output in range(1, 12):
            for work in range(1, 12):
                area = sum(w for i in range(work) for _, w in n.box_axis(output, work, i))
                self.assertAlmostEqual(area, output, places=12)

    def test_invalid_reference_inputs_fail(self):
        for call in (lambda: n.effective_exposure(0), lambda: n.effective_exposure(math.nan),
                     lambda: n.encode_color((1, 2, 3), hdr=1),
                     lambda: n.encode_color((n.FLOAT32_MAX, 0, 0), hdr=True, exposure=2),
                     lambda: n.box_axis(0, 2, 0), lambda: n.box_axis(2, 2, True),
                     lambda: n.reconstruct_color((1, 1, 1), (math.nan, 0, 0), hdr=True)):
            with self.assertRaises(ValueError): call()

    def test_recovered_literals_match_instruction_bytes(self):
        root = Path(__file__).parent/'evidence/nr-codec'
        samples = [('nr-encode', 0x64fc, 0xc1b8aa3b), ('nr-encode', 0x65c0, 0x3b4d2e1c),
                   ('nr-decode', 0x2278, 0xc29044fe), ('nr-decode', 0x22a8, 0x3a83126f),
                   ('nr-decode', 0x29f4, 0x4019999a)]
        for name, offset, bits in samples:
            rows = json.loads((root/(name+'.tokens.json')).read_text())
            row = next(r for r in rows if int(r['offset'], 16) == offset)
            self.assertIn(struct.pack('<I', bits), bytes.fromhex(row['bytes']))


if __name__ == '__main__':
    unittest.main()
