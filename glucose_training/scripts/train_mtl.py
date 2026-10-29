#!/usr/bin/env python3
"""
train_mtl.py — P3 创新阶段: C1 (MTL) + C4 (Physics-prior) 联合训练

设计:
  - GroupKFold(5) × 1 seed × 50 epoch (诊断预算, 与 P1/P2 对齐可比)
  - Loss = 1.0 * Huber(glucose) + alpha_hr * Huber(hr_normalized)
  - HR target 从 X_scal[:, 0] / 120.0 抽出来 (与 normalize_scalars 一致)
  - 评估: glucose OOF 5 维硬闸门 + HR OOF (sanity check, 期望 Pearson>0.5)

预期 (非保证):
  - MTL 不能突破信息论上限, 但 backbone 共享 HR/glucose 学习可能让
    Pearson 从 0.07 提到 0.15-0.25, std_ratio 从 0.18 提到 0.3+
  - gate (PPG-driven) 平均值揭示模型对 PPG vs Physics 的依赖度

用法:
  python train_mtl.py --epochs 50 --alpha-hr 0.3
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

tf = None
build_mtl_model = None
normalize_scalars = None


def _ensure_deps():
    global tf, build_mtl_model, normalize_scalars
    if tf is not None:
        return
    try:
        import tensorflow as _tf
        tf = _tf
    except ImportError:
        print("[err] tensorflow not installed")
        sys.exit(2)
    from model_mtl import build_mtl_model as _bm, normalize_scalars as _ns
    build_mtl_model = _bm
    normalize_scalars = _ns


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
PROC_DIR = ROOT / "data" / "processed"
OUT_DIR = ROOT / "outputs" / "mtl"


# ── 数据加载 ──────────────────────────────────────────────────────────


def load_data():
    src = PROC_DIR / "multi.npz"
    if not src.exists():
        src = PROC_DIR / "must.npz"
    if not src.exists():
        print("[err] no data npz")
        sys.exit(2)
    d = np.load(src, allow_pickle=True)
    X_ppg = d["X_ppg"].astype(np.float32)
    X_scal_raw = d["X_scal"].astype(np.float32)   # 原始未归一
    X_scal = normalize_scalars(X_scal_raw)
    y = d["y"].astype(np.float32)
    # HR target: 取原 X_scal[:, 0],归一到 [0, 1] (与 normalize_scalars 公式一致)
    hr_target = np.clip(X_scal_raw[:, 0] / 120.0, 0.0, 1.0).astype(np.float32)
    sid = (np.asarray(d["subject_id"]).astype(str)
           if "subject_id" in d.files
           else np.array([f"auto_{i//3}" for i in range(len(y))]))
    return X_ppg, X_scal, hr_target, y, sid


# ── 评估工具 ──────────────────────────────────────────────────────────


def trivial_mean_mae(y_true):
    return float(np.mean(np.abs(y_true - y_true.mean())))


def quick_metrics(y_true, y_pred):
    err = y_pred - y_true
    mae = float(np.mean(np.abs(err)))
    ss_res = float(np.sum(err ** 2))
    ss_tot = float(np.sum((y_true - y_true.mean()) ** 2))
    r2 = float(1.0 - ss_res / max(ss_tot, 1e-9))
    if y_pred.std() < 1e-6 or y_true.std() < 1e-6:
        pearson = float("nan")
    else:
        pearson = float(np.corrcoef(y_true, y_pred)[0, 1])
    return {
        "MAE": mae, "R2": r2, "Pearson": pearson,
        "pred_mean": float(y_pred.mean()),
        "pred_std": float(y_pred.std()),
        "true_mean": float(y_true.mean()),
        "true_std": float(y_true.std()),
        "std_ratio": float(y_pred.std() / max(y_true.std(), 1e-9)),
    }


def gate_check_5dim(m, trivial_mae):
    """5 维硬闸门 (spec AC-2b)."""
    pearson_pass = (not np.isnan(m["Pearson"])) and (m["Pearson"] >= 0.4)
    gain_pct = (trivial_mae - m["MAE"]) / trivial_mae * 100
    return {
        "R2_pass": m["R2"] >= 0.3,
        "Pearson_pass": pearson_pass,
        "std_ratio_pass": m["std_ratio"] >= 0.5,
        "gain_pass": gain_pct >= 20.0,
        "MAE_pass": m["MAE"] <= 1.0,
        "n_pass": sum([m["R2"] >= 0.3, pearson_pass,
                       m["std_ratio"] >= 0.5, gain_pct >= 20.0,
                       m["MAE"] <= 1.0]),
        "gain_pct": gain_pct,
    }


# ── 训练一折 ──────────────────────────────────────────────────────────


def train_one_mtl(X_ppg_tr, X_scal_tr, hr_tr, y_tr,
                  X_ppg_va, X_scal_va, hr_va, y_va,
                  *, epochs, batch, seed, alpha_hr):
    tf.random.set_seed(seed)
    np.random.seed(seed)

    model = build_mtl_model()
    steps_per_epoch = max(len(y_tr) // batch, 1)

    # warmup-cosine LR
    warmup_steps = 10 * steps_per_epoch
    total_steps = epochs * steps_per_epoch
    lr_init, lr_peak, lr_min = 1e-4, 1e-3, 1e-5

    class WC(tf.keras.optimizers.schedules.LearningRateSchedule):
        def __call__(self, step):
            step = tf.cast(step, tf.float32)
            wlr = lr_init + (lr_peak - lr_init) * step / max(warmup_steps, 1)
            progress = (step - warmup_steps) / max(total_steps - warmup_steps, 1)
            progress = tf.clip_by_value(progress, 0.0, 1.0)
            clr = lr_min + 0.5 * (lr_peak - lr_min) * (1 + tf.cos(np.pi * progress))
            return tf.where(step < warmup_steps, wlr, clr)

        def get_config(self):
            return {}

    model.compile(
        optimizer=tf.keras.optimizers.Adam(WC()),
        loss={
            "glucose_out": tf.keras.losses.Huber(delta=1.0),
            "hr_pred": tf.keras.losses.Huber(delta=0.05),  # HR 归一化后 [0,1], 用小 delta
        },
        loss_weights={"glucose_out": 1.0, "hr_pred": alpha_hr},
    )

    cbs = [tf.keras.callbacks.EarlyStopping(
        monitor="val_glucose_out_loss", mode="min", patience=30,
        restore_best_weights=True, verbose=0,
    )]

    model.fit(
        {"ppg_in": X_ppg_tr, "scal_in": X_scal_tr},
        {"glucose_out": y_tr, "hr_pred": hr_tr},
        validation_data=(
            {"ppg_in": X_ppg_va, "scal_in": X_scal_va},
            {"glucose_out": y_va, "hr_pred": hr_va},
        ),
        epochs=epochs, batch_size=batch, callbacks=cbs, verbose=0,
    )

    preds = model.predict({"ppg_in": X_ppg_va, "scal_in": X_scal_va}, verbose=0)
    y_g = preds[0].squeeze()
    y_h = preds[1].squeeze()

    # gate 平均值 (诊断 — 看模型偏向 neural 还是 physics)
    # 直接抓 gate 层输出
    gate_layer = model.get_layer("gate")
    gate_model = tf.keras.Model(inputs=model.inputs, outputs=gate_layer.output)
    gate_vals = gate_model.predict(
        {"ppg_in": X_ppg_va, "scal_in": X_scal_va}, verbose=0
    ).squeeze()
    gate_mean = float(gate_vals.mean())
    gate_std = float(gate_vals.std())

    return y_g, y_h, gate_mean, gate_std


# ── 主流程 ────────────────────────────────────────────────────────────


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--epochs", type=int, default=50)
    parser.add_argument("--batch", type=int, default=16)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--alpha-hr", type=float, default=0.3,
                        help="HR aux loss weight (default 0.3)")
    args = parser.parse_args()

    _ensure_deps()
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    X_ppg, X_scal, hr_target, y, sid = load_data()
    n_subj = len(np.unique(sid))
    print(f"[data] N={len(y)} subjects={n_subj} "
          f"y_range=[{y.min():.2f}, {y.max():.2f}] mean={y.mean():.2f}+-{y.std():.2f}")
    print(f"[hr]   range=[{hr_target.min() * 120:.1f}, {hr_target.max() * 120:.1f}] bpm "
          f"(normalized [{hr_target.min():.3f}, {hr_target.max():.3f}])")
    tri_mae = trivial_mean_mae(y)
    print(f"[trivial-mean MAE] {tri_mae:.3f}")
    print(f"[config] alpha_hr={args.alpha_hr} epochs={args.epochs}")

    # GroupKFold(5)
    from sklearn.model_selection import GroupKFold
    gkf = GroupKFold(n_splits=5)
    all_y_g, all_y_h, all_yt_g, all_yt_h = [], [], [], []
    fold_metrics = []
    gate_means = []

    print("\n" + "=" * 72)
    print(" MTL Training (5 fold, GroupKFold by subject)")
    print("=" * 72)

    for fi, (idx_tr, idx_va) in enumerate(gkf.split(np.arange(len(y)), groups=sid)):
        print(f"\n[fold {fi}] train={len(idx_tr)} val={len(idx_va)}")
        y_g, y_h, gm, gs = train_one_mtl(
            X_ppg[idx_tr], X_scal[idx_tr], hr_target[idx_tr], y[idx_tr],
            X_ppg[idx_va], X_scal[idx_va], hr_target[idx_va], y[idx_va],
            epochs=args.epochs, batch=args.batch,
            seed=args.seed, alpha_hr=args.alpha_hr,
        )
        m_g = quick_metrics(y[idx_va], y_g)
        m_h = quick_metrics(hr_target[idx_va], y_h)
        print(f"  glucose MAE={m_g['MAE']:.3f}  R2={m_g['R2']:+.3f}  "
              f"Pearson={m_g['Pearson']:+.3f}  std_r={m_g['std_ratio']:.3f}")
        print(f"  hr      MAE={m_h['MAE']:.3f}  R2={m_h['R2']:+.3f}  "
              f"Pearson={m_h['Pearson']:+.3f}")
        print(f"  gate    mean={gm:.3f} std={gs:.3f}  "
              f"(>0.5 = lean neural, <0.5 = lean physics)")
        fold_metrics.append({
            "fold": fi, "glucose": m_g, "hr": m_h,
            "gate_mean": gm, "gate_std": gs,
        })
        gate_means.append(gm)
        all_y_g.append(y_g)
        all_y_h.append(y_h)
        all_yt_g.append(y[idx_va])
        all_yt_h.append(hr_target[idx_va])
        tf.keras.backend.clear_session()

    # ── OOF 拼接 ──────────────────────────────────────────────────────
    yp_g = np.concatenate(all_y_g)
    yt_g = np.concatenate(all_yt_g)
    yp_h = np.concatenate(all_y_h)
    yt_h = np.concatenate(all_yt_h)
    m_glucose = quick_metrics(yt_g, yp_g)
    m_hr = quick_metrics(yt_h, yp_h)
    gates_overall = float(np.mean(gate_means))

    print("\n" + "=" * 72)
    print(" OOF Summary")
    print("=" * 72)
    print(f"\n[OOF Glucose] MAE={m_glucose['MAE']:.3f}  R2={m_glucose['R2']:+.3f}  "
          f"Pearson={m_glucose['Pearson']:+.3f}  std_r={m_glucose['std_ratio']:.3f}  "
          f"pred_std={m_glucose['pred_std']:.3f}  true_std={m_glucose['true_std']:.3f}")
    print(f"[OOF HR]      MAE={m_hr['MAE']:.3f}  R2={m_hr['R2']:+.3f}  "
          f"Pearson={m_hr['Pearson']:+.3f}  "
          f"(target {yt_h.min()*120:.1f}-{yt_h.max()*120:.1f} bpm)")
    print(f"[gate overall mean] {gates_overall:.3f}  "
          f"({'neural-dominant' if gates_overall > 0.5 else 'physics-dominant'})")

    # ── 5 维硬闸门 ────────────────────────────────────────────────────
    gc = gate_check_5dim(m_glucose, tri_mae)
    print("\n" + "=" * 72)
    print(" 5-Dim Hard Gate (spec AC-2b)")
    print("=" * 72)
    print(f"  R2 >= 0.3        : {m_glucose['R2']:+.3f}  -> "
          f"{'PASS' if gc['R2_pass'] else 'FAIL'}")
    print(f"  Pearson >= 0.4   : {m_glucose['Pearson']:+.3f}  -> "
          f"{'PASS' if gc['Pearson_pass'] else 'FAIL'}")
    print(f"  std_ratio >= 0.5 : {m_glucose['std_ratio']:.3f}  -> "
          f"{'PASS' if gc['std_ratio_pass'] else 'FAIL'}")
    print(f"  gain >= 20%      : {gc['gain_pct']:+.1f}%  -> "
          f"{'PASS' if gc['gain_pass'] else 'FAIL'}")
    print(f"  MAE <= 1.0       : {m_glucose['MAE']:.3f}  -> "
          f"{'PASS' if gc['MAE_pass'] else 'FAIL'}")
    print(f"  -> n_pass = {gc['n_pass']}/5")

    # ── 对比 A++ baseline ────────────────────────────────────────────
    base_pred = ROOT / "outputs" / "ensemble_pred.npy"
    base_true = ROOT / "outputs" / "ensemble_true.npy"
    if base_pred.exists() and base_true.exists():
        yt_b = np.load(base_true)
        yp_b = np.load(base_pred)
        m_b = quick_metrics(yt_b, yp_b)
        print("\n" + "=" * 72)
        print(" Comparison vs A++ Baseline")
        print("=" * 72)
        print(f"{'metric':<14} {'A++ baseline':>14} {'MTL':>14} {'delta':>10}")
        print("-" * 60)
        for k in ["MAE", "R2", "Pearson", "std_ratio"]:
            dv = m_glucose[k] - m_b[k]
            print(f"{k:<14} {m_b[k]:>+14.3f} {m_glucose[k]:>+14.3f} "
                  f"{dv:>+10.3f}")

    # ── 保存 ─────────────────────────────────────────────────────────
    report = {
        "experiment": "P3 C1+C4 MTL + Physics-prior Fusion",
        "config": {
            "epochs": args.epochs,
            "alpha_hr": args.alpha_hr,
            "batch": args.batch,
            "seed": args.seed,
        },
        "n_samples": int(len(y)),
        "n_subjects": int(n_subj),
        "trivial_mean_MAE": tri_mae,
        "OOF_glucose": m_glucose,
        "OOF_hr": m_hr,
        "gate_overall_mean": gates_overall,
        "gate_check_5dim": gc,
        "fold_metrics": fold_metrics,
    }
    out = OUT_DIR / "mtl_report.json"
    out.write_text(json.dumps(report, indent=2, ensure_ascii=False, default=float))
    print(f"\n[save] {out}")

    # 同时存 OOF prediction 供后续诊断/蒸馏
    np.save(OUT_DIR / "mtl_glucose_pred.npy", yp_g)
    np.save(OUT_DIR / "mtl_glucose_true.npy", yt_g)
    np.save(OUT_DIR / "mtl_hr_pred.npy", yp_h)
    np.save(OUT_DIR / "mtl_hr_true.npy", yt_h)
    print(f"[save] OOF predictions -> {OUT_DIR}")


if __name__ == "__main__":
    main()
