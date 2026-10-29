#!/usr/bin/env python3
"""
calibration.py — P3 C3: Personalized Edge Calibration Loop

设计:
  用户首次使用前, 提供 K 次"模型预测"与"真血糖" pair (K=2-5),
  设备本地拟合 1D 线性变换 y_personal = a * y_model + b,
  后续推理输出 y_personal 而非 y_model.

  K=2 -> 最小二乘 2 点拟合 (a, b)
  K>=3 -> OLS 最小二乘
  数学:
    minimize sum_i (y_true_i - (a * y_model_i + b))^2
    -> a = cov(y_model, y_true) / var(y_model)
       b = mean(y_true) - a * mean(y_model)

  正则化 (避免 K 小时 overfit):
    a 限制 [0.5, 2.0] (硬截断, 防止 anti-correlation 翻转)
    若 var(y_model) < 1e-3 (模型预测几乎常数), a 强制 1.0, b = mean(y_true) - mean(y_model)
    (退化为纯 offset 校准)

模拟评估:
  从 OOF 预测里, 对每个 subject 抽 K 个样本 (前 K 个时间点) 当 calibration ref,
  剩余样本作 hold-out, 看 calibration 后 MAE 改善多少.

部署形态 (M55 侧):
  calibration_task.c 持久化 (a, b) 到 flash;
  glucose_task 推理后做 y = a * y_npu + b 即可.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
PROC_DIR = ROOT / "data" / "processed"
OUT_DIR = ROOT / "outputs" / "calibration"


# ── 校准算法 (M55 端要 1:1 移植到 C) ──────────────────────────────────


def fit_personal_calib(y_model_ref: np.ndarray, y_true_ref: np.ndarray,
                       a_min: float = 0.5, a_max: float = 2.0) -> tuple[float, float]:
    """OLS 拟合 y_true = a * y_model + b.

    Args:
      y_model_ref: K 个模型预测 (mmol/L)
      y_true_ref: K 个真血糖 (mmol/L), Accu-Check 等指尖测量值
    Returns:
      (a, b): a in [a_min, a_max], b 无限制
    """
    if len(y_model_ref) < 1:
        return 1.0, 0.0

    if len(y_model_ref) == 1:
        # K=1: 只能做纯 offset (a=1)
        return 1.0, float(y_true_ref[0] - y_model_ref[0])

    var_m = float(np.var(y_model_ref))
    if var_m < 1e-3:
        # 模型预测几乎常数 → 退化为纯 offset
        return 1.0, float(np.mean(y_true_ref) - np.mean(y_model_ref))

    cov_mt = float(np.cov(y_model_ref, y_true_ref, ddof=0)[0, 1])
    a = cov_mt / var_m
    a = float(np.clip(a, a_min, a_max))
    b = float(np.mean(y_true_ref) - a * np.mean(y_model_ref))
    return a, b


def apply_calib(y_model: np.ndarray, a: float, b: float) -> np.ndarray:
    return a * y_model + b


# ── 模拟评估 ──────────────────────────────────────────────────────────


def simulate_per_subject_calibration(y_model: np.ndarray, y_true: np.ndarray,
                                     sid: np.ndarray, K: int):
    """对每个 subject:
       - 取前 K 个样本作 calibration ref → 拟合 (a, b)
       - 剩余样本作 hold-out, 计算 calibration 前后 MAE
       - 若样本数 <= K, 全部当 ref, hold-out 为空 (跳过)
    返回 (mae_before, mae_after, per_subject_results)
    """
    per_subject = []
    holdout_before = []
    holdout_after = []

    subjects = sorted(np.unique(sid))
    for sj in subjects:
        mask = sid == sj
        idx = np.where(mask)[0]
        if len(idx) <= K:
            continue
        ref_idx = idx[:K]
        ho_idx = idx[K:]

        a, b = fit_personal_calib(y_model[ref_idx], y_true[ref_idx])
        y_calib_ho = apply_calib(y_model[ho_idx], a, b)
        err_before = np.abs(y_model[ho_idx] - y_true[ho_idx])
        err_after = np.abs(y_calib_ho - y_true[ho_idx])
        mae_b = float(err_before.mean())
        mae_a = float(err_after.mean())

        per_subject.append({
            "subject": str(sj),
            "n_ref": int(K),
            "n_holdout": int(len(ho_idx)),
            "a": a, "b": b,
            "mae_before": mae_b, "mae_after": mae_a,
            "improvement_pct": (mae_b - mae_a) / max(mae_b, 1e-6) * 100,
        })
        holdout_before.append(err_before)
        holdout_after.append(err_after)

    if not holdout_before:
        return None

    mae_all_before = float(np.concatenate(holdout_before).mean())
    mae_all_after = float(np.concatenate(holdout_after).mean())
    improvement = (mae_all_before - mae_all_after) / max(mae_all_before, 1e-6) * 100

    return {
        "K_ref": K,
        "n_subjects_with_holdout": len(per_subject),
        "MAE_before_calib": mae_all_before,
        "MAE_after_calib": mae_all_after,
        "MAE_improvement_pct": improvement,
        "per_subject": per_subject,
    }


# ── 主流程 ────────────────────────────────────────────────────────────


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--K-list", nargs="+", type=int, default=[1, 2, 3],
                        help="K values to try (default 1 2 3)")
    args = parser.parse_args()

    OUT_DIR.mkdir(parents=True, exist_ok=True)

    # 加载 OOF 预测 + 真值 + sid
    pred_file = ROOT / "outputs" / "ensemble_pred.npy"
    true_file = ROOT / "outputs" / "ensemble_true.npy"
    if not (pred_file.exists() and true_file.exists()):
        print(f"[err] missing OOF files: {pred_file} / {true_file}")
        sys.exit(2)
    y_pred = np.load(pred_file).astype(np.float32)
    y_true = np.load(true_file).astype(np.float32)

    # 重建 sid (GroupKFold val 顺序)
    src = PROC_DIR / "multi.npz"
    if not src.exists():
        src = PROC_DIR / "must.npz"
    d = np.load(src, allow_pickle=True)
    y_full = d["y"].astype(np.float32)
    sid_full = (np.asarray(d["subject_id"]).astype(str)
                if "subject_id" in d.files
                else np.array([f"auto_{i//3}" for i in range(len(y_full))]))
    from sklearn.model_selection import GroupKFold
    splits = list(GroupKFold(n_splits=5).split(np.arange(len(y_full)), groups=sid_full))
    val_idx_concat = np.concatenate([va for _, va in splits])
    sid = sid_full[val_idx_concat]

    if abs(y_full[val_idx_concat].max() - y_true.max()) > 0.01:
        print(f"[warn] true values mismatched, sid alignment may be off")

    n = len(y_pred)
    print(f"[data] N={n} subjects={len(np.unique(sid))} "
          f"y_range=[{y_true.min():.2f}, {y_true.max():.2f}]")
    print(f"[A++ pred] mean={y_pred.mean():.2f} std={y_pred.std():.3f}")
    print(f"[true]     mean={y_true.mean():.2f} std={y_true.std():.3f}")

    # baseline (no calibration)
    mae_baseline = float(np.mean(np.abs(y_pred - y_true)))
    print(f"\n[baseline MAE] {mae_baseline:.3f} (no calib)")

    # 对每个 K 跑模拟
    print("\n" + "=" * 72)
    print(" Personalized Calibration Simulation")
    print("=" * 72)
    print(f"{'K':>3} {'#subj_ho':>10} {'MAE_before':>12} {'MAE_after':>12} {'improve%':>10}")
    print("-" * 60)

    all_results = {}
    for K in args.K_list:
        r = simulate_per_subject_calibration(y_pred, y_true, sid, K)
        if r is None:
            print(f"{K:>3} {'(no holdout)':>10}")
            continue
        print(f"{K:>3} {r['n_subjects_with_holdout']:>10} "
              f"{r['MAE_before_calib']:>12.3f} {r['MAE_after_calib']:>12.3f} "
              f"{r['MAE_improvement_pct']:>+10.1f}")
        all_results[f"K_{K}"] = r

    # 示例 subject 详情 (K=3)
    if "K_3" in all_results:
        print("\n" + "=" * 72)
        print(" Per-subject Detail (K=3)")
        print("=" * 72)
        print(f"{'subject':<26} {'a':>6} {'b':>7} {'MAE_b':>7} {'MAE_a':>7} {'impr%':>7}")
        print("-" * 70)
        for s in all_results["K_3"]["per_subject"][:15]:
            print(f"{s['subject']:<26} {s['a']:>6.3f} {s['b']:>+7.3f} "
                  f"{s['mae_before']:>7.3f} {s['mae_after']:>7.3f} "
                  f"{s['improvement_pct']:>+7.1f}")

    # 保存
    report = {
        "experiment": "P3 C3 Personalized Edge Calibration",
        "n_samples": int(n),
        "n_subjects_total": int(len(np.unique(sid))),
        "MAE_no_calib": mae_baseline,
        "by_K": all_results,
    }
    out = OUT_DIR / "calibration_report.json"
    out.write_text(json.dumps(report, indent=2, ensure_ascii=False, default=float))
    print(f"\n[save] {out}")

    # ── M55 部署引导 ─────────────────────────────────────────────────
    print("\n" + "=" * 72)
    print(" M55 Deployment Guidance")
    print("=" * 72)
    print("  1. UI flow:")
    print("     - 首次启动 -> 引导用户测 K 次手指血糖 (Accu-Check),")
    print("       同时设备记录 K 个模型预测")
    print("     - 设备本地调用 fit_personal_calib() -> 存 (a, b) 到 flash")
    print("  2. 推理时:")
    print("     - glucose_task: y_npu = model.predict(...)")
    print("     - calibration_task: y_final = a * y_npu + b")
    print("  3. 重新校准触发:")
    print("     - 每次用户测一次真血糖 (后续校验) -> 用 EMA 更新 (a, b)")
    print("     - a_new = 0.8 * a_old + 0.2 * a_fit (滑动平均)")
    if "K_3" in all_results:
        impr = all_results["K_3"]["MAE_improvement_pct"]
        print(f"\n  K=3 仿真预期 MAE 改善 {impr:+.1f}% (从 {mae_baseline:.3f} "
              f"到 {all_results['K_3']['MAE_after_calib']:.3f} mmol/L)")


if __name__ == "__main__":
    main()
