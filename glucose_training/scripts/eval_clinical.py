#!/usr/bin/env python3
"""
eval_clinical.py — A++ 临床级评估（Clarke EGA + Bland-Altman + ISO 15197）

输入：
  --pred outputs/ensemble_pred.npy   (N,) float32 mmol/L
  --true outputs/ensemble_true.npy   (N,) float32 mmol/L
或：
  --pred outputs/glucose_int8_pred.npy --true outputs/ensemble_true.npy

输出：
  outputs/clinical_report.html      单页 HTML 报告（含所有指标 + 嵌入两张 PNG）
  outputs/clarke_ega.png            Clarke Error Grid 散点图
  outputs/bland_altman.png          Bland-Altman 一致性图
  outputs/clinical_metrics.json     机读指标 + gates.precision PASS/FAIL

闸门（spec.md AC-2b）：
  precision_pass = MAE ≤ 1.0 mmol/L AND Clarke A+B ≥ 90%
  iso_15197_pass = 见 ISO 标准

Exit code:
  0 = 报告写完，闸门 PASS
  1 = 报告写完，但闸门 FAIL（让上层决策是降级目标还是回退）
  2 = 依赖 / 输入错误
"""

from __future__ import annotations

import argparse
import base64
import json
import sys
from pathlib import Path

import numpy as np

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    HAS_PLT = True
except ImportError:
    HAS_PLT = False

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
OUT_DIR = ROOT / "outputs"


# ── 1. Clarke Error Grid（标准坐标 mg/dL） ───────────────────────────


def clarke_zones(y_true_mmol: np.ndarray, y_pred_mmol: np.ndarray) -> tuple[dict, np.ndarray]:
    """五区分布 + 每个样本归属区。"""
    y_true = y_true_mmol * 18.018
    y_pred = y_pred_mmol * 18.018
    zones_count = {"A": 0, "B": 0, "C": 0, "D": 0, "E": 0}
    per_sample = []
    for ref, pred in zip(y_true, y_pred):
        if (ref <= 70 and pred <= 70) or abs(pred - ref) / max(ref, 1e-6) <= 0.20:
            z = "A"
        elif (ref > 180 and pred < 70) or (ref < 70 and pred > 180):
            z = "E"
        elif (ref >= 240 and 70 < pred < 180) or (ref <= 70 and 70 < pred < 180):
            z = "D"
        elif (70 <= ref <= 180 and pred > 180) or (70 <= ref <= 180 and pred < 70):
            z = "C"
        else:
            z = "B"
        zones_count[z] += 1
        per_sample.append(z)
    n = len(y_true)
    pct = {k: v / n * 100 for k, v in zones_count.items()}
    return pct, np.array(per_sample)


def plot_clarke_ega(y_true_mmol: np.ndarray, y_pred_mmol: np.ndarray,
                    out_path: Path, zones_per_sample: np.ndarray) -> None:
    if not HAS_PLT:
        return
    y_true = y_true_mmol * 18.018
    y_pred = y_pred_mmol * 18.018
    fig, ax = plt.subplots(figsize=(7, 7))
    # 五区背景颜色（A 浅绿，B 浅黄，C/D/E 浅红）
    color_map = {"A": "#3a8", "B": "#cc4", "C": "#e88", "D": "#d44", "E": "#a11"}
    colors = [color_map[z] for z in zones_per_sample]
    ax.scatter(y_true, y_pred, c=colors, alpha=0.7, s=30, edgecolors="k", linewidths=0.5)
    # 1:1 参考线
    lim = max(y_true.max(), y_pred.max()) * 1.05
    ax.plot([0, lim], [0, lim], "k--", alpha=0.3, lw=1)
    # ±20% 区
    xs = np.linspace(0, lim, 100)
    ax.plot(xs, xs * 1.2, "k:", alpha=0.2, lw=1)
    ax.plot(xs, xs * 0.8, "k:", alpha=0.2, lw=1)
    # 区域边界（70 / 180 / 240 mg/dL）
    for v in [70, 180, 240]:
        ax.axvline(v, c="gray", alpha=0.2, lw=0.5)
        ax.axhline(v, c="gray", alpha=0.2, lw=0.5)
    ax.set_xlim(0, lim); ax.set_ylim(0, lim)
    ax.set_xlabel("Reference Glucose (mg/dL)")
    ax.set_ylabel("Predicted Glucose (mg/dL)")
    ax.set_title("Clarke Error Grid Analysis")
    ax.set_aspect("equal")
    # 图例
    from matplotlib.patches import Patch
    handles = [Patch(color=color_map[z], label=f"Zone {z}") for z in ["A", "B", "C", "D", "E"]]
    ax.legend(handles=handles, loc="lower right", fontsize=8)
    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    plt.close(fig)


