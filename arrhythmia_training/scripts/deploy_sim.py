#!/usr/bin/env python3
"""
deploy_sim.py — 部署流式管线验证(C 端 arrhythmia_task.c 的 Python 真值参照)

在 MIT-BIH 真实记录上跑【与 C 端将要实现的完全相同】的流式逻辑：
  Pan-Tompkins R 峰检测 → 256 样本窗(R 对齐) → min-max → R-R 特征 → int8 tflite
然后两项验证：
  ① R 峰检测 vs 标注：sensitivity / PPV（窗内 ±75ms）
  ② 分类 vs 标注 AAMI：每类 F1（用流式 R 而非标注 R，检验端到端契约）

证明这条管线对了，才把 PT/窗/R-R 逻辑 port 成 C(arrhythmia_preproc.c)。
> MIT-BIH 本就 360Hz，这里不做 500→360 重采样(那是 AD8232 专属步骤，C 端单独实现并单测)。

用法: python scripts/deploy_sim.py --rec 200
"""
from __future__ import annotations
import argparse
import sys
from pathlib import Path
import numpy as np
import scipy.signal as sg

ROOT = Path(__file__).resolve().parent.parent
RAW = ROOT / "data" / "raw" / "mitdb"
TFLITE = ROOT / "outputs" / "arrhythmia_int8.tflite"

FS = 360
WIN_HALF = 128
RR_LOCAL_K = 10
CLASSES = ["N", "S", "V", "F"]
AAMI = {"N": list("NLRej"), "S": list("AaJS"), "V": list("VE"), "F": list("F")}
SYM2CLS = {s: i for i, c in enumerate(CLASSES) for s in AAMI[c]}


# ── Pan-Tompkins(流式，可 1:1 port 到 C) ───────────────────────────────
def pan_tompkins(sig):
    """返回检测到的 R 峰样本下标(在原始信号域)。

    流程: butterworth 5-15Hz 带通 → 5 点导数 → 平方 → 150ms 滑窗积分
          → 自适应双阈值 + 200ms 不应期 + searchback；R 精定位=原始信号窗内极大。
    """
    n = len(sig)
    # 1. 带通(2 阶 butter → sos 2 段；C 端用同系数 biquad)
    sos = sg.butter(2, [5, 15], btype="bandpass", fs=FS, output="sos")
    bp = sg.sosfilt(sos, sig)
    # 2. 5 点导数
    der = np.zeros(n)
    for i in range(4, n):
        der[i] = (2*bp[i] + bp[i-1] - bp[i-3] - 2*bp[i-4]) / 8.0
    # 3. 平方
    sq = der * der
    # 4. 因果滑窗积分 150ms(与 C 流式一致:mwi[i]=mean(sq[i-w+1..i]))
    w = int(round(0.150 * FS))
    csum = np.cumsum(sq)
    mwi = np.zeros(n)
    mwi[:w] = csum[:w] / w
    mwi[w:] = (csum[w:] - csum[:-w]) / w

    # 5. 自适应阈值峰检测
    refr = int(0.200 * FS)            # 不应期 200ms
    rr_avg = int(0.8 * FS)
    SPKI = np.max(mwi[:2*FS]) * 0.25 if n > 2*FS else np.max(mwi)*0.25
    NPKI = np.mean(mwi[:2*FS]) * 0.5 if n > 2*FS else np.mean(mwi)*0.5
    last = -refr
    peaks_mwi = []
    i = 1
    while i < n - 1:
        if mwi[i] > mwi[i-1] and mwi[i] >= mwi[i+1]:      # 局部极大
            thr1 = NPKI + 0.25 * (SPKI - NPKI)
            peak = mwi[i]
            if peak > thr1 and (i - last) > refr:
                peaks_mwi.append(i)
                SPKI = 0.125 * peak + 0.875 * SPKI
                if last > 0:
                    rr_avg = int(0.875 * rr_avg + 0.125 * (i - last))
                last = i
            else:
                NPKI = 0.125 * peak + 0.875 * NPKI
                # searchback: 超过 1.66*RR 没峰，用半阈值回扫
                if last > 0 and (i - last) > int(1.66 * rr_avg):
                    thr2 = 0.5 * thr1
                    seg = mwi[last+refr:i]
                    if len(seg) and seg.max() > thr2:
                        j = last + refr + int(seg.argmax())
                        peaks_mwi.append(j)
                        SPKI = 0.25 * mwi[j] + 0.75 * SPKI
                        last = j
        i += 1

    # 6. R 精定位:因果 MWI 峰回溯到原始信号窗内幅值极大(窗覆盖因果群延迟 ~w)
    back = w + 30                      # 因果 MWI 延迟 ~w,留余量
    rpeaks = []
    for p in peaks_mwi:
        a = max(0, p - back)
        b = min(n, p + 5)
        if b - a < 3:
            continue
        r = a + int(np.argmax(np.abs(sig[a:b] - np.mean(sig[a:b]))))
        rpeaks.append(r)
    # 去重(可能精定位到同一点)
    out = []
    for r in rpeaks:
        if not out or r - out[-1] > refr // 2:
            out.append(r)
    return np.array(out, dtype=int)


