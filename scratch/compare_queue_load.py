"""Bounded ABBA queue experiment. Never stops the game or an existing compositor.

A: synthetic inference with game running. B: same plus capture/upscale, F9 OFF.
With --display-cache, both A and B run capture/upscale: A cache OFF, B ON.
Not a total FG benchmark: B excludes reconstruction and dynamic captured inputs.
Foreground/occlusion, heat and game-scene changes remain possible confounders.
"""
import argparse
import csv
import ctypes
from ctypes import wintypes
import datetime
import json
import math
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent
LOG = Path('C:/Temp/aiframe_compositor.log')


def process_exists(name):
    result = subprocess.run(['tasklist', '/FI', f'IMAGENAME eq {name}', '/FO', 'CSV', '/NH'],
                            capture_output=True, text=True, check=True, timeout=15)
    return any(row and row[0].lower() == name.lower() for row in csv.reader(result.stdout.splitlines()))


def close_owned(process):
    if process is None or process.poll() is not None:
        return
    user = ctypes.WinDLL('user32', use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]

    @callback_type
    def close_window(hwnd, unused):
        pid = wintypes.DWORD()
        user.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value == process.pid:
            user.PostMessageW(hwnd, 0x312, 1, 0)  # compositor's graceful quit
        return True

    user.EnumWindows(close_window, 0)
    try:
        process.wait(timeout=15)
    except subprocess.TimeoutExpired:
        process.terminate()  # only the process this experiment created
        process.wait(timeout=10)


def summarize(path):
    with path.open(newline='') as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        raise RuntimeError('No benchmark samples')
    summary = {'samples': len(rows), 'missed_slots': int(rows[-1]['missed_slots'])}
    for key in ('wall_ms', 'host_run_ms', 'finish_wait_ms', 'inference_queue_ms'):
        values = sorted(float(row[key]) for row in rows)
        if any(not math.isfinite(v) or v < 0 for v in values):
            raise ValueError('Invalid timing samples')
        summary[key] = {'median': statistics.median(values),
                        'p95': values[math.ceil(.95 * len(values)) - 1],
                        'max': values[-1], 'over_50ms': sum(v >= 50 for v in values)}
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, default=20)
    parser.add_argument('--adapter', type=int, default=1)
    parser.add_argument('--display-cache', action='store_true',
                        help='Run compositor in all trials: A cache off, B cache on; F9 stays off')
    args = parser.parse_args()
    if not 2 <= args.seconds <= 60 or args.adapter < 0:
        parser.error('seconds must be 2..60 and adapter nonnegative')
    if process_exists('AIFrameEngineCompositor.exe') or process_exists('DmlQueueBench.exe'):
        raise RuntimeError('Close existing compositor/queue benchmark first')
    if not process_exists('witcher3.exe'):
        raise RuntimeError('Witcher must be running for all four trials')
    bench = ROOT / 'build/Release/DmlQueueBench.exe'
    app = ROOT / 'build/Release/AIFrameEngineCompositor.exe'
    model = ROOT / 'models/RIFE_flow_576x320_fp16.onnx'
    for path in (bench, app, model):
        if not path.exists():
            raise FileNotFoundError(path)
    folder = ROOT / 'scratch' / (('cache_abba_' if args.display_cache else 'queue_abba_') + datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
    folder.mkdir()
    print(folder, flush=True)
    results = []
    for trial in ('A1', 'B1', 'B2', 'A2'):
        compositor = gpu = None
        if not process_exists('witcher3.exe'):
            raise RuntimeError('Game exited; comparison invalid')
        print('Starting ' + trial, flush=True)
        with (folder / f'{trial}_gpu.csv').open('w') as telemetry:
            try:
                if args.display_cache or trial.startswith('B'):
                    if LOG.exists():
                        LOG.rename(folder / f'{trial}_previous_compositor.log')
                    env = os.environ.copy()
                    env['AIFRAME_RIFE_FP16'] = '1'
                    env['AIFRAME_DML_DEVICE'] = str(args.adapter)
                    env['AIFRAME_DISPLAY_CACHE'] = '1' if args.display_cache and trial.startswith('B') else '0'
                    compositor = subprocess.Popen([str(app)], cwd=app.parent, env=env)
                    deadline = time.monotonic() + 60
                    while time.monotonic() < deadline:
                        if compositor.poll() is not None:
                            raise RuntimeError('Compositor exited during startup')
                        text = LOG.read_text(errors='replace') if LOG.exists() else ''
                        if 'RIFE ready:' in text and 'First captured Witcher frame received.' in text:
                            break
                        time.sleep(.25)
                    else:
                        raise TimeoutError('Capture/model readiness timed out')
                time.sleep(3)
                smi = shutil.which('nvidia-smi')
                if smi:
                    gpu = subprocess.Popen([smi, '--query-gpu=timestamp,index,utilization.gpu,temperature.gpu,clocks.current.graphics,power.draw,memory.used',
                        '--format=csv', '-lms', '500'], stdout=telemetry, stderr=subprocess.STDOUT)
                with (folder / f'{trial}_bench.log').open('w') as status:
                    subprocess.run([str(bench), str(model), str(args.adapter), str(args.seconds),
                                    str(folder / f'{trial}.csv')], stdout=status, stderr=subprocess.STDOUT,
                                   check=True, timeout=args.seconds + 90)
                if compositor and compositor.poll() is not None:
                    raise RuntimeError('Compositor exited during measurement')
            finally:
                if gpu is not None and gpu.poll() is None:
                    gpu.terminate()
                    gpu.wait(timeout=10)
                close_owned(compositor)
                if compositor and LOG.exists():
                    shutil.copy2(LOG, folder / f'{trial}_compositor.log')
            if args.display_cache or trial.startswith('B'):
                text = (folder / f'{trial}_compositor.log').read_text(errors='replace')
                if 'RIFE frame generation enabled' in text:
                    raise RuntimeError('F9 was enabled; B trial invalid')
                if args.display_cache:
                    expected = 'Display cache: enabled' if trial.startswith('B') else 'Display cache: disabled'
                    if expected not in text:
                        raise RuntimeError('Display cache configuration was not applied')
            results.append({'trial': trial, **summarize(folder / f'{trial}.csv')})
            (folder / 'summary.json').write_text(json.dumps(results, indent=2))
            print(json.dumps(results[-1]), flush=True)
    print('COMPLETE: four trials; game untouched; owned compositor closed.', flush=True)


if __name__ == '__main__':
    main()