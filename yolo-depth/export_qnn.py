"""Export YOLO26n-depth to a Qualcomm QNN context binary for QCS8550 (Hexagon V73)."""

import argparse

from ultralytics import YOLO


def main() -> int:
    """Export the depth model to QNN format. Returns 0 on success."""
    parser = argparse.ArgumentParser(
        description="Export YOLO26-depth to QNN context binary for QCS8550.",
        epilog="Example: uv run python export_qnn.py --weights weights/yolo26n-depth.pt --data depth8.yaml",
    )
    parser.add_argument("--weights", default="weights/yolo26n-depth.pt", help="path to .pt weights")
    parser.add_argument("--data", default="depth8.yaml", help="calibration dataset YAML (W8A16 needs calibration)")
    parser.add_argument("--imgsz", type=int, default=768, help="inference image size")
    parser.add_argument("--target", default="73", help="Hexagon HTP arch: 73 = Snapdragon 8 Gen 2 / QCS8550")
    parser.add_argument("--fraction", type=float, default=1.0, help="fraction of calibration set to use")
    args = parser.parse_args()

    model = YOLO(args.weights)
    path = model.export(
        format="qnn",
        imgsz=args.imgsz,
        name=args.target,
        data=args.data,
        fraction=args.fraction,
    )
    print(f"\nEXPORTED: {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
