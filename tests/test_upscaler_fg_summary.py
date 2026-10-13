import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scratch'))
from compare_upscaler_fg import COUNTERS, parse_stats, summarize, validate_log


def stats(count):
    fields = dict.fromkeys(COUNTERS, count)
    fields.update(worker_ms=20, service_ema_ms=25, playback_delay_ms=48, present_cpu_ms=1,
                  preprocess='gpu', precision='16', display_mode='0',
                  reconstruction='full-res-flow', pacing='waitable', admission='normal')
    return fields


class UpscalerSummaryTest(unittest.TestCase):
    def records(self):
        return [{'received_seconds': t, 'stats': stats(n)} for t, n in [(10, 100), (12, 160), (14, 220)]]

    def test_counter_delta_excludes_warmup(self):
        result = summarize(self.records())
        self.assertEqual(result['generated_submissions_per_second'], 30)
        self.assertEqual(result['counter_deltas']['generated_presented'], 120)
        self.assertEqual(result['worker_ms']['p95'], 20)

    def test_invalid_or_empty_measurement(self):
        for records in ([], self.records()[:2]):
            with self.assertRaises(ValueError):
                summarize(records)
        for key in ('received_seconds', 'counter', 'no_work'):
            records = self.records()
            if key == 'received_seconds':
                records[1][key] = records[0][key]
            elif key == 'counter':
                records[1]['stats']['completed'] = 0
            else:
                for row in records:
                    row['stats']['generated_presented'] = 100
            with self.assertRaises(ValueError):
                summarize(records)

    def test_parser_and_configuration(self):
        fields = stats(20)
        def line(values):
            return 'FG STATS: ' + ' '.join(f'{k}={v}' for k, v in values.items())
        self.assertEqual(parse_stats(line(fields))['completed'], 20)
        self.assertIsNone(parse_stats('Captured frame heartbeat: 120.'))
        for key, value in [('worker_ms', 'nan'), ('worker_ms', -1),
                           ('completed', '1.2'), ('precision', 32), ('display_mode', 1),
                           ('reconstruction', 'legacy-image'), ('pacing', 'legacy')]:
            bad = copy.copy(fields)
            bad[key] = value
            with self.assertRaises(ValueError):
                parse_stats(line(bad))

    def test_pipeline_confirmation_and_toggles(self):
        base = ('First captured Witcher frame received.\n'
                'RIFE ready: precision=fp16; warmup complete\n'
                'Display cache: enabled\nRIFE frame generation enabled\n'
                'UPSCALE CHECK: bicubic=on sharpness=0.600000\n')
        fsr = 'Upscaler: reference FSR1 EASU/RCAS enabled (SDR).\n'
        validate_log(base, False)
        validate_log(base + fsr, True)
        for text, mode in [(base, True), (base + fsr, False),
                           (base + 'Upscaling: bilinear', False),
                           (base + 'RIFE frame generation enabled', False),
                           (base + 'session failed', False)]:
            with self.assertRaises(ValueError):
                validate_log(text, mode)


if __name__ == '__main__':
    unittest.main()