# ── 2. Bland-Altman ──────────────────────────────────────────────────


def bland_altman_stats(y_true: np.ndarray, y_pred: np.ndarray) -> dict:
    diff = y_pred - y_true
    mean = (y_true + y_pred) / 2
    bias = float(diff.mean())
    sd = float(diff.std())
    loa_upper = bias + 1.96 * sd
    loa_lower = bias - 1.96 * sd
    return {
        "bias_mmol": bias,
        "sd_mmol": sd,
        "loa_upper_mmol": loa_upper,
        "loa_lower_mmol": loa_lower,
        "mean_arr": mean,
        "diff_arr": diff,
    }


def plot_bland_altman(stats: dict, out_path: Path) -> None:
    if not HAS_PLT:
        return
    mean, diff = stats["mean_arr"], stats["diff_arr"]
    fig, ax = plt.subplots(figsize=(7, 5))
    ax.scatter(mean, diff, alpha=0.6, s=30, edgecolors="k", linewidths=0.4)
    ax.axhline(stats["bias_mmol"], c="r", lw=1.5, label=f"Mean bias = {stats['bias_mmol']:+.3f}")
    ax.axhline(stats["loa_upper_mmol"], c="g", lw=1, ls="--",
               label=f"+1.96σ = {stats['loa_upper_mmol']:+.3f}")
    ax.axhline(stats["loa_lower_mmol"], c="g", lw=1, ls="--",
               label=f"-1.96σ = {stats['loa_lower_mmol']:+.3f}")
    ax.set_xlabel("Mean of Reference and Predicted (mmol/L)")
    ax.set_ylabel("Predicted - Reference (mmol/L)")
    ax.set_title("Bland-Altman Plot")
    ax.legend(loc="best", fontsize=8)
    ax.grid(alpha=0.2)
    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    plt.close(fig)


# ── 3. ISO 15197:2013 ────────────────────────────────────────────────


def iso_15197_2013(y_true_mmol: np.ndarray, y_pred_mmol: np.ndarray) -> dict:
    """ISO 15197:2013 5.6 准确度：
      - y_true < 5.55 mmol/L: 95% 样本误差 < ±0.83 mmol/L
      - y_true ≥ 5.55 mmol/L: 95% 样本相对误差 < ±15%
    """
    low_mask = y_true_mmol < 5.55
    high_mask = ~low_mask

    low_pass = np.abs(y_pred_mmol[low_mask] - y_true_mmol[low_mask]) < 0.83
    high_rel = np.abs(y_pred_mmol[high_mask] - y_true_mmol[high_mask]) / y_true_mmol[high_mask]
    high_pass = high_rel < 0.15

    n_low = int(low_mask.sum())
    n_high = int(high_mask.sum())
    low_pct = float(low_pass.mean() * 100) if n_low else 100.0
    high_pct = float(high_pass.mean() * 100) if n_high else 100.0

    # 综合：所有样本 95% 通过 → ISO PASS
    all_pass_count = int(low_pass.sum() + high_pass.sum())
    all_pct = float(all_pass_count / len(y_true_mmol) * 100)

    return {
        "low_samples": n_low,
        "low_pass_pct": low_pct,
        "high_samples": n_high,
        "high_pass_pct": high_pct,
        "overall_pass_pct": all_pct,
        "iso_pass": (low_pct >= 95.0 and high_pct >= 95.0),
    }


