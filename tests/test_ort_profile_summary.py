import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("summary", Path(__file__).resolve().parents[1] / "scratch" / "summarize_ort_profile.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class SummaryTest(unittest.TestCase):
    def test_containment_and_units(self):
        result = module.summarize([
            dict(name="model_run", ph="X", ts=100, dur=200000),
            dict(name="a_kernel_time", cat="Node", ph="X", ts=200, dur=1000,
                 args={"provider": "DmlExecutionProvider"}),
            dict(name="outside_kernel_time", cat="Node", ph="X", ts=300000, dur=99999),
            dict(name="fence_before", cat="Node", ph="X", ts=100, dur=9000)])
        row = result["slowest_runs"][0]
        self.assertEqual(row["run_ms"], 200)
        self.assertEqual(row["node_host_sum_ms"], 1)
        self.assertEqual(len(row["largest_node_events"]), 1)
    def test_empty(self):
        self.assertEqual(module.summarize([])["run_count_including_warmup"], 0)

if __name__ == "__main__":
    unittest.main()