import struct
import tempfile
import unittest
from pathlib import Path
from audit_combined import pixel_metrics, field_metrics, audit_artifacts


class AuditTests(unittest.TestCase):
    def test_pixels(self):
        self.assertEqual(pixel_metrics(bytes([1, 2, 3, 255]), bytes([1, 2, 4, 255]), 4), 1)

    def test_extent(self):
        for extent in (0, 3, 4):
            with self.assertRaises(ValueError):
                pixel_metrics(b'', b'', extent)
        for count in (0, 1):
            with self.assertRaises(ValueError):
                field_metrics(b'', b'', count)

    def test_alpha(self):
        with self.assertRaises(ValueError):
            pixel_metrics(bytes(4), bytes(4), 4)

    def test_fields(self):
        value = struct.pack('<2f', 0, 1)
        self.assertEqual(field_metrics(value, value, 2), 0)

    def test_nonfinite(self):
        for value in (float('nan'), float('inf'), -float('inf')):
            for a, b in ((value, 0), (0, value)):
                with self.assertRaises(ValueError):
                    field_metrics(struct.pack('<f', a), struct.pack('<f', b), 1)

    def test_tolerance(self):
        with self.assertRaises(ValueError):
            field_metrics(struct.pack('<f', 0.001), struct.pack('<f', 0), 1)
        self.assertLessEqual(field_metrics(struct.pack('<f', 0.00009), struct.pack('<f', 0), 1), 1e-4)

    def test_missing_collection(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(FileNotFoundError):
                audit_artifacts(Path(directory))


if __name__ == '__main__':
    unittest.main()