# ── 4. 综合指标 ───────────────────────────────────────────────────────


def basic_metrics(y_true: np.ndarray, y_pred: np.ndarray) -> dict:
    err = y_pred - y_true
    mae = float(np.mean(np.abs(err)))
    rmse = float(np.sqrt(np.mean(err**2)))
    mape = float(np.mean(np.abs(err / (y_true + 1e-6))) * 100)
    # R²
    ss_res = float(((y_true - y_pred) ** 2).sum())
    ss_tot = float(((y_true - y_true.mean()) ** 2).sum())
    r2 = 1 - ss_res / ss_tot if ss_tot > 1e-9 else float("nan")
    # Pearson
    pearson = float(np.corrcoef(y_true, y_pred)[0, 1]) if len(y_true) > 1 else float("nan")
    return {
        "n_samples": int(len(y_true)),
        "MAE_mmol": mae,
        "RMSE_mmol": rmse,
        "MAPE_pct": mape,
        "R2": r2,
        "Pearson_r": pearson,
    }


# ── 5. HTML 报告 ─────────────────────────────────────────────────────


def _png_to_base64(p: Path) -> str:
    if not p.exists():
        return ""
    return base64.b64encode(p.read_bytes()).decode("ascii")


def build_html_report(metrics: dict, clarke_pct: dict, ba: dict, iso: dict,
                      gates: dict, png_clarke: Path, png_ba: Path,
                      out_html: Path) -> None:
    img_clarke = _png_to_base64(png_clarke)
    img_ba = _png_to_base64(png_ba)

    def gate_badge(passed: bool, label: str) -> str:
        color = "#2a8" if passed else "#d44"
        return (
            f'<span style="background:{color};color:white;padding:3px 10px;'
            f'border-radius:8px;font-weight:bold;">{label}: {"PASS" if passed else "FAIL"}</span>'
        )

    html = f"""<!DOCTYPE html>
<html lang="zh"><head>
<meta charset="utf-8">
<title>Glucose 临床评估报告</title>
<style>
  body {{ font-family: -apple-system, "Segoe UI", sans-serif; max-width: 900px; margin: 24px auto; padding: 0 20px; color: #222; }}
  h1 {{ border-bottom: 2px solid #346; padding-bottom: 6px; }}
  h2 {{ border-bottom: 1px solid #ccc; margin-top: 28px; padding-bottom: 4px; color: #346; }}
  table {{ border-collapse: collapse; margin: 10px 0; }}
  td, th {{ padding: 6px 14px; border: 1px solid #ccc; }}
  th {{ background: #eef; }}
  .gate-row {{ font-size: 1.1em; margin: 14px 0; }}
  .num {{ font-family: Consolas, monospace; }}
  img {{ max-width: 100%; border: 1px solid #ddd; padding: 4px; background: #fafafa; }}
  .pass {{ color: #2a8; }} .fail {{ color: #d44; }}
</style></head><body>

<h1>Glucose 临床评估报告 — A++ pipeline</h1>
<div style="color:#666;font-size:0.9em;">生成时间: {Path(out_html).stat().st_mtime if out_html.exists() else 'now'}</div>

<div class="gate-row">
  {gate_badge(gates['precision'], 'AC-2b 精准化闸门')}
  {gate_badge(iso['iso_pass'], 'ISO 15197:2013')}
</div>

<h2>1. 基础回归指标</h2>
<table>
<tr><th>指标</th><th>值</th></tr>
<tr><td>样本数</td><td class="num">{metrics['n_samples']}</td></tr>
<tr><td>MAE (mmol/L)</td><td class="num">{metrics['MAE_mmol']:.3f}</td></tr>
<tr><td>RMSE (mmol/L)</td><td class="num">{metrics['RMSE_mmol']:.3f}</td></tr>
<tr><td>MAPE (%)</td><td class="num">{metrics['MAPE_pct']:.2f}</td></tr>
<tr><td>R²</td><td class="num">{metrics['R2']:.4f}</td></tr>
<tr><td>Pearson r</td><td class="num">{metrics['Pearson_r']:.4f}</td></tr>
</table>

<h2>2. Clarke Error Grid Analysis</h2>
<table>
<tr><th>区</th><th>比例</th><th>含义</th></tr>
<tr><td>A</td><td class="num">{clarke_pct['A']:.2f}%</td><td>临床准确</td></tr>
<tr><td>B</td><td class="num">{clarke_pct['B']:.2f}%</td><td>良性误差</td></tr>
<tr><td>C</td><td class="num">{clarke_pct['C']:.2f}%</td><td>过度治疗</td></tr>
<tr><td>D</td><td class="num">{clarke_pct['D']:.2f}%</td><td>漏诊</td></tr>
<tr><td>E</td><td class="num">{clarke_pct['E']:.2f}%</td><td>危险</td></tr>
<tr><td><b>A+B（临床可接受）</b></td>
    <td class="num"><b>{clarke_pct['A']+clarke_pct['B']:.2f}%</b></td>
    <td>目标 ≥ 90%</td></tr>
</table>
{f'<img src="data:image/png;base64,{img_clarke}" alt="Clarke EGA">' if img_clarke else '<p style="color:#888;">[matplotlib 未装，跳过散点图]</p>'}

<h2>3. Bland-Altman 一致性</h2>
<table>
<tr><th>统计</th><th>值 (mmol/L)</th></tr>
<tr><td>Mean bias</td><td class="num">{ba['bias_mmol']:+.3f}</td></tr>
<tr><td>SD</td><td class="num">{ba['sd_mmol']:.3f}</td></tr>
<tr><td>LoA upper (+1.96σ)</td><td class="num">{ba['loa_upper_mmol']:+.3f}</td></tr>
<tr><td>LoA lower (-1.96σ)</td><td class="num">{ba['loa_lower_mmol']:+.3f}</td></tr>
</table>
{f'<img src="data:image/png;base64,{img_ba}" alt="Bland-Altman">' if img_ba else ''}

<h2>4. ISO 15197:2013 准确度</h2>
<table>
<tr><th>段</th><th>样本数</th><th>通过率</th><th>标准</th></tr>
<tr><td>低血糖 (&lt; 5.55 mmol/L)</td>
    <td class="num">{iso['low_samples']}</td>
    <td class="num">{iso['low_pass_pct']:.1f}%</td>
    <td>误差 &lt; ±0.83 mmol/L</td></tr>
<tr><td>正常+高血糖 (≥ 5.55 mmol/L)</td>
    <td class="num">{iso['high_samples']}</td>
    <td class="num">{iso['high_pass_pct']:.1f}%</td>
    <td>相对误差 &lt; ±15%</td></tr>
<tr><td><b>整体通过率</b></td>
    <td colspan="2" class="num"><b>{iso['overall_pass_pct']:.1f}%</b></td>
    <td>目标 ≥ 95%（每段）</td></tr>
</table>

<h2>5. 闸门判定</h2>
<table>
<tr><th>闸门</th><th>条件</th><th>结果</th></tr>
<tr><td>AC-2b 精准化</td><td>MAE ≤ 1.0 AND Clarke A+B ≥ 90%</td>
    <td class="{'pass' if gates['precision'] else 'fail'}">
        {'PASS' if gates['precision'] else 'FAIL'}</td></tr>
<tr><td>ISO 15197:2013</td><td>两段通过率均 ≥ 95%</td>
    <td class="{'pass' if iso['iso_pass'] else 'fail'}">
        {'PASS' if iso['iso_pass'] else 'FAIL'}</td></tr>
</table>

<p style="color:#666;font-size:0.85em;margin-top:32px;">
本报告由 glucose_training/scripts/eval_clinical.py 生成。详见 outputs/clinical_metrics.json。
</p>
</body></html>"""
    out_html.write_text(html, encoding="utf-8")