# ── R-R 特征(与 data_mitbih.py 完全一致) ──────────────────────────────
def rr_feats(rpeaks):
    """逐拍返回 [pre/med, post/med, pre/local]，med/local 用流式可得的运行统计。"""
    feats = {}
    if len(rpeaks) < 3:
        return feats
    rr = np.diff(rpeaks)
    med_run = max(float(np.median(rr)), 1.0)   # C 端用最近 N 拍运行中位数近似"记录中位"
    for i in range(1, len(rpeaks) - 1):
        pre = rpeaks[i] - rpeaks[i-1]
        post = rpeaks[i+1] - rpeaks[i]
        lo = max(0, i - RR_LOCAL_K)
        local = max(float(np.mean(rpeaks[lo+1:i+1] - rpeaks[lo:i])), 1.0)
        feats[i] = (
            np.clip(pre/med_run, 0, 2)/2.0,
            np.clip(post/med_run, 0, 2)/2.0,
            np.clip(pre/local, 0, 2)/2.0,
        )
    return feats


def linear_resample_500_to_360(x500):
    """C 端 arr_preproc_push_500 重采样逻辑的 Python 镜像(线性插值)。"""
    ratio = 360.0 / 500.0
    acc = 0.0
    prev = x500[0]
    out = []
    for s in x500[1:]:
        acc += ratio
        if acc >= 1.0:
            acc -= 1.0
            f = 1.0 - acc
            out.append(prev + (s - prev) * f)
        prev = s
    return np.array(out)


def main():
    import wfdb
    import scipy.signal as _sg
    from sklearn.metrics import classification_report, f1_score
    ap = argparse.ArgumentParser()
    ap.add_argument("--rec", default="200")
    ap.add_argument("--from500", action="store_true",
                    help="模拟完整 AD8232 路径:先上采样到 500 再线性重采样回 360")
    args = ap.parse_args()

    import tensorflow as tf
    rec = wfdb.rdrecord(str(RAW / args.rec))
    ann = wfdb.rdann(str(RAW / args.rec), "atr")
    names = [s.upper() for s in rec.sig_name]
    ch = names.index("MLII") if "MLII" in names else 0
    sig = rec.p_signal[:, ch].astype(np.float64)

    if args.from500:                       # 完整 AD8232 路径:360→500→(C 线性重采样)→360
        sig = linear_resample_500_to_360(_sg.resample_poly(sig, 500, 360))
        print(f"[rec {args.rec}] --from500: 经 500Hz 往返重采样,len={len(sig)}")

    # 真值心拍
    true_r = np.array([s for s, sym in zip(ann.sample, ann.symbol) if sym in SYM2CLS])
    true_c = np.array([SYM2CLS[sym] for sym in ann.symbol if sym in SYM2CLS])

    # ① 流式 R 峰检测
    det_r = pan_tompkins(sig)
    tol = int(0.075 * FS)
    matched = 0
    for r in det_r:
        if len(true_r) and np.min(np.abs(true_r - r)) <= tol:
            matched += 1
    se = matched / len(true_r) * 100 if len(true_r) else 0
    pp = matched / len(det_r) * 100 if len(det_r) else 0
    print(f"[rec {args.rec}] R 峰检测: 真值 {len(true_r)}  检出 {len(det_r)}  "
          f"匹配 {matched}  →  Se={se:.1f}%  PPV={pp:.1f}%")

    # ② 端到端分类(用流式 R)
    interp = tf.lite.Interpreter(model_path=str(TFLITE))
    interp.allocate_tensors()
    ins = interp.get_input_details()
    out = interp.get_output_details()[0]
    ecg_d = next(t for t in ins if "ecg" in t["name"].lower())
    rr_d = next(t for t in ins if "rr" in t["name"].lower())

    def q8(x, det):
        s, zp = det["quantization"]
        return np.round(x/s + zp).clip(-128, 127).astype(np.int8)

    feats = rr_feats(det_r)
    y_true, y_pred = [], []
    n = len(sig)
    for i, r in enumerate(det_r):
        if i not in feats:
            continue
        a, b = r - WIN_HALF, r + WIN_HALF
        if a < 0 or b > n:
            continue
        w = sig[a:b].astype(np.float32)
        rng = w.max() - w.min()
        if rng < 1e-6:
            continue
        w = (w - w.min()) / rng
        f = np.array(feats[i], dtype=np.float32)
        interp.set_tensor(ecg_d["index"], q8(w[None, :, None], ecg_d))
        interp.set_tensor(rr_d["index"], q8(f[None, :], rr_d))
        interp.invoke()
        pred = int(interp.get_tensor(out["index"])[0].argmax())
        # 匹配最近真值标签
        j = int(np.argmin(np.abs(true_r - r)))
        if abs(true_r[j] - r) <= tol:
            y_true.append(int(true_c[j]))
            y_pred.append(pred)

    y_true, y_pred = np.array(y_true), np.array(y_pred)
    print(f"[rec {args.rec}] 端到端分类(流式 R): 评估 {len(y_true)} 拍")
    print(classification_report(y_true, y_pred, labels=[0, 1, 2, 3],
                                target_names=CLASSES, digits=3, zero_division=0))
    print(f"  macro-F1 = {f1_score(y_true, y_pred, average='macro', labels=[0,1,2,3], zero_division=0):.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
