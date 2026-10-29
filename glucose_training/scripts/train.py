#!/usr/bin/env python3
"""
train.py — A++ 多源 PPG-glucose 训练

升级（spec.md 维度 4）：
  1. 数据源：默认 data/processed/multi.npz（fallback must.npz）
  2. KFold 改 GroupKFold(subject_id)：防 leakage
  3. 损失：Huber(δ=1.0) 替代 MSE
  4. LR：warmup(10 epoch from 1e-4 → 1e-3) + cosine decay 到 1e-5
  5. 数据增强：--aug 0..4 控制级别（参见 augment.py）
  6. ensemble：--seeds N 跑 N 个种子，每折 N 模型 → 5×N 集成
  7. 早停：patience=30 on val_mae
  8. 输出：models/checkpoints/best_fold{0..4}_seed{0..N-1}.h5
           outputs/train_metrics.json (fold×seed 矩阵 + ensemble 指标)
           outputs/ensemble_pred.npy + outputs/ensemble_true.npy（喂 eval_clinical）

用法:
  # A++ 完整训练
  python train.py --data data/processed/multi.npz --folds 5 --seeds 3 --aug 4 --epochs 200

  # 快速冒烟（少 epoch）
  python train.py --epochs 30 --folds 3 --seeds 1 --aug 0

  # 单 split（67 样本 baseline 调试）
  python train.py --no-cv --epochs 50
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

# tf 与 model 推迟到 main()，让 --help 在无 tf 环境也可用
tf = None
HAS_TF = False
build_glucose_model = None
normalize_scalars = None


def _ensure_tf():
    """延迟加载 tensorflow + model.py。"""
    global tf, HAS_TF, build_glucose_model, normalize_scalars
    if HAS_TF:
        return
    try:
        import tensorflow as _tf
        tf = _tf
        HAS_TF = True
        from model import (
            build_glucose_model as _bm,
            normalize_scalars as _ns,
        )
        build_glucose_model = _bm
        normalize_scalars = _ns
    except ImportError as e:
        print(f"[err] 依赖未装: {e}\n  pip install -r requirements.txt")
        sys.exit(2)

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
PROC_DIR = ROOT / "data" / "processed"
MULTI_NPZ = PROC_DIR / "multi.npz"
MUST_NPZ = PROC_DIR / "must.npz"
OUT_DIR = ROOT / "outputs"
CKPT_DIR = ROOT / "models" / "checkpoints"


# ── 数据加载（兼容多源 + 单源） ──────────────────────────────────────
def load_data(data_path: Path | None = None) -> dict:
    if data_path is None:
        if MULTI_NPZ.exists():
            data_path = MULTI_NPZ
        elif MUST_NPZ.exists():
            data_path = MUST_NPZ
            print(f"[fallback] 用 must.npz；建议先跑 data_merge.py 合并多源")
        else:
            raise FileNotFoundError(
                f"未找到 {MULTI_NPZ} 或 {MUST_NPZ}\n"
                f"请先跑: python data_must.py --extract --process [+ data_kaggle.py + data_merge.py]"
            )

    d = np.load(data_path, allow_pickle=True)
    out = {
        "X_ppg": d["X_ppg"].astype(np.float32),
        "X_scal": normalize_scalars(d["X_scal"]),
        "y": d["y"].astype(np.float32),
    }

    # subject_id（多源 npz 必有；旧 must.npz 自动补 fake）
    if "subject_id" in d.files:
        out["subject_id"] = np.asarray(d["subject_id"]).astype(str)
    else:
        # 旧 must.npz 没有 subject_id → 每 3 样本一组当一个虚拟 subject
        n = len(out["y"])
        out["subject_id"] = np.array([f"must:auto_{i//3}" for i in range(n)])

    print(f"[load] {data_path.name}")
    print(f"       X_ppg={out['X_ppg'].shape}  X_scal={out['X_scal'].shape}  y={out['y'].shape}")
    print(f"       glucose [{out['y'].min():.2f}, {out['y'].max():.2f}] "
          f"mean={out['y'].mean():.2f}±{out['y'].std():.2f}")
    print(f"       unique subjects: {len(np.unique(out['subject_id']))}")
    return out


# ── 学习率调度 ────────────────────────────────────────────────────────
def make_lr_schedule(steps_per_epoch: int, epochs: int,
                     lr_init: float = 1e-4, lr_peak: float = 1e-3,
                     lr_min: float = 1e-5, warmup_epochs: int = 10):
    """warmup → cosine decay。"""
    warmup_steps = warmup_epochs * steps_per_epoch
    total_steps = epochs * steps_per_epoch

    class WarmupCosine(tf.keras.optimizers.schedules.LearningRateSchedule):
        def __call__(self, step):
            step = tf.cast(step, tf.float32)
            warmup_lr = lr_init + (lr_peak - lr_init) * step / max(warmup_steps, 1)
            progress = (step - warmup_steps) / max(total_steps - warmup_steps, 1)
            progress = tf.clip_by_value(progress, 0.0, 1.0)
            cosine_lr = lr_min + 0.5 * (lr_peak - lr_min) * (
                1 + tf.cos(np.pi * progress)
            )
            return tf.where(step < warmup_steps, warmup_lr, cosine_lr)

        def get_config(self):
            return {
                "lr_init": lr_init, "lr_peak": lr_peak, "lr_min": lr_min,
                "warmup_steps": warmup_steps, "total_steps": total_steps,
            }

    return WarmupCosine()


# ── 数据增强生成器 ───────────────────────────────────────────────────
def make_augmented_generator(X_ppg, X_scal, y, batch: int, aug_level: int, seed: int):
    """无限循环 batch 生成器，每个 batch 应用 augment.py 级别化增强。"""
    from augment import get_augment_fn_by_level

    aug_fn = get_augment_fn_by_level(aug_level, seed=seed)
    rng = np.random.default_rng(seed)
    n = len(y)

    def gen():
        while True:
            idx = rng.permutation(n)
            for i in range(0, n, batch):
                bi = idx[i : i + batch]
                p, s, yy = X_ppg[bi], X_scal[bi], y[bi]
                if aug_level > 0:
                    p, s, yy = aug_fn(p, s, yy)
                yield {"ppg_in": p, "scal_in": s}, yy

    return gen


# ── Clarke EGA + 评估 ────────────────────────────────────────────────
def clarke_zones(y_true_mmol: np.ndarray, y_pred_mmol: np.ndarray) -> dict:
    y_true = y_true_mmol * 18.018
    y_pred = y_pred_mmol * 18.018
    zones = {"A": 0, "B": 0, "C": 0, "D": 0, "E": 0}
    for ref, pred in zip(y_true, y_pred):
        if (ref <= 70 and pred <= 70) or abs(pred - ref) / max(ref, 1e-6) <= 0.20:
            zones["A"] += 1
        elif (ref > 180 and pred < 70) or (ref < 70 and pred > 180):
            zones["E"] += 1
        elif (ref >= 240 and 70 < pred < 180) or (ref <= 70 and 70 < pred < 180):
            zones["D"] += 1
        elif (70 <= ref <= 180 and pred > 180) or (70 <= ref <= 180 and pred < 70):
            zones["C"] += 1
        else:
            zones["B"] += 1
    n = len(y_true)
    return {k: v / n * 100 for k, v in zones.items()}


def eval_metrics(y_true: np.ndarray, y_pred: np.ndarray) -> dict:
    err = y_pred - y_true
    mae = float(np.mean(np.abs(err)))
    rmse = float(np.sqrt(np.mean(err**2)))
    mape = float(np.mean(np.abs(err / (y_true + 1e-6))) * 100)
    zones = clarke_zones(y_true, y_pred)
    return {
        "MAE_mmol": mae,
        "RMSE_mmol": rmse,
        "MAPE_pct": mape,
        "CEG_A_pct": zones["A"],
        "CEG_AB_pct": zones["A"] + zones["B"],
        "CEG_DE_pct": zones["D"] + zones["E"],
    }


# ── 训练单 (fold, seed) ───────────────────────────────────────────────
def train_one(X_ppg_tr, X_scal_tr, y_tr,
              X_ppg_va, X_scal_va, y_va,
              *, epochs: int, batch: int, aug: int, seed: int,
              huber_delta: float, ckpt_path: Path):
    tf.random.set_seed(seed)
    np.random.seed(seed)

    model = build_glucose_model()
    steps_per_epoch = max(len(y_tr) // batch, 1)
    lr_schedule = make_lr_schedule(steps_per_epoch, epochs)

    model.compile(
        optimizer=tf.keras.optimizers.Adam(lr_schedule),
        loss=tf.keras.losses.Huber(delta=huber_delta),
        metrics=["mae"],
    )

    cbs = [
        tf.keras.callbacks.ModelCheckpoint(
            str(ckpt_path), monitor="val_mae", mode="min",
            save_best_only=True, save_weights_only=False, verbose=0,
        ),
        tf.keras.callbacks.EarlyStopping(
            monitor="val_mae", mode="min", patience=30,
            restore_best_weights=True, verbose=0,
        ),
    ]

    if aug > 0:
        gen = make_augmented_generator(X_ppg_tr, X_scal_tr, y_tr, batch, aug, seed)()
        model.fit(
            gen,
            steps_per_epoch=steps_per_epoch,
            validation_data=({"ppg_in": X_ppg_va, "scal_in": X_scal_va}, y_va),
            epochs=epochs, callbacks=cbs, verbose=0,
        )
    else:
        model.fit(
            x={"ppg_in": X_ppg_tr, "scal_in": X_scal_tr}, y=y_tr,
            validation_data=({"ppg_in": X_ppg_va, "scal_in": X_scal_va}, y_va),
            epochs=epochs, batch_size=batch, callbacks=cbs, verbose=0,
        )

    # 评估（用最佳 ckpt 已 restore）
    y_pred = model.predict(
        {"ppg_in": X_ppg_va, "scal_in": X_scal_va}, verbose=0
    ).squeeze()
    m = eval_metrics(y_va, y_pred)
    return m, y_pred


# ── 主流程 ────────────────────────────────────────────────────────────


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--data", type=Path, default=None,
                        help="训练数据 .npz（默认自动选 multi.npz 或 must.npz）")
    parser.add_argument("--folds", type=int, default=5, help="CV 折数")
    parser.add_argument("--seeds", type=int, default=3,
                        help="ensemble 种子数（每折跑 N 模型）")
    parser.add_argument("--aug", type=int, default=4, choices=[0, 1, 2, 3, 4],
                        help="数据增强级别（参见 augment.py）")
    parser.add_argument("--epochs", type=int, default=200)
    parser.add_argument("--batch", type=int, default=16)
    parser.add_argument("--huber-delta", type=float, default=1.0)
    parser.add_argument("--no-cv", action="store_true", help="单 split 而非 KFold")
    parser.add_argument("--seed-base", type=int, default=42)
    args = parser.parse_args()

    _ensure_tf()                              # 延迟 import

    CKPT_DIR.mkdir(parents=True, exist_ok=True)
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    data = load_data(args.data)
    X_ppg, X_scal, y, sid = data["X_ppg"], data["X_scal"], data["y"], data["subject_id"]
    n = len(y)

    # 显示训练配置
    print("\n=== 训练配置 ===")
    print(f"  folds={args.folds}  seeds={args.seeds}  aug={args.aug}")
    print(f"  epochs={args.epochs}  batch={args.batch}  huber_delta={args.huber_delta}")
    tmp = build_glucose_model()
    print(f"  模型参数: {tmp.count_params()}\n")

    if args.no_cv:
        from sklearn.model_selection import train_test_split
        idx_tr, idx_va = train_test_split(np.arange(n), test_size=0.2, random_state=args.seed_base)
        ckpt = CKPT_DIR / "best_single.h5"
        m, y_pred = train_one(
            X_ppg[idx_tr], X_scal[idx_tr], y[idx_tr],
            X_ppg[idx_va], X_scal[idx_va], y[idx_va],
            epochs=args.epochs, batch=args.batch, aug=args.aug, seed=args.seed_base,
            huber_delta=args.huber_delta, ckpt_path=ckpt,
        )
        print(f"[single] {m}")
        np.save(OUT_DIR / "ensemble_pred.npy", y_pred)
        np.save(OUT_DIR / "ensemble_true.npy", y[idx_va])
        results = {"single_split": m}
    else:
        from sklearn.model_selection import GroupKFold
        gkf = GroupKFold(n_splits=args.folds)

        fold_seed_metrics = []                # list[list[dict]] = fold×seed
        fold_seed_preds = [[None] * args.seeds for _ in range(args.folds)]
        fold_true = [None] * args.folds

        for fold_i, (idx_tr, idx_va) in enumerate(gkf.split(np.arange(n), groups=sid)):
            fold_metrics_per_seed = []
            fold_true[fold_i] = y[idx_va]
            print(f"\n--- Fold {fold_i}/{args.folds-1} ---")
            print(f"  train subjects: {len(np.unique(sid[idx_tr]))}, "
                  f"val subjects: {len(np.unique(sid[idx_va]))}")

            for seed_i in range(args.seeds):
                seed = args.seed_base + seed_i
                ckpt = CKPT_DIR / f"best_fold{fold_i}_seed{seed_i}.h5"
                m, y_pred = train_one(
                    X_ppg[idx_tr], X_scal[idx_tr], y[idx_tr],
                    X_ppg[idx_va], X_scal[idx_va], y[idx_va],
                    epochs=args.epochs, batch=args.batch, aug=args.aug,
                    seed=seed, huber_delta=args.huber_delta, ckpt_path=ckpt,
                )
                fold_metrics_per_seed.append(m)
                fold_seed_preds[fold_i][seed_i] = y_pred
                print(f"  [fold {fold_i} seed {seed_i}] MAE={m['MAE_mmol']:.3f} "
                      f"CEG_AB={m['CEG_AB_pct']:.1f}%")

            fold_seed_metrics.append(fold_metrics_per_seed)

        # 折内 ensemble（平均 N 个 seed 预测）
        fold_ens_metrics = []
        all_pred, all_true = [], []
        for fold_i in range(args.folds):
            preds = np.stack(fold_seed_preds[fold_i])  # (seeds, N_val)
            pred_ens = preds.mean(axis=0)
            m_ens = eval_metrics(fold_true[fold_i], pred_ens)
            fold_ens_metrics.append(m_ens)
            print(f"[fold {fold_i} ENSEMBLE] MAE={m_ens['MAE_mmol']:.3f}  "
                  f"CEG_AB={m_ens['CEG_AB_pct']:.1f}%")
            all_pred.append(pred_ens)
            all_true.append(fold_true[fold_i])

        # 全 CV ensemble
        all_pred = np.concatenate(all_pred)
        all_true = np.concatenate(all_true)
        global_ens = eval_metrics(all_true, all_pred)

        # 单 fold 平均（无 ensemble）
        avg_single = {
            k: float(np.mean([fs[k] for fold in fold_seed_metrics for fs in fold]))
            for k in fold_seed_metrics[0][0]
        }

        print("\n=== 交叉验证汇总 ===")
        print("  单模型平均（across fold×seed）:")
        for k, v in avg_single.items():
            print(f"    {k:<14}: {v:.3f}")
        print("  ENSEMBLE（per-fold seed-mean → global CV）:")
        for k, v in global_ens.items():
            print(f"    {k:<14}: {v:.3f}")
        print(f"  ensemble 增益 MAE: {avg_single['MAE_mmol']:.3f} → "
              f"{global_ens['MAE_mmol']:.3f}")

        # 保存全 CV ensemble 预测（给 eval_clinical.py）
        np.save(OUT_DIR / "ensemble_pred.npy", all_pred)
        np.save(OUT_DIR / "ensemble_true.npy", all_true)

        results = {
            "fold_seed_matrix": fold_seed_metrics,
            "fold_ensemble": fold_ens_metrics,
            "avg_single": avg_single,
            "ensemble": global_ens,
            "n_samples": int(n),
            "n_subjects": int(len(np.unique(sid))),
            "config": {
                "folds": args.folds, "seeds": args.seeds, "aug": args.aug,
                "epochs": args.epochs, "batch": args.batch,
                "huber_delta": args.huber_delta,
            },
        }

    out_json = OUT_DIR / "train_metrics.json"
    out_json.write_text(json.dumps(results, indent=2, ensure_ascii=False))
    print(f"\n[save] 指标 → {out_json}")
    print(f"[save] 模型 → {CKPT_DIR}/best_fold*_seed*.h5")
    print(f"[save] ensemble 预测 → {OUT_DIR}/ensemble_pred.npy + ensemble_true.npy")


if __name__ == "__main__":
    main()