# ── CLI ───────────────────────────────────────────────────────────────


def evaluate(y_true: np.ndarray, y_pred: np.ndarray, out_dir: Path) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    png_clarke = out_dir / "clarke_ega.png"
    png_ba = out_dir / "bland_altman.png"
    html = out_dir / "clinical_report.html"
    json_path = out_dir / "clinical_metrics.json"

    metrics = basic_metrics(y_true, y_pred)
    clarke_pct, zones_per_sample = clarke_zones(y_true, y_pred)
    ba_stats = bland_altman_stats(y_true, y_pred)
    iso = iso_15197_2013(y_true, y_pred)

    # AC-2b 闸门
    gates = {
        "precision": (metrics["MAE_mmol"] <= 1.0 and
                      (clarke_pct["A"] + clarke_pct["B"]) >= 90.0),
    }

    # 绘图
    if HAS_PLT:
        plot_clarke_ega(y_true, y_pred, png_clarke, zones_per_sample)
        plot_bland_altman(ba_stats, png_ba)
    else:
        print("[warn] matplotlib 未装，跳过图表")

    # 保存
    report_json = {
        "metrics": metrics,
        "clarke": clarke_pct,
        "bland_altman": {k: v for k, v in ba_stats.items() if not k.endswith("_arr")},
        "iso_15197": {k: v for k, v in iso.items() if k != "overall_pass_pct" or True},
        "gates": gates,
    }
    json_path.write_text(json.dumps(report_json, indent=2, ensure_ascii=False))
    build_html_report(metrics, clarke_pct, ba_stats, iso, gates,
                      png_clarke, png_ba, html)

    # 控制台摘要
    print("\n=== 临床评估摘要 ===")
    print(f"  N = {metrics['n_samples']}")
    print(f"  MAE  = {metrics['MAE_mmol']:.3f} mmol/L")
    print(f"  RMSE = {metrics['RMSE_mmol']:.3f} mmol/L")
    print(f"  Clarke A+B = {clarke_pct['A'] + clarke_pct['B']:.2f}%  (D+E = {clarke_pct['D']+clarke_pct['E']:.2f}%)")
    print(f"  ISO 15197 low/high = {iso['low_pass_pct']:.1f}% / {iso['high_pass_pct']:.1f}%")
    print(f"\n  [AC-2b 精准化闸门]   {'PASS' if gates['precision'] else 'FAIL'}")
    print(f"  [ISO 15197:2013]      {'PASS' if iso['iso_pass'] else 'FAIL'}")
    print(f"\n[save] {html}")
    print(f"[save] {json_path}")
    if HAS_PLT:
        print(f"[save] {png_clarke}")
        print(f"[save] {png_ba}")

    return 0 if gates["precision"] else 1


