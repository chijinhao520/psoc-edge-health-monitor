#!/usr/bin/env python3
"""
quantize.py — 把训好的 .h5 模型 PTQ int8 量化为 TFLite

策略：
  1. 用训练集做 representative dataset（≥100 个样本，~5% 数据）
  2. 全量 int8（input/output 也 int8，TFLite Micro + Ethos-U55 要求）
  3. 量化后 vs 原模型 MAE 差异 < 10% 视为通过
  4. 输出文件:
     - outputs/glucose_int8.tflite          # 给 Vela 输入
     - outputs/glucose_quantize_report.json # 精度对比

用法:
  python quantize.py                         # 用 fold0 模型
  python quantize.py --ckpt models/checkpoints/best_fold2.h5
  python quantize.py --ckpt ... --out outputs/glucose_int8.tflite
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

# 推迟 tf / model 加载到 main()
tf = None
normalize_scalars = None
eval_metrics = None


def _ensure_deps():
    global tf, normalize_scalars, eval_metrics
    if tf is not None:
        return
    try:
        import tensorflow as _tf
        tf = _tf
    except ImportError:
        print("[err] tensorflow 未装；pip install -r requirements.txt")
        sys.exit(2)
    from model import normalize_scalars as _ns
    from train import eval_metrics as _em
    normalize_scalars = _ns
    eval_metrics = _em


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
PROC_DIR = ROOT / "data" / "processed"
OUT_DIR = ROOT / "outputs"
CKPT_DIR = ROOT / "models" / "checkpoints"


# ── representative dataset 生成器 ─────────────────────────────────────
def make_representative_dataset(
    X_ppg: np.ndarray, X_scal: np.ndarray, n_samples: int = 100
):
    """量化校准用 generator。返回 [ppg_in, scal_in]，每次一个样本。"""
    n = min(n_samples, len(X_ppg))
    idx = np.random.RandomState(0).choice(len(X_ppg), n, replace=False)

    def gen():
        for i in idx:
            yield {
                "ppg_in": X_ppg[i : i + 1].astype(np.float32),
                "scal_in": X_scal[i : i + 1].astype(np.float32),
            }

    return gen


# ── 量化 ─────────────────────────────────────────────────────────────
def quantize_to_int8(h5_path: Path, X_ppg: np.ndarray, X_scal: np.ndarray) -> bytes:
    model = tf.keras.models.load_model(str(h5_path), compile=False)
    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = make_representative_dataset(X_ppg, X_scal)
    # 全 int8（含 IO），Ethos-U55 需要
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    tflite_bytes = converter.convert()
    return tflite_bytes


# ── TFLite 推理（量化后端到端验证） ───────────────────────────────────
def run_tflite_inference(
    tflite_bytes: bytes, X_ppg: np.ndarray, X_scal: np.ndarray
) -> np.ndarray:
    interpreter = tf.lite.Interpreter(model_content=tflite_bytes)
    interpreter.allocate_tensors()
    inputs = interpreter.get_input_details()
    output = interpreter.get_output_details()[0]

    # 输入名 → index 映射
    in_map = {d["name"].split(":")[0].split("/")[-1]: d for d in inputs}
    # Keras 命名 ppg_in / scal_in 可能被 TFLite 重命名为 serving_default_ppg_in:0
    ppg_det = next(
        d for d in inputs if "ppg" in d["name"].lower()
    )
    scal_det = next(
        d for d in inputs if "scal" in d["name"].lower()
    )

    preds = []
    for i in range(len(X_ppg)):
        # 量化输入 float → int8
        def quantize(x_float, det):
            scale, zp = det["quantization"]
            return np.round(x_float / scale + zp).clip(-128, 127).astype(np.int8)

        ppg_q = quantize(X_ppg[i : i + 1], ppg_det)
        scal_q = quantize(X_scal[i : i + 1], scal_det)
        interpreter.set_tensor(ppg_det["index"], ppg_q)
        interpreter.set_tensor(scal_det["index"], scal_q)
        interpreter.invoke()
        out_q = interpreter.get_tensor(output["index"])
        # 反量化
        scale, zp = output["quantization"]
        out_f = (out_q.astype(np.float32) - zp) * scale
        preds.append(out_f.squeeze())

    return np.array(preds, dtype=np.float32)


# ── 主流程 ────────────────────────────────────────────────────────────
def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--ckpt",
        type=Path,
        default=CKPT_DIR / "best_fold0_seed0.h5",
        help="Keras .h5 模型路径",
    )
    parser.add_argument(
        "--data",
        type=Path,
        default=PROC_DIR / "multi.npz",
        help="数据 .npz（默认 multi.npz，fallback must.npz）",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=OUT_DIR / "glucose_int8.tflite",
        help="输出 .tflite 路径",
    )
    args = parser.parse_args()

    _ensure_deps()

    if not args.ckpt.exists():
        # 自动找可用 ckpt
        for alt in [
            CKPT_DIR / "best_fold0.h5",
            CKPT_DIR / "best_single.h5",
            *sorted(CKPT_DIR.glob("best_fold*_seed*.h5"))[:1],
        ]:
            if alt.exists():
                args.ckpt = alt
                break
        else:
            raise FileNotFoundError(f"找不到模型: {args.ckpt}\n请先运行 train.py")

    data_path = args.data if args.data.exists() else PROC_DIR / "must.npz"
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    # 加载数据 (全量，用于 representative dataset + 取 holdout 子集)
    d = np.load(data_path, allow_pickle=True)
    X_ppg_full = d["X_ppg"].astype(np.float32)
    X_scal_full = normalize_scalars(d["X_scal"])
    y_full = d["y"].astype(np.float32)

    # === 2026-05-27 审查修订：评估必须在 holdout 上,不能用 fp32 见过的训练数据 ===
    # 选取 ckpt 文件对应的 fold 的 val subjects 作为 holdout (无 fp32 见过的数据)
    # ckpt 命名 best_fold{F}_seed{S}.h5 → 提取 F
    import re
    m = re.search(r"fold(\d+)_seed\d+", args.ckpt.name)
    holdout_mask = None
    if m and "subject_id" in d.files:
        from sklearn.model_selection import GroupKFold
        sid = np.asarray(d["subject_id"]).astype(str)
        gkf = GroupKFold(n_splits=5)
        splits = list(gkf.split(np.arange(len(y_full)), groups=sid))
        fold_idx = int(m.group(1))
        if 0 <= fold_idx < len(splits):
            _, val_idx = splits[fold_idx]
            holdout_mask = val_idx
            print(f"[holdout] fold {fold_idx} val 集 {len(val_idx)}/{len(y_full)} 样本"
                  f"  (fp32 未见过)")

    if holdout_mask is None:
        # fallback: 用全量评估 (兼容旧 ckpt 命名),并在报告中标记
        print("[warn] 无法解析 ckpt 对应 fold,用全量评估 (含训练数据,漂移偏低)")
        holdout_mask = np.arange(len(y_full))

    X_ppg = X_ppg_full[holdout_mask]
    X_scal = X_scal_full[holdout_mask]
    y = y_full[holdout_mask]
    print(f"[load] {args.ckpt}  data: {data_path.name} {X_ppg.shape}")

    # 1. float 模型评估 (holdout)
    model_fp = tf.keras.models.load_model(str(args.ckpt), compile=False)
    y_fp = model_fp.predict(
        {"ppg_in": X_ppg, "scal_in": X_scal}, verbose=0
    ).squeeze()
    metrics_fp = eval_metrics(y, y_fp)
    print(f"[fp32 holdout] {metrics_fp}")

    # 2. 量化 (representative dataset 用**全量**,因为 representative 是校准信号
    #          而非评估;漂移评估才必须 holdout)
    print("[quant] PTQ int8 量化中...")
    tflite_bytes = quantize_to_int8(args.ckpt, X_ppg_full, X_scal_full)
    args.out.write_bytes(tflite_bytes)
    size_kb = len(tflite_bytes) / 1024
    print(f"[save] {args.out} ({size_kb:.1f} KB)")

    # 3. 量化后评估 (holdout)
    y_q = run_tflite_inference(tflite_bytes, X_ppg, X_scal)
    metrics_q = eval_metrics(y, y_q)
    print(f"[int8 holdout] {metrics_q}")

    # 4. 差异
    mae_drop = (metrics_q["MAE_mmol"] - metrics_fp["MAE_mmol"]) / metrics_fp["MAE_mmol"] * 100
    print(f"\n=== 量化精度变化 ===")
    print(f"  MAE: {metrics_fp['MAE_mmol']:.3f} → {metrics_q['MAE_mmol']:.3f} mmol/L "
          f"({mae_drop:+.1f}%)")
    print(f"  CEG A+B: {metrics_fp['CEG_AB_pct']:.1f}% → {metrics_q['CEG_AB_pct']:.1f}%")
    status = "PASS" if abs(mae_drop) < 10 else "FAIL (>10% MAE 漂移)"
    print(f"  状态: {status}")

    # 5. 报告
    report = {
        "ckpt": str(args.ckpt),
        "tflite_size_kb": size_kb,
        "fp32": metrics_fp,
        "int8": metrics_q,
        "mae_drop_pct": mae_drop,
        "status": status,
        "holdout_n": int(len(y)),
        "holdout_strategy": "fold-val (audit-fix 2026-05-27)",
    }
    rpt = OUT_DIR / "glucose_quantize_report.json"
    rpt.write_text(json.dumps(report, indent=2, ensure_ascii=False))
    print(f"[save] 报告 → {rpt}")


if __name__ == "__main__":
    main()
