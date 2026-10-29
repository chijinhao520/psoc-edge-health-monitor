#!/usr/bin/env python3
"""
baseline_compare.py — 防 trivial-predictor 自欺的永久档

读 outputs/ensemble_pred.npy + ensemble_true.npy（train.py 写出的 OOF 预测），
对照以下 4 个 baseline 算 MAE/RMSE/R²/Pearson r/pred-std-ratio：

  1. trivial-mean    : 永远输出 y_true.mean()
  2. trivial-median  : 永远输出 np.median(y_true)
  3. last-seen       : 永远输出 y_true[0]（"昨天血糖"的极端版）
  4. our model       : OOF ensemble 预测

如果 our model 在 R²/Pearson/std-ratio/gain-vs-mean 上**没显著好于 baseline**，
则 AC-2b 闸门 FAIL，无论 MAE 多低、Clarke A+B 多高。

写出：
  outputs/baseline_compare.json  (机读)
  控制台对照表

退出码：
  0 = model 显著好于所有 trivial baseline（pass）
  1 = model 与 trivial baseline 不可区分（trivial predictor 警告）
  2 = 文件缺失 / 数据错误

用法:
  python baseline_compare.py
  python baseline_compare.py --pred outputs/glucose_int8_pred.npy
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
OUT_DIR = ROOT / "outputs"


# ── 闸门阈值（与 spec.md AC-2b 同步） ─────────────────────────────────
GATE_R2 = 0.30                      # 模型 R² 至少 ≥ 0.3
GATE_PEARSON = 0.40                 # Pearson r ≥ 0.4
GATE_PRED_STD_RATIO = 0.50          # pred σ / true σ ≥ 0.5（防常数输出）
GATE_GAIN_PCT = 20.0                # MAE 增益 vs trivial-mean ≥ 20%


def metrics(y_true: np.ndarray, y_pred: np.ndarray) -> dict:
    """完整指标：MAE/RMSE/MAPE + R²/Pearson r + 预测分布特征。"""
    err = y_pred - y_true
    mae = float(np.mean(np.abs(err)))
    rmse = float(np.sqrt(np.mean(err ** 2)))
    mape = float(np.mean(np.abs(err / (y_true + 1e-6))) * 100)
    ss_res = float(np.sum(err ** 2))
    ss_tot = float(np.sum((y_true - y_true.mean()) ** 2))
    r2 = 1.0 - ss_res / max(ss_tot, 1e-9)
    # Pearson r 仅在两个序列都不是常数时有定义
    if y_pred.std() < 1e-9 or y_true.std() < 1e-9:
        pearson = 0.0
    else:
        pearson = float(np.corrcoef(y_true, y_pred)[0, 1])
    return {
        "MAE_mmol":         mae,
        "RMSE_mmol":        rmse,
        "MAPE_pct":         mape,
        "R2":               r2,
        "Pearson_r":        pearson,
        "pred_mean":        float(y_pred.mean()),
        "pred_std":         float(y_pred.std()),
        "pred_std_ratio":   float(y_pred.std() / max(y_true.std(), 1e-9)),
    }


def make_baselines(y_true: np.ndarray) -> dict:
    """生成 3 个 trivial baseline 的预测。"""
    return {
        "trivial_mean":   np.full_like(y_true, y_true.mean()),
        "trivial_median": np.full_like(y_true, float(np.median(y_true))),
        "last_seen":      np.full_like(y_true, float(y_true[0])),
    }


def evaluate_all(y_true: np.ndarray, y_pred_model: np.ndarray) -> dict:
    """对 model + 3 baseline 算指标。"""
    out = {"our_model": metrics(y_true, y_pred_model)}
    for name, y_b in make_baselines(y_true).items():
        out[name] = metrics(y_true, y_b)
    return out


def check_gates(model: dict, mean_baseline: dict) -> dict:
    """AC-2b 4 维闸门检查。"""
    mae_gain_pct = (mean_baseline["MAE_mmol"] - model["MAE_mmol"]) \
                   / max(mean_baseline["MAE_mmol"], 1e-9) * 100
    gates = {
        "R2_pass":           model["R2"] >= GATE_R2,
        "Pearson_pass":      model["Pearson_r"] >= GATE_PEARSON,
        "pred_std_pass":     model["pred_std_ratio"] >= GATE_PRED_STD_RATIO,
        "gain_vs_mean_pass": mae_gain_pct >= GATE_GAIN_PCT,
        "mae_gain_vs_mean_pct": mae_gain_pct,
        "thresholds": {
            "R2":               GATE_R2,
            "Pearson_r":        GATE_PEARSON,
            "pred_std_ratio":   GATE_PRED_STD_RATIO,
            "gain_vs_mean_pct": GATE_GAIN_PCT,
        },
    }
    gates["all_pass"] = all(gates[k] for k in (
        "R2_pass", "Pearson_pass", "pred_std_pass", "gain_vs_mean_pass"))
    return gates


def format_table(results: dict) -> str:
    """ASCII 对照表。"""
    cols = ["MAE_mmol", "RMSE_mmol", "R2", "Pearson_r",
            "pred_mean", "pred_std", "pred_std_ratio"]
    header = "{:<18} ".format("predictor") + " ".join(
        "{:>11}".format(c[:11]) for c in cols)
    lines = [header, "-" * len(header)]
    order = ["our_model", "trivial_mean", "trivial_median", "last_seen"]
    for name in order:
        if name not in results:
            continue
        m = results[name]
        row = "{:<18} ".format(name) + " ".join(
            "{:>11.4f}".format(m[c]) for c in cols)
        lines.append(row)
    return "\n".join(lines)


def format_gate_report(gates: dict) -> str:
    th = gates["thresholds"]
    return (
        "\n=== AC-2b Gate Check ===\n"
        "  R2              >= {:.2f}    {:<6}  (model={:.4f})\n"
        "  Pearson r       >= {:.2f}    {:<6}  (model={:.4f})\n"
        "  pred_std_ratio  >= {:.2f}    {:<6}  (model={:.4f})\n"
        "  MAE gain vs mean>= {:.0f}%    {:<6}  (model={:+.2f}%)\n"
        "  ----------------------------------------\n"
        "  ALL_PASS        :          {}\n"
    ).format(
        th["R2"],
        "PASS" if gates["R2_pass"] else "FAIL",
        gates.get("_R2", float("nan")),
        th["Pearson_r"],
        "PASS" if gates["Pearson_pass"] else "FAIL",
        gates.get("_Pearson", float("nan")),
        th["pred_std_ratio"],
        "PASS" if gates["pred_std_pass"] else "FAIL",
        gates.get("_pred_std", float("nan")),
        th["gain_vs_mean_pct"],
        "PASS" if gates["gain_vs_mean_pass"] else "FAIL",
        gates["mae_gain_vs_mean_pct"],
        "[PASS]" if gates["all_pass"] else "[FAIL] (trivial-predictor warning)",
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pred", type=Path,
                        default=OUT_DIR / "ensemble_pred.npy",
                        help="模型预测 npy（默认 ensemble_pred.npy）")
    parser.add_argument("--true", type=Path,
                        default=OUT_DIR / "ensemble_true.npy",
                        help="真值 npy（默认 ensemble_true.npy）")
    parser.add_argument("--out", type=Path,
                        default=OUT_DIR / "baseline_compare.json")
    parser.add_argument("--tag", type=str, default="default",
                        help="本次实验标签（写入 JSON,区分多次跑）")
    args = parser.parse_args()

    if not args.pred.exists() or not args.true.exists():
        print(f"[err] 找不到 {args.pred} 或 {args.true}\n"
              f"      先跑 train.py 产生 OOF 预测", file=sys.stderr)
        sys.exit(2)

    y_pred = np.load(args.pred).astype(np.float32).squeeze()
    y_true = np.load(args.true).astype(np.float32).squeeze()
    if len(y_pred) != len(y_true):
        print(f"[err] 长度不一致 pred={len(y_pred)} true={len(y_true)}",
              file=sys.stderr)
        sys.exit(2)

    results = evaluate_all(y_true, y_pred)
    print(format_table(results))

    gates = check_gates(results["our_model"], results["trivial_mean"])
    # 把模型实际数填进去供 format 用
    gates["_R2"] = results["our_model"]["R2"]
    gates["_Pearson"] = results["our_model"]["Pearson_r"]
    gates["_pred_std"] = results["our_model"]["pred_std_ratio"]
    print(format_gate_report(gates))

    report = {
        "tag": args.tag,
        "n_samples": int(len(y_true)),
        "y_true_stats": {
            "mean": float(y_true.mean()),
            "std":  float(y_true.std()),
            "min":  float(y_true.min()),
            "max":  float(y_true.max()),
        },
        "results": results,
        "gates": {k: v for k, v in gates.items() if not k.startswith("_")},
    }
    args.out.write_text(json.dumps(report, indent=2, ensure_ascii=False))
    print(f"\n[save] {args.out}")

    sys.exit(0 if gates["all_pass"] else 1)


if __name__ == "__main__":
    main()
