#!/usr/bin/env python3
"""
data_merge.py — 合并 MUST + Kaggle .npz 到 multi.npz

输入（自动扫描 data/processed/）：
  must.npz                — data_must.py 产物（必须有 X_ppg/X_scal/y）
  kaggle_*.npz            — data_kaggle.py 产物（带 subject_id/scalar_mask）

输出：
  data/processed/multi.npz 含以下字段
    X_ppg (N, 128, 2)       float32 [0,1]
    X_scal (N, 7)           float32
    y (N,)                  float32  mmol/L
    subject_id (N,)         str      '<source>:<orig_id>'
    source (N,)             str      'must' / 'kaggle_<name>'
    scalar_mask (N, 7)      uint8    1=真实/0=默认填充

兼容性：
  旧版 must.npz 无 subject_id / scalar_mask → 自动补
    subject_id = 'must:auto_<i>'（每 N 样本一组归为同 subject，模拟受试者）
    scalar_mask = [1,0,0,0,0,0,0]（只有 HR 现算是真实，其他默认）

闸门（spec.md Gate 阶段 2）：
  总样本 ≥ 150 → 通过
  否则警告但仍输出（不阻塞 task）

用法:
  python data_merge.py                       # 自动扫描
  python data_merge.py --sources must.npz kaggle_a.npz  # 显式指定
  python data_merge.py --out custom.npz
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np


# ── 配置 ──────────────────────────────────────────────────────────────
HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
PROC_DIR = ROOT / "data" / "processed"

GATE_MIN_SAMPLES = 150               # spec Gate 阶段 2 闸门
MUST_SUBJECT_GROUP_SIZE = 3          # 旧 must.npz 每 N 窗当一个虚拟 subject


# ── 数据加载与补全 ───────────────────────────────────────────────────
def load_with_compat(npz_path: Path) -> dict:
    """加载 .npz，自动补齐缺失字段。"""
    d = np.load(npz_path, allow_pickle=True)
    keys = set(d.files)
    out = {}

    # 必须字段
    for k in ("X_ppg", "X_scal", "y"):
        if k not in keys:
            raise ValueError(f"{npz_path.name}: 缺关键字段 {k}")
        out[k] = d[k].astype(np.float32) if k != "y" else d["y"].astype(np.float32)

    n = len(out["y"])
    src_name = npz_path.stem                  # 'must' / 'kaggle_xxx'

    # subject_id：缺则按组生成虚拟 ID
    if "subject_id" in keys:
        out["subject_id"] = np.asarray(d["subject_id"]).astype(object)
    else:
        # must.npz 滑窗后样本来自同少数 subject；这里假定连续 group_size 窗同一人
        sids = []
        for i in range(n):
            grp = i // MUST_SUBJECT_GROUP_SIZE
            sids.append(f"{src_name}:auto_{grp}")
        out["subject_id"] = np.array(sids, dtype=object)

    # source
    if "source" in keys:
        out["source"] = np.asarray(d["source"]).astype(object)
    else:
        out["source"] = np.array([src_name] * n, dtype=object)

    # scalar_mask
    if "scalar_mask" in keys:
        out["scalar_mask"] = d["scalar_mask"].astype(np.uint8)
    else:
        # 旧 must.npz：只有 HR 现算 + 其他默认
        mask = np.zeros((n, 7), dtype=np.uint8)
        mask[:, 0] = 1                        # HR 现算
        out["scalar_mask"] = mask

    return out


# ── 直方图 ────────────────────────────────────────────────────────────
def ascii_hist(values: np.ndarray, bins: int = 20, width: int = 50) -> str:
    counts, edges = np.histogram(values, bins=bins)
    max_c = counts.max() or 1
    lines = []
    for i, c in enumerate(counts):
        bar = "#" * int(c / max_c * width)
        lines.append(f"  [{edges[i]:5.2f}-{edges[i+1]:5.2f}] {bar} {c}")
    return "\n".join(lines)


# ── 主流程 ────────────────────────────────────────────────────────────
def discover_sources(proc_dir: Path) -> list[Path]:
    """自动发现可合并 .npz 文件（must.npz + kaggle_*.npz）。"""
    cands = []
    must = proc_dir / "must.npz"
    if must.exists():
        cands.append(must)
    for p in sorted(proc_dir.glob("kaggle_*.npz")):
        cands.append(p)
    # 也可包含 real.npz（finetune 阶段，5/1 真采血后）
    real = proc_dir / "real.npz"
    if real.exists():
        cands.append(real)
    return cands


def merge(sources: list[Path]) -> dict:
    if not sources:
        print("[err] 没有可合并的 .npz；请先跑 data_must.py / data_kaggle.py")
        sys.exit(1)

    print(f"[merge] 合并 {len(sources)} 源:")
    loaded = []
    for p in sources:
        print(f"  - {p.name}")
        try:
            d = load_with_compat(p)
            loaded.append(d)
            print(
                f"      N={len(d['y'])}  subjects={len(np.unique(d['subject_id']))}  "
                f"glucose [{d['y'].min():.2f}, {d['y'].max():.2f}]"
            )
        except Exception as e:
            print(f"      [skip] {e}")

    if not loaded:
        print("[err] 所有源加载失败")
        sys.exit(1)

    # 校验 shape 一致
    ref_shape = loaded[0]["X_ppg"].shape[1:]
    for d in loaded[1:]:
        if d["X_ppg"].shape[1:] != ref_shape:
            print(
                f"[err] X_ppg shape 不一致: {ref_shape} vs {d['X_ppg'].shape[1:]}"
            )
            sys.exit(1)

    merged = {
        "X_ppg":       np.concatenate([d["X_ppg"]       for d in loaded]),
        "X_scal":      np.concatenate([d["X_scal"]      for d in loaded]),
        "y":           np.concatenate([d["y"]           for d in loaded]),
        "subject_id":  np.concatenate([d["subject_id"]  for d in loaded]),
        "source":      np.concatenate([d["source"]      for d in loaded]),
        "scalar_mask": np.concatenate([d["scalar_mask"] for d in loaded]),
    }

    return merged


def report(data: dict) -> bool:
    """打印统计 + 直方图，返回是否过闸门。"""
    n = len(data["y"])
    n_sub = len(np.unique(data["subject_id"]))

    print("\n=== merged dataset stats ===")
    print(f"  total samples : {n}")
    print(f"  unique subjects: {n_sub}")
    print(f"  X_ppg shape    : {data['X_ppg'].shape}")
    print(f"  X_scal shape   : {data['X_scal'].shape}")
    print(
        f"  glucose range  : [{data['y'].min():.2f}, {data['y'].max():.2f}]  "
        f"mean {data['y'].mean():.2f} ± {data['y'].std():.2f}"
    )

    # 按 source 分布
    src_arr = data["source"]
    unique_src, counts = np.unique(src_arr, return_counts=True)
    print("\n  per source:")
    for s, c in zip(unique_src, counts):
        print(f"    {s:30s} : {c} samples ({c/n*100:.1f}%)")

    # 血糖直方图
    print("\n  glucose histogram (mmol/L):")
    print(ascii_hist(data["y"], bins=20, width=40))

    # 闸门
    passed = n >= GATE_MIN_SAMPLES
    print(
        f"\n  [Gate 阶段 2] 样本量 {n} {'>=' if passed else '<'} "
        f"{GATE_MIN_SAMPLES} → {'PASS' if passed else 'WARN (但不阻塞)'}"
    )
    return passed


def save(data: dict, out: Path) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(
        out,
        X_ppg=data["X_ppg"],
        X_scal=data["X_scal"],
        y=data["y"],
        subject_id=data["subject_id"],
        source=data["source"],
        scalar_mask=data["scalar_mask"],
        meta=np.array(
            [
                "merged via data_merge.py",
                f"total={len(data['y'])}",
                f"subjects={len(np.unique(data['subject_id']))}",
                f"sources={','.join(np.unique(data['source']).tolist())}",
            ]
        ),
    )
    size_kb = out.stat().st_size / 1024
    print(f"\n[save] → {out} ({size_kb:.1f} KB)")


# ── CLI ───────────────────────────────────────────────────────────────


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "--sources", nargs="+", type=Path, default=None,
        help="显式指定要合并的 .npz 文件路径（不传则自动扫描 data/processed/）",
    )
    parser.add_argument(
        "--out", type=Path, default=PROC_DIR / "multi.npz",
        help="输出 .npz 路径",
    )
    parser.add_argument(
        "--no-gate", action="store_true",
        help="即使样本不够也不返回非零 exit code",
    )
    args = parser.parse_args()

    if args.sources is None:
        sources = discover_sources(PROC_DIR)
    else:
        sources = [Path(p) for p in args.sources]

    merged = merge(sources)
    passed = report(merged)
    save(merged, args.out)

    if not passed and not args.no_gate:
        # 不阻塞 — 仅警告退出 0；spec 已说明闸门不阻塞 task
        pass
    sys.exit(0)


if __name__ == "__main__":
    main()
