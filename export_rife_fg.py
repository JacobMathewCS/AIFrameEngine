"""
Export RIFE_trained.onnx at 576x320 for real-time frame generation.

Why 576x320:
- Both axes divisible by 64 (576=64*9, 320=64*5) for RIFE's multi-scale flow pyramid
- Benchmark before claiming real-time operation; the full capture/display path costs more.
- Still large enough to preserve meaningful motion detail

Output:
    models/RIFE_trained_576x320.onnx
    build/Debug/models/RIFE_trained_576x320.onnx
    build/Release/models/RIFE_trained_576x320.onnx
"""

from pathlib import Path
from urllib.request import urlretrieve
import shutil
import sys
import tempfile
import zipfile

import onnx
import onnxsim
import torch
import torch.nn as nn

ROOT = Path(__file__).resolve().parent
MODELS = ROOT / "models"
BUILD_DESTS = [ROOT / "build" / config / "models" for config in ("Debug", "Release")]
OUTPUT = MODELS / "RIFE_trained_576x320.onnx"

CHECKPOINT_URL = (
    "https://drive.usercontent.google.com/download"
    "?id=1APIzVeI-4ZZCEuIRE1m6WYfSCaOsi_7_&export=download&confirm=t"
)
SOURCE_URL = "https://github.com/hzwer/ECCV2022-RIFE/archive/refs/heads/main.zip"

# Fixed processing resolution for frame generation
RIFE_HEIGHT = 320
RIFE_WIDTH = 576


class ExportWrapper(nn.Module):
    def __init__(self, flownet: nn.Module) -> None:
        super().__init__()
        self.flownet = flownet

    def forward(self, img0: torch.Tensor, img1: torch.Tensor) -> torch.Tensor:
        images = torch.cat((img0, img1), dim=1)
        _, _, merged = self.flownet(images, [4.0, 2.0, 1.0])
        return merged[2]


def download_inputs(work: Path):
    source_zip = work / "rife-source.zip"
    checkpoint_zip = work / "rife-trained.zip"
    if not source_zip.exists():
        print("Downloading official RIFE source ...")
        urlretrieve(SOURCE_URL, source_zip)
    if not checkpoint_zip.exists():
        print("Downloading official trained RIFE checkpoint ...")
        urlretrieve(CHECKPOINT_URL, checkpoint_zip)

    source_root = work / "source"
    checkpoint_root = work / "checkpoint"
    if not source_root.exists():
        source_root.mkdir()
        with zipfile.ZipFile(source_zip) as archive:
            archive.extractall(source_root)
    if not checkpoint_root.exists():
        checkpoint_root.mkdir()
        with zipfile.ZipFile(checkpoint_zip) as archive:
            archive.extractall(checkpoint_root)

    source_candidates = list(source_root.glob("ECCV2022-RIFE-*/"))
    checkpoint_path = checkpoint_root / "train_log"
    if not source_candidates or not (checkpoint_path / "flownet.pkl").exists():
        raise RuntimeError("The downloaded RIFE source or checkpoint archive is incomplete.")
    return source_candidates[0], checkpoint_path


def export_raw(work: Path) -> Path:
    source_root, checkpoint_root = download_inputs(work)
    sys.path.insert(0, str(source_root))
    sys.path.insert(0, str(checkpoint_root.parent))

    from train_log.RIFE_HDv3 import Model

    model = Model()
    model.load_model(str(checkpoint_root), -1)
    model.eval()
    wrapper = ExportWrapper(model.flownet).cpu().eval()

    sample0 = torch.zeros(1, 3, RIFE_HEIGHT, RIFE_WIDTH)
    sample1 = torch.zeros(1, 3, RIFE_HEIGHT, RIFE_WIDTH)
    raw_output = work / "RIFE_trained_576x320_raw.onnx"

    torch.onnx.export(
        wrapper,
        (sample0, sample1),
        raw_output,
        export_params=True,
        opset_version=18,
        do_constant_folding=True,
        input_names=["img0", "img1"],
        output_names=["output"],
        dynamo=False,
    )
    print(f"  raw export: {raw_output}")
    return raw_output


def simplify_static(raw_path: Path, work: Path) -> Path:
    """Fold the shape chains into constants using the known fixed input size."""
    print("Running onnxsim with fixed input shapes ...")
    model = onnx.load(str(raw_path))
    input_shapes = {
        "img0": [1, 3, RIFE_HEIGHT, RIFE_WIDTH],
        "img1": [1, 3, RIFE_HEIGHT, RIFE_WIDTH],
    }
    try:
        simplified, ok = onnxsim.simplify(model, input_shapes=input_shapes)
    except TypeError:
        simplified, ok = onnxsim.simplify(model)
    if not ok:
        raise RuntimeError("onnxsim validation failed; refusing to publish the model")
    sim_path = work / "RIFE_trained_576x320_sim.onnx"
    onnx.save(simplified, str(sim_path))
    print(f"  simplified: {sim_path}")
    return sim_path


def validate(model_path: Path) -> None:
    model = onnx.load(str(model_path))
    onnx.checker.check_model(model, full_check=True)
    if [t.name for t in model.graph.input] != ["img0", "img1"] or [t.name for t in model.graph.output] != ["output"]:
        raise RuntimeError("Unexpected model input/output names")
    for tensor in list(model.graph.input) + list(model.graph.output):
        shape = [dim.dim_value for dim in tensor.type.tensor_type.shape.dim]
        if shape != [1, 3, RIFE_HEIGHT, RIFE_WIDTH]:
            raise RuntimeError(f"Unexpected shape for {tensor.name}: {shape}")

    dyn = 0
    for tensor in list(model.graph.input) + list(model.graph.output):
        for dim in tensor.type.tensor_type.shape.dim:
            if dim.dim_param:
                dyn += 1
    print(f"  inputs: {[t.name for t in model.graph.input]}")
    print(f"  outputs: {[t.name for t in model.graph.output]}")
    print(f"  dynamic axes remaining: {dyn}")
    print(f"  nodes: {len(model.graph.node)}  initializers: {len(model.graph.initializer)}")


def main() -> None:
    MODELS.mkdir(exist_ok=True)
    for dest in BUILD_DESTS:
        dest.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="aiframe-rife-fg-") as tmp:
        work = Path(tmp)
        raw_path = export_raw(work)
        sim_path = simplify_static(raw_path, work)

        validate(sim_path)

        shutil.copy2(sim_path, OUTPUT)
        for dest in BUILD_DESTS:
            shutil.copy2(sim_path, dest / OUTPUT.name)
        print("\n[OK] Frame generation RIFE model written to:")
        print(f"   {OUTPUT}")
        for dest in BUILD_DESTS:
            print(f"   {dest / OUTPUT.name}")
        print(f"Fixed size: {RIFE_WIDTH}x{RIFE_HEIGHT}")


if __name__ == "__main__":
    main()