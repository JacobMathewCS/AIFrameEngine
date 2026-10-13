import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('queue_comparison',
    Path(__file__).resolve().parents[1] / 'scratch/compare_queue_load.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class QueueSummaryTest(unittest.TestCase):
    def write(self, folder, rows):
        path = Path(folder) / 'timings.csv'
        path.write_text('wall_ms,host_run_ms,finish_wait_ms,inference_queue_ms,missed_slots\n' + rows)
        return path

    def test_percentiles_and_cumulative_misses(self):
        with tempfile.TemporaryDirectory() as folder:
            path = self.write(folder, '10,1,8,7,0\n20,2,16,14,1\n90,3,80,75,4\n')
            result = module.summarize(path)
            self.assertEqual(result['samples'], 3)
            self.assertEqual(result['missed_slots'], 4)
            self.assertEqual(result['wall_ms'], {'median': 20, 'p95': 90, 'max': 90, 'over_50ms': 1})

    def test_empty_and_invalid(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(RuntimeError):
                module.summarize(self.write(folder, ''))
            for value in ('nan', '-1', 'inf'):
                with self.assertRaises(ValueError):
                    module.summarize(self.write(folder, f'{value},1,2,3,0\n'))


if __name__ == '__main__':
    unittest.main()