"""Bounded ABBA gameplay-pipeline comparison with FG ON; no game input injection.

A = legacy bicubic/custom sharpening, B = reference EASU/RCAS. This measures
whole pipelines, not isolated scaler cost or physical display FPS. Leave the
game in the same scene, 720p/windowed/30 FPS, without changing hotkeys/settings.
Only owned compositors are closed; existing instances cause refusal.
"""
import argparse
import ctypes
from ctypes import wintypes
import datetime
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import time

from compare_queue_load import close_owned, process_exists

ROOT = Path(__file__).resolve().parents[1]
LOG = Path('C:/Temp/aiframe_compositor.log')
COUNTERS = ('submitted', 'completed', 'generated_presented', 'stale',
            'busy_skips', 'auto_skips', 'expired')
TIMINGS = ('worker_ms', 'service_ema_ms', 'playback_delay_ms', 'present_cpu_ms')


def parse_stats(line):
    if 'FG STATS:' not in line:
        return None
    fields = dict(re.findall(r'(\w+)=([^\s]+)', line.split('FG STATS:', 1)[1]))
    for key in COUNTERS:
        value = fields[key]
        if not value.isdigit():
            raise ValueError('Invalid counter: ' + key)
        fields[key] = int(value)
    for key in TIMINGS:
        value = float(fields[key])
        if not math.isfinite(value) or value < 0:
            raise ValueError('Invalid timing: ' + key)
        fields[key] = value
    expected = {'preprocess': 'gpu', 'precision': '16', 'display_mode': '0',
                'reconstruction': 'full-res-flow', 'pacing': 'waitable'}
    if any(fields.get(k) != v for k, v in expected.items()):
        raise ValueError('Unexpected FG configuration')
    return fields


def summarize(records):
    if len(records) < 3:
        raise ValueError('At least three measured statistics records required')
    for a, b in zip(records, records[1:]):
        if b['received_seconds'] <= a['received_seconds']:
            raise ValueError('Non-increasing receipt timestamps')
        if any(b['stats'][k] < a['stats'][k] for k in COUNTERS):
            raise ValueError('Counter reset: trial invalid')
    elapsed = records[-1]['received_seconds'] - records[0]['received_seconds']
    deltas = {k: records[-1]['stats'][k] - records[0]['stats'][k] for k in COUNTERS}
    if deltas['completed'] == 0 or deltas['generated_presented'] == 0:
        raise ValueError('No completed/presented FG work in measured window')
    result = {'statistics_records': len(records), 'observed_seconds': elapsed,
              'counter_deltas': deltas,
              'generated_submissions_per_second': deltas['generated_presented'] / elapsed,
              'probe_records': sum(r['stats']['admission'] == 'probe' for r in records)}
    for key in TIMINGS:
        values = sorted(r['stats'][key] for r in records)
        result[key] = {'median': statistics.median(values),
                       'p95': values[math.ceil(.95 * len(values)) - 1], 'max': values[-1]}
    return result


