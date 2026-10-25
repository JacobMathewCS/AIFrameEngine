"""Regenerate the tiny CPU-only ORT buffer regression fixture (requires onnx)."""
from pathlib import Path
import onnx
from onnx import TensorProto, helper

def main():
    shape = [1, 3, 4, 8]
    tensor = lambda name: helper.make_tensor_value_info(name, TensorProto.FLOAT, shape)
    graph = helper.make_graph([helper.make_node("Add", ["img0", "img1"], ["output"])],
                              "buffer_reuse", [tensor("img0"), tensor("img1")], [tensor("output")])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)], ir_version=8)
    onnx.checker.check_model(model)
    path = Path(__file__).resolve().parent / "fixtures" / "rife_io_add.onnx"
    path.parent.mkdir(exist_ok=True)
    onnx.save(model, path)
    print(path)

if __name__ == "__main__":
    main()