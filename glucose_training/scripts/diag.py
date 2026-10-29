#!/usr/bin/env python3
"""
diag.py — P1 诊断包：4 个零打扰实验,定位"模型为何坍缩成 trivial baseline"

实验目录:
  A1 (PPG-only)    : 把 X_scal 置 0 重训,看 PPG 单独是否有可学信号
  A2 (Scalar-only) : 把 X_ppg 置 0 重训,看 scalar (HR/BMR/...) 单独贡献度
  A3 (BMR-noise)   : 把 BMR 列 (X_scal[:,6]) 替成 N(65,10) 噪声,看是否暴跌
                    → 暴跌 = BMR 是 shortcut (模型只学了 BMR 与血糖的伪相关)
  A4 (per-subject) : 不重训,用 ensemble_pred.npy + true.npy 按 subject 分组聚合
                    → 看高血糖 subject 是否误差暴崩 (trivial-mean 假说验证)

预算: A1/A2/A3 各 5fold × 1seed × 50 epoch × aug=0 (~20 min/实验)
      A4 < 5 sec

用法:
  python diag.py                     # 全跑 (A4 → A1 → A2 → A3)
  python diag.py --only a4           # 只跑 A4 (快速看分布)
  python diag.py --only a1 a3        # 跑 A1 + A3
  python diag.py --epochs 30         # 缩短重训预算 (debug)
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

tf = None
build_glucose_model = None
normalize_scalars = None
eval_metrics = None
train_one = None


def _ensure_deps():
    global tf, build_glucose_model, normalize_scalars, eval_metrics, train_one
    if tf is not None:
        return
    try:
        import tensorflow as _tf
        tf = _tf
    except ImportError:
        print("[err] tensorflow 未装")
        sys.exit(2)
    from model import build_glucose_model as _bm, normalize_scalars as _ns
    import train as _train_mod
    _train_mod._ensure_tf()  # 必须: train_one 闭包用的是 train 模块的 tf
    build_glucose_model = _bm
    normalize_scalars = _ns
    eval_metrics = _train_mod.eval_metrics
    train_one = _train_mod.train_one


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
PROC_DIR = ROOT / "data" / "processed"
OUT_DIR = ROOT / "outputs"
CKPT_DIR = ROOT / "models" / "checkpoints"
DIAG_DIR = OUT_DIR / "diag"


def load_full_data():
    """加载原始多源 npz (含 subject_id)。"""
    src = PROC_DIR / "multi.npz"
    if not src.exists():
        src = PROC_DIR / "must.npz"
    if not src.exists():
        print(f"[err] 找不到数据 npz")
        sys.exit(2)
    d = np.load(src, allow_pickle=True)
    X_ppg = d["X_ppg"].astype(np.float32)
    X_scal = normalize_scalars(d["X_scal"])
    y = d["y"].astype(np.float32)
    sid = (np.asarray(d["subject_id"]).astype(str)
           if "subject_id" in d.files
           else np.array([f"auto_{i//3}" for i in range(len(y))]))
    return X_ppg, X_scal, y, sid


def trivial_baselines(y_true):
    """trivial-mean / trivial-median / last-seen MAE,用于对比模型增益。"""
    mae_mean = float(np.mean(np.abs(y_true - y_true.mean())))
    mae_median = float(np.mean(np.abs(y_true - np.median(y_true))))
    # last-seen: 用 y[i-1] 预测 y[i], i=0 用 y.mean()
    last_pred = np.empty_like(y_true)
    last_pred[0] = y_true.mean()
    last_pred[1:] = y_true[:-1]
    mae_last = float(np.mean(np.abs(y_true - last_pred)))
    return {"mean": mae_mean, "median": mae_median, "last_seen": mae_last}


def quick_metrics(y_true, y_pred):
    """简版 metrics: MAE / R2 / Pearson / pred_std_ratio。"""
    err = y_pred - y_true
    mae = float(np.mean(np.abs(err)))
    ss_res = float(np.sum(err**2))
    ss_tot = float(np.sum((y_true - y_true.mean())**2))
    r2 = float(1.0 - ss_res / max(ss_tot, 1e-9))
    if y_pred.std() < 1e-6 or y_true.std() < 1e-6:
        pearson = float("nan")
    else:
        pearson = float(np.corrcoef(y_true, y_pred)[0, 1])
    pred_std_ratio = float(y_pred.std() / max(y_true.std(), 1e-9))
    return {
        "MAE_mmol": mae,
        "R2": r2,
        "Pearson_r": pearson,
        "pred_mean": float(y_pred.mean()),
        "pred_std": float(y_pred.std()),
        "true_mean": float(y_true.mean()),
        "true_std": float(y_true.std()),
        "pred_std_ratio": pred_std_ratio,
    }


# ════════════════════════════════════════════════════════════════════════
#  A4 — per-subject 误差分布 (零训练)
# ════════════════════════════════════════════════════════════════════════


def exp_a4(X_ppg, X_scal, y_full, sid):
    """A4 — 看 OOF ensemble 预测的 per-subject error 分布。

    假说: 模型是 trivial-mean predictor (恒输出 ~6 mmol/L)。
          → 真值接近 6 的 subject 误差极小,远离 6 的 subject 误差暴崩
          → 期望: high-glucose subject (y > 8) MAE >> mean subject MAE
    """
    print("\n" + "═" * 72)
    print(" A4 — per-subject error 分布 (零训练)")
    print("═" * 72)

    pred_file = OUT_DIR / "ensemble_pred.npy"
    true_file = OUT_DIR / "ensemble_true.npy"
    if not pred_file.exists() or not true_file.exists():
        print(f"[err] 缺 OOF 预测文件: {pred_file} / {true_file}")
        return None

    y_pred = np.load(pred_file)
    y_true = np.load(true_file)
    print(f"[load] ensemble_pred.npy ({y_pred.shape}) + ensemble_true.npy ({y_true.shape})")

    # GroupKFold 的预测顺序与原数据顺序不同,需要重建对应 sid
    # 重新跑一次 GroupKFold 拿 val_idx 顺序
    from sklearn.model_selection import GroupKFold
    splits = list(GroupKFold(n_splits=5).split(np.arange(len(y_full)), groups=sid))
    val_idx_concat = np.concatenate([va for _, va in splits])
    sid_concat = sid[val_idx_concat]
    y_full_concat = y_full[val_idx_concat]

    # 校验: y_full_concat 应 ≈ y_true (allow 浮点小误差)
    if len(y_full_concat) != len(y_true):
        print(f"[warn] 长度不匹配: full={len(y_full_concat)} pred={len(y_true)}")
        return None
    diff = np.abs(y_full_concat - y_true).max()
    if diff > 1e-3:
        print(f"[warn] 真值顺序对不上 (max diff={diff:.3f}),无法做 per-subject 分组")
        return None

    # 按 subject 聚合
    err_per_sample = y_pred - y_true
    subjects = sorted(np.unique(sid_concat))
    rows = []
    for sj in subjects:
        mask = sid_concat == sj
        if mask.sum() == 0:
            continue
        y_t = y_true[mask]
        y_p = y_pred[mask]
        e = y_p - y_t
        rows.append({
            "subject": sj,
            "n": int(mask.sum()),
            "true_mean": float(y_t.mean()),
            "true_range_min": float(y_t.min()),
            "true_range_max": float(y_t.max()),
            "pred_mean": float(y_p.mean()),
            "MAE": float(np.mean(np.abs(e))),
            "bias": float(np.mean(e)),
        })

    # 按真值均值排序
    rows.sort(key=lambda r: r["true_mean"])
    print(f"\nN subjects = {len(rows)}\n")
    print(f"{'subject':<28} {'n':>3} {'true_μ':>7} {'pred_μ':>7} {'MAE':>6} {'bias':>7}")
    print("-" * 72)
    for r in rows:
        print(f"{r['subject']:<28} {r['n']:>3} {r['true_mean']:>7.2f} "
              f"{r['pred_mean']:>7.2f} {r['MAE']:>6.2f} {r['bias']:>+7.2f}")

    # 分层统计 (按 true_mean 区间)
    print("\n[分层] 按 subject true_mean 区间聚合:")
    print(f"{'bucket':<20} {'#subj':>6} {'MAE':>6} {'bias':>7} {'true_μ':>7} {'pred_μ':>7}")
    print("-" * 60)
    buckets = [
        ("low <5.5",   lambda r: r["true_mean"] < 5.5),
        ("mid 5.5-7",  lambda r: 5.5 <= r["true_mean"] < 7.0),
        ("hi 7-9",     lambda r: 7.0 <= r["true_mean"] < 9.0),
        ("vhi >=9",    lambda r: r["true_mean"] >= 9.0),
    ]
    bucket_summary = []
    for name, fn in buckets:
        sub = [r for r in rows if fn(r)]
        if not sub:
            print(f"{name:<20} {'-':>6}")
            continue
        n_subj = len(sub)
        avg_mae = float(np.mean([r["MAE"] for r in sub]))
        avg_bias = float(np.mean([r["bias"] for r in sub]))
        avg_true = float(np.mean([r["true_mean"] for r in sub]))
        avg_pred = float(np.mean([r["pred_mean"] for r in sub]))
        print(f"{name:<20} {n_subj:>6} {avg_mae:>6.2f} {avg_bias:>+7.2f} "
              f"{avg_true:>7.2f} {avg_pred:>7.2f}")
        bucket_summary.append({
            "bucket": name, "n_subj": n_subj, "MAE": avg_mae,
            "bias": avg_bias, "true_mean": avg_true, "pred_mean": avg_pred,
        })

    result = {
        "experiment": "A4_per_subject",
        "n_subjects": len(rows),
        "subjects": rows,
        "buckets": bucket_summary,
    }

    # 结论自动推导
    high_buckets = [b for b in bucket_summary if b["bucket"] in ("hi 7-9", "vhi >=9")]
    low_buckets = [b for b in bucket_summary if b["bucket"] in ("low <5.5", "mid 5.5-7")]
    if high_buckets and low_buckets:
        high_mae = float(np.mean([b["MAE"] for b in high_buckets]))
        low_mae = float(np.mean([b["MAE"] for b in low_buckets]))
        ratio = high_mae / max(low_mae, 1e-6)
        print(f"\n[A4 结论] high (>7) MAE = {high_mae:.2f},low (<7) MAE = {low_mae:.2f},ratio = {ratio:.2f}x")
        if ratio > 3.0:
            verdict = "✓ trivial-mean predictor 假说成立 — 高血糖 subject 误差暴崩"
        elif ratio > 1.5:
            verdict = "~ 部分支持 trivial-mean 假说 — 但不显著"
        else:
            verdict = "✗ trivial-mean 假说否定 — 高血糖 subject 与低血糖 MAE 相近"
        print(f"          {verdict}")
        result["verdict"] = verdict
        result["high_low_ratio"] = ratio

    return result


# ════════════════════════════════════════════════════════════════════════
#  A1/A2/A3 重训核心
# ════════════════════════════════════════════════════════════════════════


def run_retrain(tag, X_ppg, X_scal, y, sid, *, folds=5, seed=42, epochs=50, batch=16):
    """5fold × 1seed × 0 aug 重训,返回 OOF 指标 + per-fold 分布。"""
    from sklearn.model_selection import GroupKFold
    gkf = GroupKFold(n_splits=folds)

    all_pred, all_true = [], []
    fold_metrics = []

    for fi, (idx_tr, idx_va) in enumerate(gkf.split(np.arange(len(y)), groups=sid)):
        ckpt = CKPT_DIR / f"diag_{tag}_fold{fi}.h5"
        m, y_pred = train_one(
            X_ppg[idx_tr], X_scal[idx_tr], y[idx_tr],
            X_ppg[idx_va], X_scal[idx_va], y[idx_va],
            epochs=epochs, batch=batch, aug=0, seed=seed,
            huber_delta=1.0, ckpt_path=ckpt,
        )
        fold_metrics.append(m)
        all_pred.append(y_pred)
        all_true.append(y[idx_va])
        print(f"  [{tag} fold {fi}] MAE={m['MAE_mmol']:.3f} CEG_AB={m['CEG_AB_pct']:.1f}%")
        # 清 ckpt 减小磁盘 (诊断不需要保留)
        if ckpt.exists():
            ckpt.unlink()
        tf.keras.backend.clear_session()

    y_pred_oof = np.concatenate(all_pred)
    y_true_oof = np.concatenate(all_true)
    metrics = quick_metrics(y_true_oof, y_pred_oof)
    trivials = trivial_baselines(y_true_oof)
    metrics["MAE_trivial_mean"] = trivials["mean"]
    metrics["gain_vs_mean_pct"] = (trivials["mean"] - metrics["MAE_mmol"]) / trivials["mean"] * 100
    print(f"\n[{tag} OOF]  MAE={metrics['MAE_mmol']:.3f}  "
          f"R2={metrics['R2']:+.3f}  Pearson={metrics['Pearson_r']:+.3f}  "
          f"pred_std/true_std={metrics['pred_std_ratio']:.3f}  "
          f"gain_vs_mean={metrics['gain_vs_mean_pct']:+.1f}%")
    return {"oof_metrics": metrics, "fold_metrics": fold_metrics}


# ════════════════════════════════════════════════════════════════════════
#  A1 — PPG-only (scal_in 置 0)
# ════════════════════════════════════════════════════════════════════════


def exp_a1(X_ppg, X_scal, y, sid, *, epochs):
    print("\n" + "═" * 72)
    print(" A1 — PPG-only (X_scal 置 0)")
    print("═" * 72)
    print("假说: 若 R2 上升 → PPG 单独有可学信号, scalar 在干扰")
    print("      若 R2 仍 ~0 → PPG 5s 窗在 67 样本上无可学信号")
    X_scal_zero = np.zeros_like(X_scal)
    return run_retrain("a1_ppg_only", X_ppg, X_scal_zero, y, sid, epochs=epochs)


# ════════════════════════════════════════════════════════════════════════
#  A2 — Scalar-only (ppg_in 置 0)
# ════════════════════════════════════════════════════════════════════════


def exp_a2(X_ppg, X_scal, y, sid, *, epochs):
    print("\n" + "═" * 72)
    print(" A2 — Scalar-only (X_ppg 置 0)")
    print("═" * 72)
    print("假说: 若 R2 > A++ 全模型 → scalar (尤其 BMR) 是 shortcut, PPG 在噪声化")
    X_ppg_zero = np.zeros_like(X_ppg)
    return run_retrain("a2_scal_only", X_ppg_zero, X_scal, y, sid, epochs=epochs)


# ════════════════════════════════════════════════════════════════════════
#  A3 — BMR-noise (替换为 N(65,10))
# ════════════════════════════════════════════════════════════════════════


def exp_a3(X_ppg, X_scal, y, sid, *, epochs):
    print("\n" + "═" * 72)
    print(" A3 — BMR-noise (X_scal[:,6] = N(65,10)/120 归一化后)")
    print("═" * 72)
    print("假说: 若 R2 暴跌 → BMR 是 shortcut,A++ 性能不是 PPG 学的而是 BMR 拟合")
    rng = np.random.default_rng(42)
    X_scal_noise = X_scal.copy()
    # 注入 N(65, 10) 然后归一 /120 (与 normalize_scalars 公式一致), clip [0,1]
    fake_bmr = rng.normal(65.0, 10.0, size=len(X_scal_noise)).astype(np.float32)
    X_scal_noise[:, 6] = np.clip(fake_bmr / 120.0, 0.0, 1.0)
    return run_retrain("a3_bmr_noise", X_ppg, X_scal_noise, y, sid, epochs=epochs)


# ════════════════════════════════════════════════════════════════════════
#  主流程
# ════════════════════════════════════════════════════════════════════════


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--only", nargs="+", default=["a4", "a1", "a2", "a3"],
                        choices=["a1", "a2", "a3", "a4"],
                        help="选择跑的实验,默认全跑")
    parser.add_argument("--epochs", type=int, default=50,
                        help="A1/A2/A3 重训 epoch 数")
    args = parser.parse_args()

    _ensure_deps()
    DIAG_DIR.mkdir(parents=True, exist_ok=True)

    X_ppg, X_scal, y, sid = load_full_data()
    print(f"[data] N={len(y)} subjects={len(np.unique(sid))} "
          f"y_range=[{y.min():.2f}, {y.max():.2f}] mean={y.mean():.2f}±{y.std():.2f}")

    results = {}

    if "a4" in args.only:
        results["a4"] = exp_a4(X_ppg, X_scal, y, sid)

    if "a1" in args.only:
        results["a1"] = exp_a1(X_ppg, X_scal, y, sid, epochs=args.epochs)

    if "a2" in args.only:
        results["a2"] = exp_a2(X_ppg, X_scal, y, sid, epochs=args.epochs)

    if "a3" in args.only:
        results["a3"] = exp_a3(X_ppg, X_scal, y, sid, epochs=args.epochs)

    # ── 总结表 ────────────────────────────────────────────────────────
    print("\n" + "═" * 72)
    print(" 诊断总结")
    print("═" * 72)
    print(f"{'实验':<22} {'MAE':>7} {'R2':>7} {'Pearson':>8} {'std_ratio':>10} {'gain%':>7}")
    print("-" * 70)

    # A++ baseline (from outputs/ensemble_pred.npy)
    if (OUT_DIR / "ensemble_pred.npy").exists():
        yp_base = np.load(OUT_DIR / "ensemble_pred.npy")
        yt_base = np.load(OUT_DIR / "ensemble_true.npy")
        m_base = quick_metrics(yt_base, yp_base)
        tb = trivial_baselines(yt_base)
        gain_base = (tb["mean"] - m_base["MAE_mmol"]) / tb["mean"] * 100
        print(f"{'A++ baseline (现状)':<22} {m_base['MAE_mmol']:>7.3f} "
              f"{m_base['R2']:>+7.3f} {m_base['Pearson_r']:>+8.3f} "
              f"{m_base['pred_std_ratio']:>10.3f} {gain_base:>+7.1f}")
        results["a0_baseline"] = m_base

    for tag in ["a1", "a2", "a3"]:
        if tag in results:
            m = results[tag]["oof_metrics"]
            print(f"{tag.upper()} ({'PPG-only' if tag == 'a1' else 'Scal-only' if tag == 'a2' else 'BMR-noise'}):"
                  + " " * (22 - len(tag) - 12)
                  + f"{m['MAE_mmol']:>7.3f} {m['R2']:>+7.3f} {m['Pearson_r']:>+8.3f} "
                  + f"{m['pred_std_ratio']:>10.3f} {m['gain_vs_mean_pct']:>+7.1f}")

    # ── A4 verdict ────────────────────────────────────────────────────
    if "a4" in results and results["a4"] and "verdict" in results["a4"]:
        print(f"\nA4 verdict: {results['a4']['verdict']}")

    # ── 决策建议 ──────────────────────────────────────────────────────
    print("\n" + "═" * 72)
    print(" P1.D 决策建议 (自动)")
    print("═" * 72)

    decisions = []
    if "a1" in results:
        a1_r2 = results["a1"]["oof_metrics"]["R2"]
        if a1_r2 > 0.1:
            decisions.append(f"[GO]   A1 R2={a1_r2:+.3f} > 0.1 -> PPG 有可学信号, D1 (方差正则) 值得跑")
        elif a1_r2 > -0.1:
            decisions.append(f"[WARN] A1 R2={a1_r2:+.3f} ~ 0 -> PPG 5s 窗信号微弱, D 阶段收益有限")
        else:
            decisions.append(f"[STOP] A1 R2={a1_r2:+.3f} < -0.1 -> PPG 在 67 样本上无可学信号, 承认天花板")

    if "a3" in results and "a0_baseline" in results:
        a3_mae = results["a3"]["oof_metrics"]["MAE_mmol"]
        a0_mae = results["a0_baseline"]["MAE_mmol"]
        delta = (a3_mae - a0_mae) / a0_mae * 100
        if delta > 20:
            decisions.append(f"[STOP] A3 BMR-noise MAE 暴涨 {delta:+.1f}% -> BMR 是 shortcut, D2 (BMR-dropout) 必做")
        elif delta > 5:
            decisions.append(f"[WARN] A3 BMR-noise MAE 涨 {delta:+.1f}% -> BMR 有轻微 shortcut 嫌疑")
        else:
            decisions.append(f"[OK]   A3 BMR-noise MAE 涨 {delta:+.1f}% -> BMR 不是 shortcut")

    for d in decisions:
        print(f"  {d}")

    # ── 保存 ──────────────────────────────────────────────────────────
    out_json = DIAG_DIR / "diag_report.json"
    out_json.write_text(json.dumps(results, indent=2, ensure_ascii=False, default=float))
    print(f"\n[save] {out_json}")


if __name__ == "__main__":
    main()
