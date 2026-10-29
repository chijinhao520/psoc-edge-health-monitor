#!/usr/bin/env python3
"""
data_kaggle.py — Kaggle PPG-glucose 数据集自适应加载器

支持两个 Kaggle 数据集（A++ 数据多源融合）：
  1. mhamza1018/ppg-glucose-dataset
  2. muhammadyasirsaleem/ppg-signal-with-blood-sugar-level-data

Kaggle 数据集格式异构，本 loader 用启发式嗅探：
  - 自动识别 PPG 信号列（最长数值列）
  - 自动识别 glucose 列（名称含 glucose/blood_sugar/bg_level）
  - 单位归一：mg/dL (>20) → mmol/L (/18.018)
  - 采样率嗅探：metadata.txt / 文件名启发 / 默认 100 Hz（KAGGLE_FS env 覆盖）
  - 受试者 ID：从文件名 / subject_id 列 / 默认从 row index 推断
  - 重采样 → 100 Hz → 5s 窗 → 128 下采样 → [0,1] 归一化 → 单通道复制成双通道
  - 标量字段：若 CSV 含 age/HR/BP 等，做合理映射；缺失用 spec 默认值 + scalar_mask

输出 .npz 含：
  X_ppg (N, 128, 2) float32 [0,1]
  X_scal (N, 7) float32     — 与 model.py 7 维契约一致
  y (N,) float32            — mmol/L
  subject_id (N,) str       — 受试者 ID（用于 GroupKFold）
  source (N,) str           — 'kaggle_<name>'
  scalar_mask (N, 7) uint8  — 1=真实值 / 0=默认填充

用法:
  python data_kaggle.py --src-dir data/raw/kaggle_mhamza1018 --out data/processed/kaggle_mhamza1018.npz
  python data_kaggle.py --src-dir data/raw/kaggle_yasirsaleem --out data/processed/kaggle_yasirsaleem.npz
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

import numpy as np

try:
    import pandas as pd
except ImportError:
    pd = None

try:
    from scipy import signal as sp_signal
except ImportError:
    sp_signal = None


# ── 配置（对齐项目契约） ─────────────────────────────────────────────
TARGET_FS = 100
WINDOW_SEC = 5
WINDOW_RAW = TARGET_FS * WINDOW_SEC          # 500 点
WINDOW_DS = 128
WINDOW_STRIDE_SEC = 2.5                      # 50% 重叠

DEFAULT_FS = 100

# 7 维标量默认值（与 data_must.py 对齐）
SCAL_DEFAULTS = {
    "HR": 75.0,
    "SpO2": 98.0,
    "BV": 1.0,
    "t_ir": 36.5,
    "t_env": 25.0,
    "h_env": 55.0,
    "BMR": 65.0,
}

# 列名嗅探：候选关键词
GLUCOSE_COL_CANDIDATES = [
    "glucose", "blood_sugar", "bloodsugar", "bg_level", "bg", "blood_glucose",
]
PPG_COL_CANDIDATES = [
    "ppg", "signal", "value", "amplitude", "ir", "red", "raw",
]
SUBJECT_COL_CANDIDATES = [
    "subject_id", "subject", "patient_id", "patient", "id", "user_id",
]
HR_COL_CANDIDATES = ["hr", "heart_rate", "heartrate", "bpm"]
AGE_COL_CANDIDATES = ["age", "Age"]
GENDER_COL_CANDIDATES = ["gender", "sex"]
SBP_CANDIDATES = ["sbp", "systolic", "systolic_peak", "systolic_pressure"]
DBP_CANDIDATES = ["dbp", "diastolic", "diastolic_peak", "diastolic_pressure"]


# ── 工具 ──────────────────────────────────────────────────────────────


def hint_download(name: str, src_dir: Path) -> None:
    """打印缺失数据集的下载指引。"""
    print(
        f"\n────── Kaggle 数据集 '{name}' 下载指引 ──────\n"
        f"未在 {src_dir} 找到 CSV 文件。\n\n"
        f"步骤：\n"
        f"  1. 浏览器打开 Kaggle 数据集页（或用 kaggle CLI）\n"
        f"     - mhamza1018: https://www.kaggle.com/datasets/mhamza1018/ppg-glucose-dataset\n"
        f"     - yasirsaleem: https://www.kaggle.com/datasets/muhammadyasirsaleem/ppg-signal-with-blood-sugar-level-data\n"
        f"  2. 点 Download → 解压所有 .csv 到 {src_dir}/\n"
        f"  3. 重跑: python data_kaggle.py --src-dir {src_dir} --out <out.npz>\n"
        f"──────────────────────────────────────────────\n"
    )


def find_col(df_columns, candidates: list[str]) -> str | None:
    """大小写不敏感模糊匹配列名。"""
    lower_map = {c.lower(): c for c in df_columns}
    for cand in candidates:
        if cand.lower() in lower_map:
            return lower_map[cand.lower()]
    # 部分匹配
    for cand in candidates:
        for k_lower, k_orig in lower_map.items():
            if cand.lower() in k_lower:
                return k_orig
    return None


def detect_ppg_column(df) -> str | None:
    """选数值列中数据量最大、方差最大的作为 PPG。"""
    # 候选 = 数值列 - glucose/subject/scalar 列
    used = set()
    for cands in [
        GLUCOSE_COL_CANDIDATES,
        SUBJECT_COL_CANDIDATES,
        HR_COL_CANDIDATES,
        AGE_COL_CANDIDATES,
        GENDER_COL_CANDIDATES,
        SBP_CANDIDATES,
        DBP_CANDIDATES,
    ]:
        c = find_col(df.columns, cands)
        if c:
            used.add(c)

    candidates = []
    for c in df.columns:
        if c in used:
            continue
        try:
            vals = pd.to_numeric(df[c], errors="coerce").dropna()
            if len(vals) >= 50 and vals.var() > 1e-6:
                candidates.append((c, len(vals), float(vals.var())))
        except Exception:
            continue

    if not candidates:
        # 优先关键词匹配
        c = find_col(df.columns, PPG_COL_CANDIDATES)
        return c

    # 选样本量×方差最大的
    candidates.sort(key=lambda t: t[1] * t[2], reverse=True)
    return candidates[0][0]


def detect_fs(src_dir: Path, default: int = DEFAULT_FS) -> int:
    """优先级：env > metadata.txt > 文件名嗅探 > 默认。"""
    env = os.environ.get("KAGGLE_FS")
    if env and env.isdigit():
        return int(env)
    # metadata.txt
    for fname in ["metadata.txt", "README.txt", "info.txt"]:
        fp = src_dir / fname
        if fp.exists():
            text = fp.read_text(encoding="utf-8", errors="ignore").lower()
            m = re.search(r"(\d{2,4})\s*hz", text)
            if m:
                return int(m.group(1))
    # 文件名（如 ppg_1000hz.csv）
    for csv in src_dir.glob("*.csv"):
        m = re.search(r"(\d{2,4})hz", csv.name.lower())
        if m:
            return int(m.group(1))
    return default


# ── 信号处理（与 data_must.py 对齐） ─────────────────────────────────


def resample_to_100hz(x: np.ndarray, src_fs: int) -> np.ndarray:
    if src_fs == TARGET_FS:
        return x.astype(np.float32)
    from math import gcd
    g = gcd(src_fs, TARGET_FS)
    up = TARGET_FS // g
    down = src_fs // g
    return sp_signal.resample_poly(x, up, down).astype(np.float32)


def bandpass_ppg(x: np.ndarray) -> np.ndarray:
    nyq = TARGET_FS / 2.0
    sos = sp_signal.butter(4, [0.5 / nyq, 8.0 / nyq], btype="band", output="sos")
    return sp_signal.sosfiltfilt(sos, x).astype(np.float32)


def estimate_hr(x: np.ndarray) -> float:
    s = (x - x.mean()) / (x.std() + 1e-6)
    peaks, _ = sp_signal.find_peaks(s, distance=int(0.5 * TARGET_FS), prominence=0.3)
    if len(peaks) < 2:
        return SCAL_DEFAULTS["HR"]
    intervals = np.diff(peaks) / TARGET_FS
    hr = 60.0 / intervals.mean()
    return float(np.clip(hr, 50.0, 120.0))


def downsample_128(x_500: np.ndarray) -> np.ndarray:
    return sp_signal.resample(x_500, WINDOW_DS).astype(np.float32)


def normalize_unit(x: np.ndarray) -> np.ndarray:
    lo, hi = x.min(), x.max()
    if hi - lo < 1e-6:
        return np.zeros_like(x, dtype=np.float32)
    return ((x - lo) / (hi - lo)).astype(np.float32)


# ── 主处理 ────────────────────────────────────────────────────────────


def _glucose_to_mmol(val: float) -> float:
    """启发式单位识别 + 钳位。"""
    if val > 20:                        # mg/dL
        val = val / 18.018
    return float(np.clip(val, 2.0, 20.0))


def _bmr_from_age_gender(age: float, gender: str) -> float:
    """Mifflin-St Jeor 简化（标准体重 70kg/1.70m 假设）→ kcal/h。"""
    if gender and gender.lower().startswith("f"):
        kcal_per_day = 10 * 70 + 6.25 * 170 - 5 * age - 161
    else:
        kcal_per_day = 10 * 70 + 6.25 * 170 - 5 * age + 5
    return float(np.clip(kcal_per_day / 24.0, 30.0, 120.0))


def process_csv_long(df, src_fs: int, source_name: str) -> dict:
    """处理"长表"格式：每行一个时间步，多个 subject 用 subject_id 列区分。"""
    ppg_col = detect_ppg_column(df)
    glu_col = find_col(df.columns, GLUCOSE_COL_CANDIDATES)
    sid_col = find_col(df.columns, SUBJECT_COL_CANDIDATES)
    hr_col = find_col(df.columns, HR_COL_CANDIDATES)
    age_col = find_col(df.columns, AGE_COL_CANDIDATES)
    gender_col = find_col(df.columns, GENDER_COL_CANDIDATES)

    if ppg_col is None or glu_col is None:
        raise ValueError(
            f"无法识别 PPG / glucose 列（ppg={ppg_col} glucose={glu_col}）；"
            f"现有列: {list(df.columns)}"
        )

    print(f"  [cols] ppg={ppg_col}  glucose={glu_col}  subject={sid_col}  hr={hr_col}")

    # 按 subject 分组（无 subject 列则全表当一个 subject）
    if sid_col:
        groups = df.groupby(sid_col)
    else:
        groups = [(f"{source_name}_all", df)]

    X_ppg_list, X_scal_list, y_list, sid_list, mask_list = [], [], [], [], []

    for sid, grp in groups:
        ppg_raw = pd.to_numeric(grp[ppg_col], errors="coerce").dropna().to_numpy(np.float32)
        if len(ppg_raw) < src_fs * 6:        # 不够 6s
            continue
        # glucose 取该 subject 第一个非空值（一般全表同值）
        glu_series = pd.to_numeric(grp[glu_col], errors="coerce").dropna()
        if glu_series.empty:
            continue
        glucose_mmol = _glucose_to_mmol(glu_series.iloc[0])

        # 标量字段
        scal = np.array(
            [
                SCAL_DEFAULTS["HR"],
                SCAL_DEFAULTS["SpO2"],
                SCAL_DEFAULTS["BV"],
                SCAL_DEFAULTS["t_ir"],
                SCAL_DEFAULTS["t_env"],
                SCAL_DEFAULTS["h_env"],
                SCAL_DEFAULTS["BMR"],
            ],
            dtype=np.float32,
        )
        mask = np.zeros(7, dtype=np.uint8)

        if hr_col:
            hr_vals = pd.to_numeric(grp[hr_col], errors="coerce").dropna()
            if not hr_vals.empty:
                scal[0] = float(np.clip(hr_vals.mean(), 50, 120))
                mask[0] = 1

        if age_col and gender_col:
            age_vals = pd.to_numeric(grp[age_col], errors="coerce").dropna()
            gend_vals = grp[gender_col].dropna().astype(str)
            if not age_vals.empty and not gend_vals.empty:
                scal[6] = _bmr_from_age_gender(age_vals.iloc[0], gend_vals.iloc[0])
                mask[6] = 1

        # 100Hz 重采样 + 带通
        x100 = resample_to_100hz(ppg_raw, src_fs)
        x100 = bandpass_ppg(x100)

        # 切窗
        stride = int(WINDOW_STRIDE_SEC * TARGET_FS)
        n_windows = (len(x100) - WINDOW_RAW) // stride + 1
        for w in range(n_windows):
            seg500 = x100[w * stride : w * stride + WINDOW_RAW]
            if len(seg500) != WINDOW_RAW:
                continue
            seg128 = downsample_128(seg500)
            seg128 = normalize_unit(seg128)
            ppg_2ch = np.stack([seg128, seg128], axis=-1)

            # HR 现算（覆盖 hr_col 提供的均值，更精确）
            local_hr = estimate_hr(seg500)
            scal_w = scal.copy()
            scal_w[0] = local_hr
            mask_w = mask.copy()
            mask_w[0] = 1                    # HR 现算即视为有效

            X_ppg_list.append(ppg_2ch)
            X_scal_list.append(scal_w)
            y_list.append(glucose_mmol)
            sid_list.append(f"{source_name}:{sid}")
            mask_list.append(mask_w)

    if not X_ppg_list:
        raise RuntimeError(f"{source_name}: 处理后样本为空")

    return {
        "X_ppg": np.stack(X_ppg_list),
        "X_scal": np.stack(X_scal_list),
        "y": np.array(y_list, dtype=np.float32),
        "subject_id": np.array(sid_list, dtype=object),
        "scalar_mask": np.stack(mask_list),
    }


def process_csv_wide(df, src_fs: int, source_name: str) -> dict:
    """处理"宽表"格式：每行一个完整样本（PPG 是 ?ms... 列展开）。"""
    glu_col = find_col(df.columns, GLUCOSE_COL_CANDIDATES)
    if glu_col is None:
        raise ValueError(f"宽表未识别 glucose 列；现有列: {list(df.columns)[:10]}...")
    sid_col = find_col(df.columns, SUBJECT_COL_CANDIDATES)
    hr_col = find_col(df.columns, HR_COL_CANDIDATES)
    age_col = find_col(df.columns, AGE_COL_CANDIDATES)
    gender_col = find_col(df.columns, GENDER_COL_CANDIDATES)
    sbp_col = find_col(df.columns, SBP_CANDIDATES)
    dbp_col = find_col(df.columns, DBP_CANDIDATES)

    # PPG 列：纯数字列名或带时间前缀（如 t0, t1, ms_0, ms_10...）
    other_cols = {glu_col, sid_col, hr_col, age_col, gender_col, sbp_col, dbp_col}
    ppg_cols = [c for c in df.columns if c not in other_cols and c is not None]
    # 过滤掉非数值列
    ppg_cols = [c for c in ppg_cols if pd.to_numeric(df[c], errors="coerce").notna().sum() > 0]

    if len(ppg_cols) < 50:
        return None                          # 列数太少，不是宽表

    print(f"  [cols] glucose={glu_col} subject={sid_col} hr={hr_col} (PPG={len(ppg_cols)} 列)")
    print(f"  [fmt] 宽表（每行一个样本，PPG 横展开）")

    X_ppg_list, X_scal_list, y_list, sid_list, mask_list = [], [], [], [], []

    for i, row in df.iterrows():
        try:
            ppg_raw = np.array(
                [float(row[c]) for c in ppg_cols if pd.notna(row[c])],
                dtype=np.float32,
            )
        except Exception:
            continue
        if len(ppg_raw) < src_fs * 5:
            continue

        try:
            glucose_mmol = _glucose_to_mmol(float(row[glu_col]))
        except Exception:
            continue

        scal = np.array(
            [
                SCAL_DEFAULTS["HR"],
                SCAL_DEFAULTS["SpO2"],
                SCAL_DEFAULTS["BV"],
                SCAL_DEFAULTS["t_ir"],
                SCAL_DEFAULTS["t_env"],
                SCAL_DEFAULTS["h_env"],
                SCAL_DEFAULTS["BMR"],
            ],
            dtype=np.float32,
        )
        mask = np.zeros(7, dtype=np.uint8)
        if hr_col and pd.notna(row[hr_col]):
            scal[0] = float(np.clip(row[hr_col], 50, 120)); mask[0] = 1
        if age_col and gender_col and pd.notna(row[age_col]) and pd.notna(row[gender_col]):
            scal[6] = _bmr_from_age_gender(row[age_col], str(row[gender_col])); mask[6] = 1

        x100 = resample_to_100hz(ppg_raw, src_fs)
        x100 = bandpass_ppg(x100)
        if len(x100) < WINDOW_RAW:
            continue

        stride = int(WINDOW_STRIDE_SEC * TARGET_FS)
        n_windows = (len(x100) - WINDOW_RAW) // stride + 1
        sid_val = str(row[sid_col]) if sid_col and pd.notna(row[sid_col]) else f"row{i}"

        for w in range(n_windows):
            seg500 = x100[w * stride : w * stride + WINDOW_RAW]
            if len(seg500) != WINDOW_RAW:
                continue
            seg128 = downsample_128(seg500)
            seg128 = normalize_unit(seg128)
            X_ppg_list.append(np.stack([seg128, seg128], axis=-1))
            local_hr = estimate_hr(seg500)
            scal_w = scal.copy(); scal_w[0] = local_hr
            mask_w = mask.copy(); mask_w[0] = 1
            X_scal_list.append(scal_w)
            y_list.append(glucose_mmol)
            sid_list.append(f"{source_name}:{sid_val}")
            mask_list.append(mask_w)

    if not X_ppg_list:
        return None

    return {
        "X_ppg": np.stack(X_ppg_list),
        "X_scal": np.stack(X_scal_list),
        "y": np.array(y_list, dtype=np.float32),
        "subject_id": np.array(sid_list, dtype=object),
        "scalar_mask": np.stack(mask_list),
    }


def process(src_dir: Path, source_name: str) -> dict:
    if pd is None or sp_signal is None:
        print("[err] 缺少依赖 pandas / scipy；pip install -r requirements.txt")
        sys.exit(2)

    csvs = list(src_dir.glob("*.csv"))
    if not csvs:
        hint_download(source_name, src_dir)
        sys.exit(1)

    print(f"[scan] {len(csvs)} CSV in {src_dir}")
    src_fs = detect_fs(src_dir)
    print(f"[fs] 采样率检测 = {src_fs} Hz")

    # 合并所有 csv
    dfs = []
    for csv in csvs:
        try:
            df = pd.read_csv(csv)
            dfs.append(df)
        except Exception as e:
            print(f"  [skip] {csv.name}: {e}")
    if not dfs:
        print("[err] 所有 CSV 读取失败")
        sys.exit(1)

    # 同 schema 合并；否则逐个跑
    schemas = set(tuple(d.columns) for d in dfs)
    results = []
    if len(schemas) == 1:
        df = pd.concat(dfs, ignore_index=True)
        print(f"[merge] {len(dfs)} CSV 同 schema → {len(df)} 行")
        # 启发式：列数 > 100 视作宽表
        if len(df.columns) > 100:
            r = process_csv_wide(df, src_fs, source_name)
            if r:
                results.append(r)
            else:
                results.append(process_csv_long(df, src_fs, source_name))
        else:
            results.append(process_csv_long(df, src_fs, source_name))
    else:
        for i, df in enumerate(dfs):
            try:
                if len(df.columns) > 100:
                    r = process_csv_wide(df, src_fs, source_name)
                    if r is None:
                        r = process_csv_long(df, src_fs, source_name)
                else:
                    r = process_csv_long(df, src_fs, source_name)
                results.append(r)
            except Exception as e:
                print(f"  [skip] csv {i}: {e}")

    if not results:
        print("[err] 全部 CSV 处理失败")
        sys.exit(1)

    # 合并所有 csv 的结果
    merged = {
        "X_ppg":      np.concatenate([r["X_ppg"]      for r in results]),
        "X_scal":     np.concatenate([r["X_scal"]     for r in results]),
        "y":          np.concatenate([r["y"]          for r in results]),
        "subject_id": np.concatenate([r["subject_id"] for r in results]),
        "scalar_mask":np.concatenate([r["scalar_mask"]for r in results]),
    }

    n = len(merged["y"])
    n_sub = len(np.unique(merged["subject_id"]))
    print(
        f"[done] N={n} samples, {n_sub} subjects, "
        f"glucose [{merged['y'].min():.2f}, {merged['y'].max():.2f}] mmol/L"
    )
    return merged


def save_npz(data: dict, out: Path, source: str) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(
        out,
        X_ppg=data["X_ppg"],
        X_scal=data["X_scal"],
        y=data["y"],
        subject_id=data["subject_id"],
        scalar_mask=data["scalar_mask"],
        source=np.array([source] * len(data["y"]), dtype=object),
        meta=np.array([
            f"source={source}",
            f"target_fs={TARGET_FS}Hz",
            f"window={WINDOW_SEC}s",
            f"input_shape={WINDOW_DS}x2",
            "ir_channel=copy_of_red",
        ]),
    )
    print(f"[save] → {out} ({out.stat().st_size/1024:.1f} KB)")


# ── CLI / 自检 ────────────────────────────────────────────────────────


def _selftest():
    """无依赖的最小自检——用 fake CSV 验证 loader 框架。"""
    print("[selftest] data_kaggle.py 自检")
    if pd is None or sp_signal is None:
        print("  [skip] pandas/scipy 未装；脚本仍可被 import")
        return 0
    import tempfile
    with tempfile.TemporaryDirectory() as tmpd:
        tmp = Path(tmpd)
        # 构造 fake 长表 CSV
        n = 1200
        sig = np.sin(np.linspace(0, 10 * np.pi, n)) * 0.5 + 0.5
        df = pd.DataFrame({"ppg": sig, "glucose": [6.5] * n, "subject_id": ["S1"] * n})
        df.to_csv(tmp / "fake.csv", index=False)
        os.environ["KAGGLE_FS"] = "100"
        try:
            data = process(tmp, "fake")
            assert data["X_ppg"].shape[1:] == (128, 2), f"shape {data['X_ppg'].shape}"
            assert data["X_scal"].shape[1] == 7
            assert len(data["y"]) > 0
            print(f"  [ok] fake CSV 处理通过，N={len(data['y'])}")
            return 0
        except Exception as e:
            print(f"  [FAIL] {e}")
            return 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--src-dir", type=Path, help="包含 CSV 的目录")
    parser.add_argument("--out", type=Path, help="输出 .npz 路径")
    parser.add_argument("--name", type=str, default=None, help="数据源名（默认从 src-dir 推断）")
    parser.add_argument("--selftest", action="store_true", help="跑自检")
    args = parser.parse_args()

    if args.selftest:
        sys.exit(_selftest())

    if not args.src_dir or not args.out:
        parser.print_help()
        print("\n[err] 必须指定 --src-dir 和 --out")
        sys.exit(1)

    name = args.name or args.src_dir.name
    if not args.src_dir.exists():
        hint_download(name, args.src_dir)
        sys.exit(1)

    data = process(args.src_dir, name)
    save_npz(data, args.out, name)


if __name__ == "__main__":
    main()