def _selftest() -> int:
    """无数据时的自检：构造 fake y_true/y_pred 验证全流程不挂。"""
    print("[selftest] eval_clinical.py 自检")
    rng = np.random.default_rng(42)
    y_true = rng.uniform(4.0, 12.0, size=50).astype(np.float32)
    y_pred = y_true + rng.normal(0, 0.8, size=50).astype(np.float32)
    return evaluate(y_true, y_pred, OUT_DIR)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pred", type=Path, default=OUT_DIR / "ensemble_pred.npy",
                        help="预测 .npy")
    parser.add_argument("--true", type=Path, default=OUT_DIR / "ensemble_true.npy",
                        help="真值 .npy")
    parser.add_argument("--out-dir", type=Path, default=OUT_DIR,
                        help="报告输出目录")
    parser.add_argument("--selftest", action="store_true", help="跑自检")
    args = parser.parse_args()

    if args.selftest:
        sys.exit(_selftest())

    if not args.pred.exists() or not args.true.exists():
        print(f"[err] 找不到 {args.pred} 或 {args.true}\n请先跑 train.py")
        sys.exit(2)

    y_true = np.load(args.true).astype(np.float32).squeeze()
    y_pred = np.load(args.pred).astype(np.float32).squeeze()
    if y_true.shape != y_pred.shape:
        print(f"[err] shape 不匹配: true={y_true.shape}, pred={y_pred.shape}")
        sys.exit(2)

    sys.exit(evaluate(y_true, y_pred, args.out_dir))


if __name__ == "__main__":
    main()
