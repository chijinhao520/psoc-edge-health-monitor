#!/usr/bin/env python3
"""
data_mitbih.py — MIT-BIH Arrhythmia DB → AAMI 五类切拍 → DS1/DS2 npz

流程：
  wfdb 脚本化下载 44 记录(排除 4 起搏记录)
    → 逐记录读 MLII 导联 + 注释
    → 每个标注心拍按 R 峰对齐切 256 样本窗(@360Hz ≈ 0.71s)
    → 按 AAMI EC57 映射成 5 类 (N/S/V/F/Q)
    → inter-patient DS1/DS2 划分(de Chazal split,防同病人泄漏)
    → data/processed/mitbih.npz

用法：
    python scripts/data_mitbih.py              # 下载 + 处理(默认)
    python scripts/data_mitbih.py --process    # 只处理(已下载)

输出 npz：
    X_train  (N1, 256, 1) float32 ∈ [0,1]   ECG 波形，每拍 min-max 归一
    RR_train (N1, 3)      float32 ∈ [0,1]   R-R 特征 [pre/med, post/med, pre/local]
    y_train  (N1,)        int64  0=N 1=S 2=V 3=F 4=Q
    X_test/RR_test/y_test (N2, …)
    classes ['N','S','V','F','Q']

> ⚠️ MIT-BIH 是 ECG，部署端对应 AD8232。训练用 MIT-BIH 原生 360Hz，
>   部署时把 AD8232 500Hz 信号 scipy.signal.resample 到 360Hz 再切窗对齐。
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

# ── 路径 ───────────────────────────────────────────────────────────────
ROOT = Path(__file__).resolve().parent.parent
RAW_DIR = ROOT / "data" / "raw" / "mitdb"
OUT_NPZ = ROOT / "data" / "processed" / "mitbih.npz"

# ── 配置 ───────────────────────────────────────────────────────────────
DB_NAME = "mitdb"
FS = 360                 # MIT-BIH 原生采样率
WIN_HALF = 128           # R 峰前后各 128 → 窗长 256
WIN_LEN = WIN_HALF * 2

# AAMI EC57 5 类映射（注释符号 → 类别）
AAMI = {
    "N": list("NLRej"),          # Normal / 束支阻滞 / 房性逸搏 / 交界逸搏
    "S": list("AaJS"),           # 室上性异位
    "V": list("VE"),             # 室性异位 + 室性逸搏
    "F": list("F"),              # 融合
    "Q": list("/fQ"),            # 起搏 / 起搏融合 / 未知
}
CLASSES = ["N", "S", "V", "F", "Q"]
SYM2CLS = {s: i for i, cls in enumerate(CLASSES) for s in AAMI[cls]}

# de Chazal inter-patient 划分（各 22 记录，已排除起搏记录 102/104/107/217）
DS1 = [101, 106, 108, 109, 112, 114, 115, 116, 118, 119, 122,
       124, 201, 203, 205, 207, 208, 209, 215, 220, 223, 230]
DS2 = [100, 103, 105, 111, 113, 117, 121, 123, 200, 202, 210,
       212, 213, 214, 219, 221, 222, 228, 231, 232, 233, 234]
ALL_RECORDS = [str(r) for r in DS1 + DS2]


# ── 下载 ───────────────────────────────────────────────────────────────
def download():
    import wfdb
    RAW_DIR.mkdir(parents=True, exist_ok=True)
    have = {p.stem for p in RAW_DIR.glob("*.hea")}
    todo = [r for r in ALL_RECORDS if r not in have]
    if not todo:
        print(f"[skip] 44 记录已全部存在于 {RAW_DIR}")
        return
    print(f"[dl] 下载 {len(todo)} 条记录到 {RAW_DIR} …")
    wfdb.dl_database(DB_NAME, dl_dir=str(RAW_DIR), records=todo)
    print("[dl] 完成")


# ── 选 MLII 导联 ───────────────────────────────────────────────────────
def _pick_lead(record):
    """优先 MLII；找不到用 channel 0。"""
    names = [n.upper() for n in record.sig_name]
    for cand in ("MLII", "ML II", "II"):
        if cand in names:
            return names.index(cand)
    return 0


# ── 单记录切拍 + R-R 特征 ─────────────────────────────────────────────
RR_LOCAL_K = 10           # 局部 R-R 均值窗口(前 K 拍)


def _segment_record(rec_id: str):
    import wfdb
    path = str(RAW_DIR / rec_id)
    record = wfdb.rdrecord(path)
    ann = wfdb.rdann(path, "atr")

    ch = _pick_lead(record)
    sig = record.p_signal[:, ch].astype(np.float32)
    n = len(sig)

    # 只保留真实心拍(在 AAMI 5 类内)，按时间顺序算 R-R
    beats = [(int(s), SYM2CLS[sym])
             for s, sym in zip(ann.sample, ann.symbol) if sym in SYM2CLS]
    if len(beats) < 3:
        return [], [], []
    samples = np.array([b[0] for b in beats])
    labels = np.array([b[1] for b in beats])
    med_rr = float(np.median(np.diff(samples)))   # 本记录中位 R-R(基准)
    med_rr = max(med_rr, 1.0)

    xs, rrs, ys = [], [], []
    for i in range(1, len(beats) - 1):            # 需前后拍 → 掐头去尾
        r = samples[i]
        a, b = r - WIN_HALF, r + WIN_HALF
        if a < 0 or b > n:
            continue                              # 边界拍丢弃
        w = sig[a:b]
        rng = w.max() - w.min()
        if rng < 1e-6:
            continue                              # 平直无效窗
        w = (w - w.min()) / rng                   # 每拍 min-max → [0,1]

        pre = samples[i] - samples[i - 1]
        post = samples[i + 1] - samples[i]
        lo = max(0, i - RR_LOCAL_K)
        local = float(np.mean(samples[lo + 1:i + 1] - samples[lo:i]))
        local = max(local, 1.0)
        # 3 个比值特征，clip[0,2]/2 → [0,1]：提前拍 pre/med<1，代偿间歇 post/med>1
        feat = np.array([
            np.clip(pre / med_rr, 0, 2) / 2.0,
            np.clip(post / med_rr, 0, 2) / 2.0,
            np.clip(pre / local, 0, 2) / 2.0,
        ], dtype=np.float32)

        xs.append(w)
        rrs.append(feat)
        ys.append(labels[i])
    return xs, rrs, ys


# ── 处理全集 ───────────────────────────────────────────────────────────
def _build(records):
    from tqdm import tqdm
    X, R, y = [], [], []
    for rec_id in tqdm(records, desc="segment"):
        xs, rrs, ys = _segment_record(rec_id)
        X.extend(xs)
        R.extend(rrs)
        y.extend(ys)
    X = np.asarray(X, dtype=np.float32)[..., None]   # (N, 256, 1)
    R = np.asarray(R, dtype=np.float32)              # (N, 3)
    y = np.asarray(y, dtype=np.int64)
    return X, R, y


def _dist(y):
    return {CLASSES[i]: int((y == i).sum()) for i in range(len(CLASSES))}


def process():
    print("[proc] 切 DS1(train) …")
    X_tr, R_tr, y_tr = _build([str(r) for r in DS1])
    print("[proc] 切 DS2(test) …")
    X_te, R_te, y_te = _build([str(r) for r in DS2])

    OUT_NPZ.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(
        OUT_NPZ,
        X_train=X_tr, RR_train=R_tr, y_train=y_tr,
        X_test=X_te, RR_test=R_te, y_test=y_te,
        classes=np.array(CLASSES),
        fs=FS, win_len=WIN_LEN,
    )
    print(f"\n[ok] 写出 {OUT_NPZ}")
    print(f"  train: X{X_tr.shape} RR{R_tr.shape} 分布 {_dist(y_tr)}")
    print(f"  test : X{X_te.shape} RR{R_te.shape} 分布 {_dist(y_te)}")
    print(f"  注：N 类占比极高(~90%),训练用 class_weight,评估看 macro-F1")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--extract", action="store_true", help="只下载")
    ap.add_argument("--process", action="store_true", help="只处理")
    args = ap.parse_args()

    do_dl = args.extract or not (args.extract or args.process)
    do_proc = args.process or not (args.extract or args.process)

    if do_dl:
        download()
    if do_proc:
        process()
    return 0


if __name__ == "__main__":
    sys.exit(main())
