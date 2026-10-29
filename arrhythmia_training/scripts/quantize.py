#!/usr/bin/env python3
"""
quantize.py — 把训好的 .h5 双分支模型 PTQ int8 量化为 TFLite

策略：
  1. 用训练集做 representative dataset（双输入 ecg_in + rr_in）
  2. 全量 int8（input/output 也 int8，TFLite Micro + Ethos-U55 要求）
  3. 量化前后在 DS2(inter-patient) 上比 macro-F1，漂移 < 5% 视为通过
  4. 输出:
     - outputs/arrhythmia_int8.tflite          # 给 Vela 输入
     - outputs/arrhythmia_quantize_report.json # 精度对比

用法:
  python quantize.py                              # 用 best.h5
  python quantize.py --ckpt models/checkpoints/best.h5
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
NPZ = ROOT / "data" / "processed" / "mitbih.npz"
CKPT = ROOT / "models" / "checkpoints" / "best.h5"
OUT_TFLITE = ROOT / "outputs" / "arrhythmia_int8.tflite"
REPORT = ROOT / "outputs" / "arrhythmia_quantize_report.json"


def _load_split(keep_q: bool):
    d = np.load(NPZ, allow_pickle=True)
    classes = [str(c) for c in d["classes"]]
    Xtr, Rtr = d["X_train"].astype(np.float32), d["RR_train"].astype(np.float32)
    ytr = d["y_train"]
    Xte, Rte = d["X_test"].astype(np.float32), d["RR_test"].astype(np.float32)
    yte = d["y_test"]
    if not keep_q and "Q" in classes:
        q = classes.index("Q")
        Xtr, Rtr, ytr = Xtr[ytr != q], Rtr[ytr != q], ytr[ytr != q]
        Xte, Rte, yte = Xte[yte != q], Rte[yte != q], yte[yte != q]
        classes = [c for c in classes if c != "Q"]
    return Xtr, Rtr, ytr, Xte, Rte, yte, classes


def make_representative_dataset(X, R, n=300):
    n = min(n, len(X))
    idx = np.random.RandomState(0).choice(len(X), n, replace=False)

    def gen():
        for i in idx:
            yield {
                "ecg_in": X[i:i+1].astype(np.float32),
                "rr_in": R[i:i+1].astype(np.float32),
            }
    return gen


def quantize_to_int8(tf, h5_path, X, R) -> bytes:
    model = tf.keras.models.load_model(str(h5_path), compile=False)
    conv = tf.lite.TFLiteConverter.from_keras_model(model)
    conv.optimizations = [tf.lite.Optimize.DEFAULT]
    conv.representative_dataset = make_representative_dataset(X, R)
    conv.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    conv.inference_input_type = tf.int8
    conv.inference_output_type = tf.int8
    return conv.convert()


def tflite_predict(tf, tflite_bytes, X, R) -> np.ndarray:
    interp = tf.lite.Interpreter(model_content=tflite_bytes)
    interp.allocate_tensors()
    ins = interp.get_input_details()
    out = interp.get_output_details()[0]
    ecg_d = next(d for d in ins if "ecg" in d["name"].lower())
    rr_d = next(d for d in ins if "rr" in d["name"].lower())

    def q(x, det):
        s, zp = det["quantization"]
        return np.round(x / s + zp).clip(-128, 127).astype(np.int8)

    preds = []
    for i in range(len(X)):
        interp.set_tensor(ecg_d["index"], q(X[i:i+1], ecg_d))
        interp.set_tensor(rr_d["index"], q(R[i:i+1], rr_d))
        interp.invoke()
        preds.append(int(interp.get_tensor(out["index"])[0].argmax()))
    return np.array(preds)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt", type=Path, default=CKPT)
    ap.add_argument("--keep-q", action="store_true")
    args = ap.parse_args()

    import tensorflow as tf
    from sklearn.metrics import f1_score, classification_report

    if not args.ckpt.exists():
        sys.exit(f"[err] 找不到 {args.ckpt}，先跑 train.py")

    Xtr, Rtr, ytr, Xte, Rte, yte, classes = _load_split(args.keep_q)
    print(f"[load] {args.ckpt}  test {Xte.shape} classes {classes}")

    # fp32 评估（DS2）
    model = tf.keras.models.load_model(str(args.ckpt), compile=False)
    yp_fp = model.predict({"ecg_in": Xte, "rr_in": Rte}, verbose=0).argmax(1)
    f1_fp = f1_score(yte, yp_fp, average="macro")
    print(f"[fp32] macro-F1 = {f1_fp:.4f}")

    # 量化
    print("[quant] PTQ int8 …")
    tfl = quantize_to_int8(tf, args.ckpt, Xtr, Rtr)
    OUT_TFLITE.parent.mkdir(parents=True, exist_ok=True)
    OUT_TFLITE.write_bytes(tfl)
    size_kb = len(tfl) / 1024
    print(f"[save] {OUT_TFLITE} ({size_kb:.1f} KB)")

    # int8 评估（DS2）
    yp_q = tflite_predict(tf, tfl, Xte, Rte)
    f1_q = f1_score(yte, yp_q, average="macro")
    print(f"[int8] macro-F1 = {f1_q:.4f}")

    drop = (f1_fp - f1_q) / f1_fp * 100 if f1_fp > 0 else 0
    status = "PASS" if drop < 5 else f"WARN (>{5}% F1 漂移)"
    print(f"\n=== 量化精度变化 ===")
    print(f"  macro-F1: {f1_fp:.4f} → {f1_q:.4f} ({-drop:+.1f}%)  {status}")
    print("[int8] 每类 F1:")
    print(classification_report(yte, yp_q, target_names=classes,
                                digits=4, zero_division=0))

    rep_fp = classification_report(yte, yp_fp, target_names=classes,
                                   output_dict=True, zero_division=0)
    rep_q = classification_report(yte, yp_q, target_names=classes,
                                  output_dict=True, zero_division=0)
    REPORT.write_text(json.dumps({
        "ckpt": str(args.ckpt),
        "tflite_size_kb": size_kb,
        "classes": classes,
        "fp32_macro_f1": float(f1_fp),
        "int8_macro_f1": float(f1_q),
        "f1_drop_pct": float(drop),
        "status": status,
        "fp32_per_class": {c: rep_fp[c] for c in classes},
        "int8_per_class": {c: rep_q[c] for c in classes},
        "n_test": int(len(yte)),
    }, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"[save] 报告 → {REPORT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
