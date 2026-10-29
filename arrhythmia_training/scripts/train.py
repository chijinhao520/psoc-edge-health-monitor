#!/usr/bin/env python3
"""
train.py — 心律不齐 CNN baseline 训练 + DS2 评估

- 从 DS1 分层切 10% 做验证集（DS2 严格留作最终测试，不参与调参）
- 类别极不平衡 → class_weight='balanced'
- 评估：macro-F1 + 每类 P/R/F1 + 混淆矩阵（不看 accuracy）

用法：
    python scripts/train.py                  # 默认 40 epoch
    python scripts/train.py --epochs 60 --batch 128

输出：
    models/checkpoints/best.h5
    outputs/train_metrics.json
    outputs/confusion_matrix.png
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))

NPZ = ROOT / "data" / "processed" / "mitbih.npz"
CKPT = ROOT / "models" / "checkpoints" / "best.h5"
METRICS = ROOT / "outputs" / "train_metrics.json"
CM_PNG = ROOT / "outputs" / "confusion_matrix.png"


def _load():
    if not NPZ.exists():
        sys.exit(f"[err] 找不到 {NPZ}，先跑 python scripts/data_mitbih.py")
    d = np.load(NPZ, allow_pickle=True)
    return (d["X_train"], d["RR_train"], d["y_train"],
            d["X_test"], d["RR_test"], d["y_test"],
            [str(c) for c in d["classes"]])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--epochs", type=int, default=40)
    ap.add_argument("--batch", type=int, default=128)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--keep-q", action="store_true",
                    help="保留 Q 类(默认丢弃,起搏记录已排除 Q 退化为噪声)")
    ap.add_argument("--cw-power", type=float, default=0.5,
                    help="class_weight 温和化指数(balanced^p,p<1 抑制稀有类过预测)")
    args = ap.parse_args()

    import tensorflow as tf
    from sklearn.model_selection import train_test_split
    from sklearn.metrics import (classification_report, confusion_matrix,
                                 f1_score)
    from sklearn.utils.class_weight import compute_class_weight
    from model import build_arrhythmia_model

    tf.random.set_seed(42)
    np.random.seed(42)

    X_tr_all, R_tr_all, y_tr_all, X_te, R_te, y_te, classes = _load()
    print(f"train {X_tr_all.shape} RR{R_tr_all.shape}  test {X_te.shape}  classes {classes}")

    # 丢弃退化的 Q 类（起搏记录已排除 → Q 仅 ~8 样本，balanced 权重会爆到 ~1000）
    if not args.keep_q and "Q" in classes:
        q = classes.index("Q")
        keep = lambda X, R, y: (X[y != q], R[y != q], y[y != q])
        X_tr_all, R_tr_all, y_tr_all = keep(X_tr_all, R_tr_all, y_tr_all)
        X_te, R_te, y_te = keep(X_te, R_te, y_te)
        classes = [c for c in classes if c != "Q"]
        print(f"[drop-Q] → 4 类 {classes}  train={len(y_tr_all)} test={len(y_te)}")
    n_classes = len(classes)

    # DS1 内分层切验证集（DS2 不碰）
    X_tr, X_val, R_tr, R_val, y_tr, y_val = train_test_split(
        X_tr_all, R_tr_all, y_tr_all, test_size=0.10,
        stratify=y_tr_all, random_state=42)

    # class_weight balanced，再 ^cw_power 温和化（抑制稀有类过预测毁精度）
    cw = compute_class_weight("balanced", classes=np.arange(n_classes), y=y_tr)
    cw = cw ** args.cw_power
    class_weight = {i: float(w) for i, w in enumerate(cw)}
    print(f"class_weight (^{args.cw_power}) {class_weight}")

    model = build_arrhythmia_model(n_classes=n_classes)
    model.compile(
        optimizer=tf.keras.optimizers.Adam(args.lr),
        loss="sparse_categorical_crossentropy",
        metrics=["accuracy"],
    )
    model.summary(line_length=100)

    CKPT.parent.mkdir(parents=True, exist_ok=True)
    callbacks = [
        tf.keras.callbacks.ModelCheckpoint(
            str(CKPT), monitor="val_loss", save_best_only=True, verbose=1),
        tf.keras.callbacks.EarlyStopping(
            monitor="val_loss", patience=8, restore_best_weights=True),
        tf.keras.callbacks.ReduceLROnPlateau(
            monitor="val_loss", factor=0.5, patience=4, min_lr=1e-5),
    ]

    model.fit(
        {"ecg_in": X_tr, "rr_in": R_tr}, y_tr,
        validation_data=({"ecg_in": X_val, "rr_in": R_val}, y_val),
        epochs=args.epochs, batch_size=args.batch,
        class_weight=class_weight, callbacks=callbacks, verbose=2,
    )

    # ── DS2 最终评估 ──────────────────────────────────────────────────
    y_pred = model.predict(
        {"ecg_in": X_te, "rr_in": R_te}, batch_size=256, verbose=0).argmax(1)
    macro_f1 = f1_score(y_te, y_pred, average="macro")
    rep = classification_report(y_te, y_pred, target_names=classes,
                                digits=4, output_dict=True, zero_division=0)
    cm = confusion_matrix(y_te, y_pred, labels=np.arange(len(classes)))

    print("\n" + "=" * 60)
    print("DS2 (inter-patient) 最终评估")
    print("=" * 60)
    print(classification_report(y_te, y_pred, target_names=classes,
                                digits=4, zero_division=0))
    print(f"macro-F1 = {macro_f1:.4f}")
    print("混淆矩阵 (行=真值 列=预测):")
    print("     " + "  ".join(f"{c:>6}" for c in classes))
    for i, c in enumerate(classes):
        print(f"{c:>3}  " + "  ".join(f"{cm[i,j]:>6d}" for j in range(len(classes))))

    METRICS.parent.mkdir(parents=True, exist_ok=True)
    METRICS.write_text(json.dumps({
        "split": "inter-patient DS1/DS2 (de Chazal)",
        "macro_f1": float(macro_f1),
        "per_class": {c: rep[c] for c in classes},
        "confusion_matrix": cm.tolist(),
        "classes": classes,
        "n_train": int(len(y_tr)), "n_val": int(len(y_val)),
        "n_test": int(len(y_te)),
    }, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"\n[ok] 指标写出 {METRICS}")

    # 混淆矩阵图
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(figsize=(5, 4))
        im = ax.imshow(cm, cmap="Blues")
        ax.set_xticks(range(len(classes)), classes)
        ax.set_yticks(range(len(classes)), classes)
        ax.set_xlabel("predicted"); ax.set_ylabel("true")
        ax.set_title(f"MIT-BIH DS2  macro-F1={macro_f1:.3f}")
        for i in range(len(classes)):
            for j in range(len(classes)):
                ax.text(j, i, cm[i, j], ha="center", va="center",
                        color="white" if cm[i, j] > cm.max() / 2 else "black",
                        fontsize=8)
        fig.colorbar(im); fig.tight_layout()
        fig.savefig(CM_PNG, dpi=120)
        print(f"[ok] 混淆矩阵图 {CM_PNG}")
    except Exception as e:
        print(f"[warn] 画图跳过: {e}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