def post_enable(pid):
    user = ctypes.WinDLL('user32', use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    sent = []

    @callback_type
    def visit(hwnd, unused):
        owner = wintypes.DWORD()
        user.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid:
            if not user.PostMessageW(hwnd, 0x312, 2, 0):
                return False
            sent.append(True)
            return False  # Toggle once, never send game keypresses.
        return True

    user.EnumWindows(visit, 0)
    if not sent:
        raise RuntimeError('Could not enable owned compositor')


def validate_log(text, fsr):
    required = ['First captured Witcher frame received.', 'precision=fp16; warmup complete',
                'Display cache: enabled', 'RIFE frame generation enabled',
                'bicubic=on', 'sharpness=0.600000']
    if fsr:
        required.append('Upscaler: reference FSR1 EASU/RCAS enabled (SDR).')
    elif 'Upscaler: reference FSR1 EASU/RCAS enabled' in text:
        raise ValueError('Unexpected FSR pipeline in A trial')
    if any(token not in text for token in required):
        raise ValueError('Capture/pipeline configuration not confirmed in log')
    if text.count('RIFE frame generation enabled') != 1:
        raise ValueError('Unexpected FG toggles')
    forbidden = ('RIFE frame generation disabled', 'Upscaling:', 'Upscale sharpen:',
                 'UPSCALE EXPORT:', 'bicubic=off', 'falling back', 'failed')
    if any(token.lower() in text.lower() for token in forbidden):
        raise ValueError('Settings changed or failure/fallback recorded; inspect trial log')


def run_trial(app, folder, trial, seconds, warmup):
    fsr = trial.startswith('B')
    if not process_exists('witcher3.exe'):
        raise RuntimeError('Witcher is not running; no valid gameplay comparison')
    if process_exists('AIFrameEngineCompositor.exe'):
        raise RuntimeError('Existing compositor detected; refusing to close it')
    if LOG.exists():
        shutil.copy2(LOG, folder / (trial + '_previous.log'))
        LOG.unlink()
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith('AIFRAME_')}
    env.update(AIFRAME_RIFE_FP16='1', AIFRAME_DISPLAY_CACHE='1',
               AIFRAME_FSR1='1' if fsr else '0')
    process = None
    records = []
    started = time.monotonic()
    try:
        process = subprocess.Popen([str(app)], cwd=app.parent, env=env)
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError('Compositor exited during startup')
            text = LOG.read_text(errors='replace') if LOG.exists() else ''
            if 'RIFE ready:' in text and 'First captured Witcher frame received.' in text:
                break
            time.sleep(.2)
        else:
            raise TimeoutError('Capture/model readiness timed out')
        post_enable(process.pid)
        measure_start = time.monotonic() + warmup
        deadline = measure_start + seconds
        # A tail reader preserves partial lines; observer time is NOT GPU/scanout time.
        with LOG.open(encoding='utf-8', errors='replace') as log, \
                (folder / (trial + '_stats.jsonl')).open('w', encoding='utf-8') as output:
            log.seek(0, 2)
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError('Compositor exited during measurement')
                position = log.tell()
                line = log.readline()
                if not line.endswith('\n'):
                    log.seek(position)
                    time.sleep(.05)
                    continue
                now = time.monotonic()
                if now >= measure_start:
                    fields = parse_stats(line)
                    if fields is not None:
                        record = {'received_seconds': now - started, 'stats': fields}
                        records.append(record)
                        output.write(json.dumps(record) + '\n')
                        output.flush()
        if not process_exists('witcher3.exe'):
            raise RuntimeError('Game exited during measurement')
    finally:
        close_owned(process)
        if LOG.exists():
            shutil.copy2(LOG, folder / (trial + '_compositor.log'))
    validate_log(LOG.read_text(errors='replace'), fsr)
    return {'trial': trial, 'pipeline': 'EASU/reference RCAS' if fsr else 'bicubic/custom RCAS',
            **summarize(records)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, default=60)
    parser.add_argument('--warmup', type=int, default=10)
    args = parser.parse_args()
    if not 10 <= args.seconds <= 180 or not 3 <= args.warmup <= 60:
        parser.error('seconds must be 10..180 and warmup 3..60')
    app = ROOT / 'build/Release/AIFrameEngineCompositor.exe'
    if not app.is_file():
        raise RuntimeError('Release executable missing')
    if process_exists('AIFrameEngineCompositor.exe'):
        raise RuntimeError('Close the existing compositor first; it will not be stopped')
    if not process_exists('witcher3.exe'):
        raise RuntimeError('Start Witcher in gameplay first; no benchmark was launched')
    folder = ROOT / 'scratch' / ('upscaler_fg_' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
    folder.mkdir()
    report = {'status': 'running', 'seconds_per_trial': args.seconds, 'warmup_seconds': args.warmup,
              'limitations': 'Sequential ABBA; scene, heat, clocks and occlusion not fixed. '
                             'Sampled host metrics and receipt-time rates, not physical FPS. '
                             'Sharpening implementations differ; whole-pipeline comparison.',
              'trials': []}
    path = folder / 'summary.json'
    print(str(folder), flush=True)
    try:
        for trial in ('A1', 'B1', 'B2', 'A2'):
            print('Starting ' + trial, flush=True)
            result = run_trial(app, folder, trial, args.seconds, args.warmup)
            report['trials'].append(result)
            path.write_text(json.dumps(report, indent=2), encoding='utf-8')
            print(json.dumps(result), flush=True)
        report['status'] = 'complete'
    except BaseException as error:
        report['status'] = 'failed'
        report['error'] = str(error)
        raise
    finally:
        path.write_text(json.dumps(report, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()