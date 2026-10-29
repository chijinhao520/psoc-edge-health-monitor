#!/usr/bin/env python3
"""
data_must.py — MUST/Mazandaran PPG-Glucose 数据集适配（v4 更新）

数据集：Mendeley 10.17632/37pm7jk7jn.4 (2025-09-10)
  - 23 受试者 / 67 个 PPG 样本
  - 2175 Hz 单通道 PPG (APDS-9008 光电二极管)
  - 血糖：Accu-Check 指尖（mg/dL）
  - 结构：
      RawData/signal_<sid>_<idx>.mat + .csv  （67 样本，sid=01..23）
      Labels/label_<sid>_<idx>.csv         （ID/Gender/Age/Glucose/Height/Weight）
      Labels/label_<sid>_<idx>.mat
      Figures/*.jpg                        （可视化，忽略）
  - 注：文件是 .rar 不是 .zip（v4 起改格式）。需要 UnRAR 或手动解压。

A++ 适配：
  - 输出含 subject_id (用于 GroupKFold)
  - 输出含 scalar_mask (1=真实值, 0=默认)
  - 用 Label 的 Gender/Age/Height/Weight 真实算 BMR（Mifflin-St Jeor）
  - HR 从 PPG 现算 → mask[0]=1
  - BMR 真实算 → mask[6]=1
  - 其他 5 维（SpO2/BV/t_ir/t_env/h_env）用 spec 默认值 → mask 对应位=0

输出（保存到 data/processed/must.npz）：
  X_ppg  (N, 128, 2) float32 [0,1]   — RED+IR 双通道，IR=RED 复制
  X_scal (N, 7) float32              — [HR, SpO2, BV, t_ir, t_env, h_env, BMR]
  y      (N,) float32                — 血糖 mmol/L
  subject_id (N,) str                — 'must:<sid>'
  source (N,) str                    — 'must'
  scalar_mask (N, 7) uint8           — 1=真实/0=默认

用法:
  # 手动解压后跑（推荐）
  python data_must.py --process

  # 老接口（zip + 内嵌解压）—— 现支持 rar
  python data_must.py --extract --process
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import zipfile
from pathlib import Path

import numpy as np
from scipy import signal as sp_signal
from scipy.io import loadmat

# ── 配置 ──────────────────────────────────────────────────────────────
HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
RAW_DIR = ROOT / "data" / "raw"
PROC_DIR = ROOT / "data" / "processed"
RAW_ARCHIVE_ZIP = RAW_DIR / "PPG_Dataset.zip"
RAW_ARCHIVE_RAR = RAW_DIR / "PPG_Dataset.rar"

TARGET_FS = 100
WINDOW_SEC = 5
WINDOW_RAW = TARGET_FS * WINDOW_SEC
WINDOW_DS = 128
SRC_FS = 2175
RESAMPLE_UP = 4
RESAMPLE_DOWN = 87
WINDOW_STRIDE_SEC = 2.5

# 7 维标量默认值（与 data_kaggle.py 对齐）
SCAL_DEFAULTS = {
    "HR": 75.0,
    "SpO2": 98.0,
    "BV": 1.0,
    "t_ir": 36.5,
    "t_env": 25.0,
    "h_env": 55.0,
    "BMR": 65.0,
}

# 文件名 pattern: signal_01_0001 / label_01_0001
FILENAME_RE = re.compile(r"^(?:signal|label)_(\d+)_(\d+)$")


# ── 下载 / 解压 ──────────────────────────────────────────────────────
def hint_download() -> None:
    print(
        "\n────── MUST PPG-Glucose 数据集下载指引 ──────\n"
        "v4 下载（已知 working，2025-09-10 发布）：\n"
        "  curl -L -o data/raw/PPG_Dataset.rar \\\n"
        "    https://data.mendeley.com/public-files/datasets/37pm7jk7jn/files/1bc4c1d9-09ff-4a01-8041-225e249c400f/file_downloaded\n"
        "或浏览器: https://data.mendeley.com/datasets/37pm7jk7jn/4 → Download All\n"
        "──────────────────────────────────────────────\n"
    )


def find_winrar() -> Path | None:
    for cand in [
        Path(r"C:\Program Files\WinRAR\UnRAR.exe"),
        Path(r"C:\Program Files\WinRAR\WinRAR.exe"),
        Path(r"C:\Program Files (x86)\WinRAR\UnRAR.exe"),
    ]:
        if cand.exists():
            return cand
    return None


def extract(force: bool = False) -> Path:
    """解压 PPG_Dataset.rar/zip → data/raw/PPG_Dataset/[PPG_Dataset/]{RawData,Labels,...}"""
    extract_to = RAW_DIR / "PPG_Dataset"
    if (extract_to / "RawData").exists() or (extract_to / "PPG_Dataset" / "RawData").exists():
        if not force:
            print(f"[skip] 已解压: {extract_to}")
            return extract_to

    extract_to.mkdir(parents=True, exist_ok=True)

    if RAW_ARCHIVE_RAR.exists():
        unrar = find_winrar()
        if unrar is None:
            print(
                "[err] 找到 PPG_Dataset.rar 但 WinRAR/UnRAR 不可用。\n"
                "选项：\n"
                "  - 安装 WinRAR 或 7-Zip\n"
                "  - 手动解压 .rar 到 data/raw/PPG_Dataset/"
            )
            sys.exit(1)
        print(f"[extract] {RAW_ARCHIVE_RAR} via {unrar.name}")
        result = subprocess.run(
            [str(unrar), "x", "-y", "-o+", str(RAW_ARCHIVE_RAR), str(extract_to) + os.sep],
            capture_output=True, text=True,
        )
        if result.returncode != 0:
            print(f"[err] UnRAR 失败:\n{result.stdout}\n{result.stderr}")
            sys.exit(result.returncode)
    elif RAW_ARCHIVE_ZIP.exists():
        print(f"[extract] {RAW_ARCHIVE_ZIP}")
        with zipfile.ZipFile(RAW_ARCHIVE_ZIP, "r") as zf:
            zf.extractall(extract_to)
    else:
        hint_download()
        sys.exit(1)

    return extract_to


def find_dataset_root(extract_to: Path) -> tuple[Path, Path]:
    """找到 RawData / Labels 子目录（v4 zip 内嵌一层 PPG_Dataset/）。"""
    for root in [extract_to, extract_to / "PPG_Dataset"]:
        raw = root / "RawData"
        lab = root / "Labels"
        if raw.exists() and lab.exists():
            return raw, lab
    # 暴力递归
    raw_cands = list(extract_to.rglob("RawData"))
    lab_cands = list(extract_to.rglob("Labels"))
    if raw_cands and lab_cands:
        return raw_cands[0], lab_cands[0]
    raise RuntimeError(f"在 {extract_to} 中找不到 RawData/Labels")


# ── 信号处理 ──────────────────────────────────────────────────────────
def resample_to_100hz(x: np.ndarray) -> np.ndarray:
    return sp_signal.resample_poly(x, RESAMPLE_UP, RESAMPLE_DOWN).astype(np.float32)


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


def downsample_to_128(x: np.ndarray) -> np.ndarray:
    return sp_signal.resample(x, WINDOW_DS).astype(np.float32)


def normalize_unit(x: np.ndarray) -> np.ndarray:
    lo, hi = x.min(), x.max()
    if hi - lo < 1e-6:
        return np.zeros_like(x, dtype=np.float32)
    return ((x - lo) / (hi - lo)).astype(np.float32)


# ── 文件读取 ──────────────────────────────────────────────────────────
def load_signal_csv(path: Path) -> np.ndarray:
    """v4 signal CSV: 单列数值，每行一个采样点。"""
    return np.loadtxt(str(path), dtype=np.float32, ndmin=1)


def load_signal_mat(path: Path) -> np.ndarray:
    mat = loadmat(str(path))
    cands = [
        (k, v) for k, v in mat.items()
        if not k.startswith("__") and isinstance(v, np.ndarray) and v.size > 100
    ]
    if not cands:
        raise ValueError(f"{path.name}: 找不到 PPG 数组")
    key, arr = max(cands, key=lambda kv: kv[1].size)
    return np.asarray(arr).squeeze().astype(np.float32)


def load_signal(stem: str, raw_dir: Path) -> np.ndarray:
    """优先 csv（更简单），fallback mat。"""
    csv = raw_dir / f"{stem}.csv"
    mat = raw_dir / f"{stem}.mat"
    if csv.exists():
        return load_signal_csv(csv)
    if mat.exists():
        return load_signal_mat(mat)
    raise FileNotFoundError(f"{stem}.csv 和 .mat 都不存在")


def load_label_csv(path: Path) -> dict:
    """v4 label CSV: ID,Gender,Age,Glucose,Height,Weight。"""
    import csv as _csv
    with open(path, "r", encoding="utf-8") as f:
        reader = _csv.DictReader(f)
        for row in reader:
            return row             # 只取第一行（label 文件就 1 个 sample）
    return {}


def label_to_features(label: dict) -> tuple[float, dict, np.ndarray]:
    """从 label 字典提取 (glucose_mmol, scalar_dict, mask)。"""
    # Glucose
    g_raw = float(label.get("Glucose", "0") or "0")
    g_mmol = g_raw / 18.018 if g_raw > 20 else g_raw      # 启发式 mg/dL → mmol/L
    g_mmol = float(np.clip(g_mmol, 2.0, 20.0))

    # Age/Gender/Height/Weight → BMR (Mifflin-St Jeor)
    scal = dict(SCAL_DEFAULTS)
    mask = np.zeros(7, dtype=np.uint8)

    try:
        age = float(label.get("Age", "0"))
        gender = label.get("Gender", "M").strip()
        height = float(label.get("Height", "170"))     # cm
        weight = float(label.get("Weight", "70"))      # kg
        # Mifflin-St Jeor (1990)
        if gender.lower().startswith("f"):
            kcal_per_day = 10 * weight + 6.25 * height - 5 * age - 161
        else:
            kcal_per_day = 10 * weight + 6.25 * height - 5 * age + 5
        bmr_kcal_h = float(np.clip(kcal_per_day / 24.0, 30.0, 120.0))
        scal["BMR"] = bmr_kcal_h
        mask[6] = 1
    except (ValueError, KeyError):
        pass

    return g_mmol, scal, mask


# ── 主流程 ────────────────────────────────────────────────────────────
def process(raw_dir: Path, lab_dir: Path) -> dict:
    print(f"[scan] RawData: {raw_dir}")
    print(f"[scan] Labels:  {lab_dir}")

    # 列所有 signal_* 文件 stem
    signal_files = set()
    for ext in ("csv", "mat"):
        for p in raw_dir.glob(f"signal_*.{ext}"):
            signal_files.add(p.stem)
    print(f"[scan] {len(signal_files)} signal stems")

    X_ppg_list, X_scal_list, y_list, sid_list, mask_list = [], [], [], [], []
    skipped = 0

    for stem in sorted(signal_files):
        m = FILENAME_RE.match(stem)
        if not m:
            skipped += 1
            continue
        sid = int(m.group(1))
        idx = int(m.group(2))

        # 配 label
        lab_stem = f"label_{m.group(1)}_{m.group(2)}"
        lab_csv = lab_dir / f"{lab_stem}.csv"
        if not lab_csv.exists():
            skipped += 1
            continue

        try:
            raw = load_signal(stem, raw_dir)
        except Exception as e:
            print(f"  [skip] {stem}: {e}")
            skipped += 1
            continue
        if raw.size < SRC_FS * 6:
            skipped += 1
            continue

        # Label
        label = load_label_csv(lab_csv)
        g_mmol, scal_dict, mask_template = label_to_features(label)

        # 100 Hz 重采样 + 带通
        x100 = resample_to_100hz(raw)
        x100 = bandpass_ppg(x100)

        # 滑窗
        stride = int(WINDOW_STRIDE_SEC * TARGET_FS)
        n_windows = (len(x100) - WINDOW_RAW) // stride + 1
        if n_windows < 1:
            skipped += 1
            continue

        for w in range(n_windows):
            seg500 = x100[w * stride : w * stride + WINDOW_RAW]
            if len(seg500) != WINDOW_RAW:
                continue
            seg128 = downsample_to_128(seg500)
            seg128 = normalize_unit(seg128)
            ppg_2ch = np.stack([seg128, seg128], axis=-1)

            local_hr = estimate_hr(seg500)
            scal = np.array([
                local_hr,
                scal_dict["SpO2"],
                scal_dict["BV"],
                scal_dict["t_ir"],
                scal_dict["t_env"],
                scal_dict["h_env"],
                scal_dict["BMR"],
            ], dtype=np.float32)
            mask = mask_template.copy()
            mask[0] = 1                    # HR 现算

            X_ppg_list.append(ppg_2ch)
            X_scal_list.append(scal)
            y_list.append(g_mmol)
            sid_list.append(f"must:{sid:02d}")
            mask_list.append(mask)

    if not X_ppg_list:
        raise RuntimeError("处理后样本为空；检查解压后 RawData/Labels 路径")

    out = {
        "X_ppg":       np.stack(X_ppg_list),
        "X_scal":      np.stack(X_scal_list),
        "y":           np.array(y_list, dtype=np.float32),
        "subject_id":  np.array(sid_list, dtype=object),
        "source":      np.array(["must"] * len(y_list), dtype=object),
        "scalar_mask": np.stack(mask_list),
    }
    n = len(out["y"])
    n_sub = len(np.unique(out["subject_id"]))
    print(
        f"[done] N={n} samples  subjects={n_sub}  "
        f"glucose [{out['y'].min():.2f}, {out['y'].max():.2f}] mmol/L  "
        f"(skipped {skipped} files)"
    )
    return out


def save(data: dict, out_path: Path) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(
        out_path,
        X_ppg=data["X_ppg"],
        X_scal=data["X_scal"],
        y=data["y"],
        subject_id=data["subject_id"],
        source=data["source"],
        scalar_mask=data["scalar_mask"],
        meta=np.array([
            "source=MUST/Mendeley 37pm7jk7jn v4",
            f"target_fs={TARGET_FS}Hz",
            f"window={WINDOW_SEC}s",
            f"input_shape={WINDOW_DS}x2",
            "ir_channel=copy_of_red",
            "hr=from_peak_detection",
            "bmr=Mifflin-St_Jeor from label CSV",
            "spo2/bv/t_ir/t_env/h_env=default_constants",
        ]),
    )
    print(f"[save] → {out_path} ({out_path.stat().st_size/1024:.1f} KB)")


# ── CLI ───────────────────────────────────────────────────────────────
def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--extract", action="store_true", help="解压 PPG_Dataset.rar/.zip")
    p.add_argument("--process", action="store_true", help="处理为 .npz")
    p.add_argument("--force", action="store_true", help="强制重解压")
    p.add_argument("--out", type=Path, default=PROC_DIR / "must.npz")
    args = p.parse_args()

    if not args.extract and not args.process:
        p.print_help()
        hint_download()
        return

    extract_to = RAW_DIR / "PPG_Dataset"
    if args.extract:
        extract_to = extract(force=args.force)
    elif not (extract_to / "RawData").exists() and not (extract_to / "PPG_Dataset" / "RawData").exists():
        print(f"[err] 未解压；先跑 --extract")
        sys.exit(1)

    if args.process:
        raw_dir, lab_dir = find_dataset_root(extract_to)
        data = process(raw_dir, lab_dir)
        save(data, args.out)


if __name__ == "__main__":
    main()
