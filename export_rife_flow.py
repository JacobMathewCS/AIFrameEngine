"""Export HDv3 final flow (xy for A/B) and sigmoid mask, without changing legacy models.

Uses the same checkpoint/source as export_rife_fg.py. --work can reuse an export cache.
Publishes only after CPU reconstruction parity and DirectML inference validation pass.
"""
import argparse
from pathlib import Path
import shutil
import sys
import tempfile
import time

import numpy as np
import onnx
import onnxruntime as ort
import onnxsim
import torch
from export_rife_fg import download_inputs, ROOT, RIFE_HEIGHT as H, RIFE_WIDTH as W


class FlowWrapper(torch.nn.Module):
    def __init__(self, net):
        super().__init__()
        self.net = net

    def forward(self, a, b):
        flows, mask, _ = self.net(torch.cat((a, b), 1), [4., 2., 1.])
        return torch.cat((flows[-1], mask), 1)


def run(work, device):
    torch.set_num_threads(4)
    source, checkpoint = download_inputs(work)
    sys.path[:0] = [str(source), str(checkpoint.parent)]
    from train_log.RIFE_HDv3 import Model
    from model.warplayer import warp
    model = Model()
    model.load_model(str(checkpoint), -1)
    model.eval()
    net = model.flownet.cpu().eval()
    wrapper = FlowWrapper(net).eval()
    raw = work / 'flow_raw.onnx'
    torch.onnx.export(wrapper, (torch.zeros(1, 3, H, W), torch.zeros(1, 3, H, W)),
                      str(raw), opset_version=18, dynamo=False,
                      input_names=['img0', 'img1'], output_names=['flow_mask'])
    graph, ok = onnxsim.simplify(onnx.load(str(raw)))
    if not ok:
        raise RuntimeError('Simplification failed')
    onnx.checker.check_model(graph, full_check=True)
    path = work / 'RIFE_flow_576x320.onnx'
    onnx.save(graph, str(path))
    opts = ort.SessionOptions()
    opts.enable_mem_pattern = False
    opts.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
    opts.add_session_config_entry('session.disable_cpu_ep_fallback', '1')
    dml = ort.InferenceSession(str(path), sess_options=opts,
                              providers=[('DmlExecutionProvider', {'device_id': device})])
    cpu = ort.InferenceSession(str(path), providers=['CPUExecutionProvider'])
    rng = np.random.default_rng(42)
    # Random texture, identical frames, translated edges, and independent images.
    base = rng.random((1, 3, H, W), dtype=np.float32)
    edges = np.zeros_like(base)
    edges[:, :, 40:240, 100:300] = 0.8
    cases = [(base, base), (edges, np.roll(edges, 8, axis=3)),
             (base, np.roll(base, 5, axis=3)), (base, rng.random(base.shape, dtype=np.float32))]
    timings = []
    for i, (a, b) in enumerate(cases):
        with torch.no_grad():
            ta, tb = torch.from_numpy(a), torch.from_numpy(b)
            tf, tm, merged = net(torch.cat((ta, tb), 1), [4., 2., 1.])
            exact = warp(ta, tf[-1][:, :2]) * tm + warp(tb, tf[-1][:, 2:4]) * (1 - tm)
            if not torch.equal(exact, merged[-1]):
                raise RuntimeError('Checkpoint is not exactly flow/mask reconstruction')
            f = cpu.run(None, {'img0': a, 'img1': b})[0]
            start = time.perf_counter()
            g = dml.run(None, {'img0': a, 'img1': b})[0]
            timings.append((time.perf_counter() - start) * 1000)
            def reconstruct(data):
                t = torch.from_numpy(data)
                return warp(ta, t[:, :2]) * t[:, 4:5] + warp(tb, t[:, 2:4]) * (1 - t[:, 4:5])
            ref = merged[-1]
            cpu_error = (reconstruct(f) - ref).abs().max().item()
            cpu_mean = (reconstruct(f) - ref).abs().mean().item()
            dml_error = (reconstruct(g) - ref).abs().mean().item()
            print(f'case={i} cpu_max={cpu_error:.7f} cpu_mean={cpu_mean:.7f} dml_mean={dml_error:.7f} dml_ms={timings[-1]:.2f}', flush=True)
            # Independent random images amplify tiny flow rounding differences on
            # high-frequency noise. Bound both peak (<~1.3/255) and mean error.
            if f.shape != (1, 5, H, W) or not np.isfinite(f).all() or not np.isfinite(g).all() or cpu_error > 0.005 or cpu_mean > 0.0002 or dml_error > 0.001:
                raise RuntimeError('Flow reconstruction validation failed')
            if i == 1:
                # Independent PyTorch oracle consumed by the D3D11 WARP shader test.
                fixtures = ROOT / 'scratch' / 'flow_fixture'
                fixtures.mkdir(exist_ok=True)
                a.astype('<f4').tofile(fixtures / 'a.bin')
                b.astype('<f4').tofile(fixtures / 'b.bin')
                g.astype('<f4').tofile(fixtures / 'fields.bin')
                reconstruct(g).numpy().astype('<f4').tofile(fixtures / 'expected.bin')
    for directory in (ROOT / 'models', ROOT / 'build/Release/models', ROOT / 'build/Debug/models'):
        directory.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, directory / path.name)
    print('[OK] Published validated flow model; legacy image model unchanged.', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--work', type=Path)
    parser.add_argument('--device', type=int, default=1)
    args = parser.parse_args()
    if args.work:
        args.work.mkdir(parents=True, exist_ok=True)
        run(args.work, args.device)
    else:
        with tempfile.TemporaryDirectory(prefix='aiframe-flow-') as tmp:
            run(Path(tmp), args.device)