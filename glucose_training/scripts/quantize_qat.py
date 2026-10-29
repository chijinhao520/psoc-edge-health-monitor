#!/usr/bin/env python3
"""
quantize_qat.py — Quantization-Aware Training (QAT) int8 量化

vs PTQ：QAT 在训练后期模拟 int8 量化误差并微调权重，通常比 PTQ MAE 漂移降低 30-50%。

流程：
  1. 加载 fp32 best_ckpt
  2. tfmot.quantization.keras.quantize_model 包装为 QAT 模型
  3. fine-tune 10 epoch（lr=1e-5，不太破坏权重）
  4. 转 TFLite int8（IO 也 int8）
  5. 评估 fp32 vs QAT-int8 MAE 漂移
  6. 与 PTQ 对比，写 quant_compare.json，择优产 outputs/glucose_int8.tflite

依赖：
  - tensorflow >= 2.15
  - tensorflow-model-optimization (tfmot)，缺则 fallback：仅产 PTQ 警告

用法:
  python quantize_qat.py --ckpt models/checkpoints/best_fold0_seed0.h5
  python quantize_qat.py --ckpt ... --finetune-epochs 15 --finetune-lr 5e-6
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

# 推迟 import tf / tfmot 到主流程
tf = None
tfmot = None
build_glucose_model = None
normalize_scalars = None
eval_metrics = None


def _ensure_deps():
    global tf, tfmot, build_glucose_model, normalize_scalars, eval_metrics
    if tf is not None:
        return
    try:
        import tensorflow as _tf
        tf = _tf
    except ImportError:
        print("[err] tensorflow 未装；pip install -r requirements.txt")
        sys.exit(2)
    try:
        import tensorflow_model_optimization as _tfmot
        tfmot = _tfmot
    except ImportError:
        tfmot = None
        print("[warn] tensorflow-model-optimization 未装；QAT 不可用，但脚本仍可启动")

    from model import build_glucose_model as _bm, normalize_scalars as _ns
    from train import eval_metrics as _em
    build_glucose_model = _bm
    normalize_scalars = _ns
    eval_metrics = _em


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
PROC_DIR = ROOT / "data" / "processed"
OUT_DIR = ROOT / "outputs"
CKPT_DIR = ROOT / "models" / "checkpoints"


# ── 数据 ──────────────────────────────────────────────────────────────


def load_data():
    multi = PROC_DIR / "multi.npz"
    must = PROC_DIR / "must.npz"
    src = multi if multi.exists() else must
    if not src.exists():
        print(f"[err] 找不到数据 npz")
        sys.exit(2)
    d = np.load(src, allow_pickle=True)
    return (
        d["X_ppg"].astype(np.float32),
        normalize_scalars(d["X_scal"]),
        d["y"].astype(np.float32),
    )


# ── representative dataset（按血糖区间分层）─────────────────────────


def make_representative_dataset(X_ppg, X_scal, y, n_samples: int = 200):
    """分层采样：按血糖区间均匀，比 random 校准更稳。"""
    bins = np.linspace(y.min(), y.max(), 6)
    rng = np.random.RandomState(0)
    samples_per_bin = n_samples // (len(bins) - 1)
    idx_list = []
    for i in range(len(bins) - 1):
        in_bin = np.where((y >= bins[i]) & (y <= bins[i + 1]))[0]
        if len(in_bin) == 0:
            continue
        take = min(samples_per_bin, len(in_bin))
        idx_list.append(rng.choice(in_bin, take, replace=False))
    idx = np.concatenate(idx_list) if idx_list else rng.choice(len(y), n_samples)

    def gen():
        for i in idx:
            yield {
                "ppg_in":  X_ppg[i : i + 1].astype(np.float32),
                "scal_in": X_scal[i : i + 1].astype(np.float32),
            }

    return gen


# ── QAT ───────────────────────────────────────────────────────────────


def run_qat(ckpt_path: Path, X_ppg, X_scal, y,
            finetune_epochs: int = 10, finetune_lr: float = 1e-5,
            batch: int = 16) -> bytes:
    """QAT fine-tune + 转 int8 TFLite。"""
    if tfmot is None:
        print("[err] tfmot 未装，无法跑 QAT；安装 pip install tensorflow-model-optimization")
        sys.exit(3)

    print(f"[qat] 加载 fp32 模型: {ckpt_path}")
    base = tf.keras.models.load_model(str(ckpt_path), compile=False)

    print("[qat] 包装为 QAT 模型...")
    q_model = tfmot.quantization.keras.quantize_model(base)
    q_model.compile(
        optimizer=tf.keras.optimizers.Adam(finetune_lr),
        loss=tf.keras.losses.Huber(delta=1.0),
        metrics=["mae"],
    )

    print(f"[qat] fine-tune {finetune_epochs} epoch (lr={finetune_lr})...")
    q_model.fit(
        {"ppg_in": X_ppg, "scal_in": X_scal}, y,
        epochs=finetune_epochs, batch_size=batch, verbose=0,
    )

    print("[qat] 转 TFLite int8...")
    conv = tf.lite.TFLiteConverter.from_keras_model(q_model)
    conv.optimizations = [tf.lite.Optimize.DEFAULT]
    conv.representative_dataset = make_representative_dataset(X_ppg, X_scal, y)
    conv.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    conv.inference_input_type = tf.int8
    conv.inference_output_type = tf.int8
    return conv.convert()


# ── 端到端 TFLite 推理（与 quantize.py 同接口） ──────────────────────


def run_tflite_inference(tflite_bytes: bytes, X_ppg, X_scal):
    interp = tf.lite.Interpreter(model_content=tflite_bytes)
    interp.allocate_tensors()
    inputs = interp.get_input_details()
    output = interp.get_output_details()[0]
    ppg_det = next(d for d in inputs if "ppg" in d["name"].lower())
    scal_det = next(d for d in inputs if "scal" in d["name"].lower())

    preds = []
    for i in range(len(X_ppg)):
        def q(x, det):
            s, zp = det["quantization"]
            return np.round(x / s + zp).clip(-128, 127).astype(np.int8)
        interp.set_tensor(ppg_det["index"], q(X_ppg[i:i+1], ppg_det))
        interp.set_tensor(scal_det["index"], q(X_scal[i:i+1], scal_det))
        interp.invoke()
        out_q = interp.get_tensor(output["index"])
        s, zp = output["quantization"]
        preds.append((out_q.astype(np.float32) - zp) * s)
    return np.array(preds, dtype=np.float32).squeeze()


# ── PTQ 对比 ──────────────────────────────────────────────────────────


def compare_with_ptq(qat_metrics: dict) -> dict:
    """读 outputs/glucose_quantize_report.json（PTQ 报告）做对比。"""
    ptq_report = OUT_DIR / "glucose_quantize_report.json"
    if not ptq_report.exists():
        return {"ptq_available": False, "chosen": "qat"}

    ptq = json.loads(ptq_report.read_text(encoding="utf-8"))
    ptq_mae = ptq["int8"]["MAE_mmol"]
    qat_mae = qat_metrics["MAE_mmol"]
    chosen = "qat" if qat_mae < ptq_mae else "ptq"
    return {
        "ptq_available": True,
        "ptq_mae": ptq_mae,
        "qat_mae": qat_mae,
        "chosen": chosen,
        "delta_mae": qat_mae - ptq_mae,
    }


# ── 主流程 ────────────────────────────────────────────────────────────


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ckpt", type=Path, default=CKPT_DIR / "best_fold0_seed0.h5",
                        help="fp32 模型 (.h5)")
    parser.add_argument("--out", type=Path, default=OUT_DIR / "glucose_qat_int8.tflite",
                        help="输出 QAT .tflite")
    parser.add_argument("--finetune-epochs", type=int, default=10)
    parser.add_argument("--finetune-lr", type=float, default=1e-5)
    parser.add_argument("--batch", type=int, default=16)
    parser.add_argument("--no-compare", action="store_true",
                        help="跳过与 PTQ 对比")
    args = parser.parse_args()

    _ensure_deps()

    # ckpt 兼容性
    if not args.ckpt.exists():
        for alt in [CKPT_DIR / "best_fold0.h5", CKPT_DIR / "best_single.h5"]:
            if alt.exists():
                args.ckpt = alt
                break
        else:
            print(f"[err] 找不到 {args.ckpt}\n先跑 train.py")
            sys.exit(2)

    OUT_DIR.mkdir(parents=True, exist_ok=True)

    X_ppg, X_scal, y = load_data()
    print(f"[data] N={len(y)}")

    # fp32 评估
    fp32 = tf.keras.models.load_model(str(args.ckpt), compile=False)
    y_fp = fp32.predict({"ppg_in": X_ppg, "scal_in": X_scal}, verbose=0).squeeze()
    m_fp = eval_metrics(y, y_fp)
    print(f"[fp32] MAE={m_fp['MAE_mmol']:.3f}")

    # QAT
    tflite_bytes = run_qat(args.ckpt, X_ppg, X_scal, y,
                           finetune_epochs=args.finetune_epochs,
                           finetune_lr=args.finetune_lr, batch=args.batch)
    args.out.write_bytes(tflite_bytes)
    size_kb = len(tflite_bytes) / 1024
    print(f"[save] {args.out} ({size_kb:.1f} KB)")

    # 评估
    y_qat = run_tflite_inference(tflite_bytes, X_ppg, X_scal)
    m_qat = eval_metrics(y, y_qat)
    print(f"[qat]  MAE={m_qat['MAE_mmol']:.3f}")

    mae_drop = (m_qat["MAE_mmol"] - m_fp["MAE_mmol"]) / m_fp["MAE_mmol"] * 100
    status = "PASS" if abs(mae_drop) < 10 else "FAIL"
    print(f"\n[delta] MAE 漂移 {mae_drop:+.1f}%  → {status}")

    report = {
        "ckpt": str(args.ckpt),
        "tflite_size_kb": size_kb,
        "fp32": m_fp,
        "qat_int8": m_qat,
        "mae_drop_pct": mae_drop,
        "status": status,
    }
    rpt = OUT_DIR / "qat_report.json"
    rpt.write_text(json.dumps(report, indent=2, ensure_ascii=False))
    print(f"[save] {rpt}")

    # 与 PTQ 对比 + 择优
    if not args.no_compare:
        cmp = compare_with_ptq(m_qat)
        cmp_path = OUT_DIR / "quant_compare.json"
        cmp_path.write_text(json.dumps(cmp, indent=2, ensure_ascii=False))
        print(f"[save] {cmp_path}")
        if cmp.get("ptq_available"):
            print(f"\n=== PTQ vs QAT ===")
            print(f"  PTQ MAE: {cmp['ptq_mae']:.4f}")
            print(f"  QAT MAE: {cmp['qat_mae']:.4f}")
            print(f"  选定: {cmp['chosen'].upper()}")
            # 拷贝择优结果到 glucose_int8.tflite
            chosen_src = (
                args.out if cmp["chosen"] == "qat"
                else OUT_DIR / "glucose_int8_ptq.tflite"
            )
            if not chosen_src.exists() and cmp["chosen"] == "ptq":
                chosen_src = OUT_DIR / "glucose_int8.tflite"  # 旧版命名
            final = OUT_DIR / "glucose_int8.tflite"
            if chosen_src.exists() and chosen_src != final:
                final.write_bytes(chosen_src.read_bytes())
                print(f"[save] 最终模型 → {final}")


if __name__ == "__main__":
    main()
