from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parent


class ContractTests(unittest.TestCase):
    def test_no_model_in_ctest(self):
        self.assertNotIn('--execute-fp16-combined', (ROOT / 'CMakeLists.txt').read_text())

    def test_fixed_scan_extent(self):
        text = (ROOT / 'finite_fields.h').read_text()
        self.assertIn('p.x < 921600', text)
        self.assertEqual(3600 * 256, 5 * 320 * 576)
        self.assertIn('0x7f800000u', text)
        self.assertIn('D3D12_RANGE read{0,4}', text)

    def test_source_pair_not_caller_supplied(self):
        text = (ROOT / 'resident_texture_run.h').read_text()
        self.assertIn('bridge.Original(0), bridge.Original(1)', text)
        self.assertNotIn('ID3D12Resource* first', text)

    def test_snapshot_before_resize(self):
        text = (ROOT / 'interop/gpu_producer_bridge.h').read_text()
        self.assertLess(text.index('deferred->CopyResource(originals[n].Get()'),
                        text.index('resize[n].Resize(deferred.Get(), originalViews[n].Get())'))

    def test_reject_before_publication(self):
        text = (ROOT / 'resident_texture_run.h').read_text()
        self.assertLess(text.index('finite.CheckFinite'), text.index('handoff.Submit'))
        self.assertIn('throw std::domain_error', text)

    def test_no_cpu_flow_or_pixels_in_candidate(self):
        for name in ('resident_texture_run.h', 'plain_texture.h', 'texture_handoff.h'):
            text = (ROOT / name).read_text()
            self.assertNotIn('->Map(', text)
            self.assertNotIn('D3D12_HEAP_TYPE_READBACK', text)

    def test_explicit_hash_before_device_creation(self):
        text = (ROOT / 'combined_test.cpp').read_text()
        self.assertIn('const bool model=argc==4', text)
        self.assertLess(text.index('modelBytes=LoadPinned'), text.index('D3D12CreateDevice'))


if __name__ == '__main__':
    unittest.main()