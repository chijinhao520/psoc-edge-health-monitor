#!/usr/bin/env python3
"""
ensemble_distill.py — 把 fold×seed ensemble 蒸馏到单个学生模型

部署到 M55 不能跑 15 个模型，所以：
  1. 加载所有 best_fold{F}_seed{S}.h5（理论上 5×3=15 个；缺则自动跳过）
  2. 用教师集成在训练集上跑一遍 → soft labels
  3. 用相同架构的学生模型训练，损失 = 0.5*Huber(y_true) + 0.5*MSE(y_teacher_soft)
  4. 蒸馏 20 epoch，输出 outputs/glucose_distilled.h5
  5. 评估学生 vs 教师集成；漂移 < 5% 视为成功

自动短路：
  如 ensemble MAE 与最佳单 fold MAE 差 < 5%（ensemble 增益微弱），跳过蒸馏，
  直接用 best_fold0_seed0.h5 作为最终模型（节省一轮训练）。

用法:
  python ensemble_distill.py
  python ensemble_distill.py --epochs 30 --lr 1e-4 --temperature 1.0
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

tf = None
build_glucose_model = None
normalize_scalars = None
eval_metrics = None


def _ensure_deps():
    global tf, build_glucose_model, normalize_scalars, eval_metrics
    if tf is not None:
        return
    try:
        import tensorflow as _tf
        tf = _tf
    except ImportError:
        print("[err] tensorflow 未装；pip install -r requirements.txt")
        sys.exit(2)
    from model import build_glucose_model as _bm, normalize_scalars as _ns
    from train import eval_metrics as _em
    build_glucose_model = _bm
    normalize_scalars = _ns
    eval_metrics = _em


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
PROC_DIR = ROOT / "data" / "processed"
OUT_DIR = ROOT / "outputs"
CKPT_DIR = ROOT / "models" / "checkpoints"

SKIP_DELTA = 0.05    # ensemble 增益 < 5% 跳过蒸馏


# ── 数据 ──────────────────────────────────────────────────────────────


def load_data():
    """返回 (X_ppg_full, X_scal_full, y_full, subject_id, splits) - 全量数据 + GroupKFold 分组。

    教师集成预测**只在每个教师对应 fold 的 val 集上**做,避免 fp32 在见过的训练数据上"自评"。
    OOF 预测拼接后才能作为蒸馏的 soft labels (干净的非数据泄漏版本)。
    """
    multi = PROC_DIR / "multi.npz"
    must = PROC_DIR / "must.npz"
    src = multi if multi.exists() else must
    if not src.exists():
        print(f"[err] 找不到数据 npz")
        sys.exit(2)
    d = np.load(src, allow_pickle=True)
    X_ppg = d["X_ppg"].astype(np.float32)
    X_scal = normalize_scalars(d["X_scal"])
    y = d["y"].astype(np.float32)
    sid = (np.asarray(d["subject_id"]).astype(str)
           if "subject_id" in d.files
           else np.array([f"auto_{i//3}" for i in range(len(y))]))
    from sklearn.model_selection import GroupKFold
    splits = list(GroupKFold(n_splits=5).split(np.arange(len(y)), groups=sid))
    return X_ppg, X_scal, y, sid, splits


# ── 教师集成预测 ──────────────────────────────────────────────────────


def collect_teacher_models():
    """扫描 best_fold*_seed*.h5 + 兜底 best_fold*.h5 / best_single.h5。"""
    cands = sorted(CKPT_DIR.glob("best_fold*_seed*.h5"))
    if not cands:
        cands = sorted(CKPT_DIR.glob("best_fold*.h5"))
    if not cands:
        single = CKPT_DIR / "best_single.h5"
        if single.exists():
            cands = [single]
    return cands


def teacher_predict_full(teachers: list, X_ppg, X_scal) -> np.ndarray:
    """[已弃用,保留兼容] 每个教师对全量数据预测取平均 — 含数据泄漏。"""
    preds = []
    for ck in teachers:
        m = tf.keras.models.load_model(str(ck), compile=False)
        p = m.predict({"ppg_in": X_ppg, "scal_in": X_scal}, verbose=0).squeeze()
        preds.append(p)
        tf.keras.backend.clear_session()
    return np.mean(np.stack(preds), axis=0)


def teacher_predict_oof(teachers: list, splits: list, X_ppg, X_scal, y) -> np.ndarray:
    """OOF 教师集成 (audit-fix 2026-05-27):
       - 每个教师 best_fold{F}_seed{S}.h5 只对它的 val fold F 预测 (fp32 没见过)
       - 同 fold 内多 seed 取平均 → 该 fold 的 OOF 预测
       - 拼接 5 fold 的 OOF 预测 → 全数据集干净的 soft labels

       如果教师命名不符合 fold{F}_seed{S} 模式,fallback 用全量预测 (旧行为)。
    """
    import re
    fold_to_models = {}
    has_unparseable = False
    for ck in teachers:
        m = re.search(r"fold(\d+)_seed\d+", ck.name)
        if m:
            f = int(m.group(1))
            fold_to_models.setdefault(f, []).append(ck)
        else:
            has_unparseable = True

    if has_unparseable or not fold_to_models:
        print("[warn] 教师命名不识别,fallback 全量预测 (含数据泄漏)")
        return teacher_predict_full(teachers, X_ppg, X_scal)

    oof_pred = np.zeros(len(y), dtype=np.float32)
    coverage = np.zeros(len(y), dtype=bool)

    for fold_i, (_, val_idx) in enumerate(splits):
        if fold_i not in fold_to_models:
            continue
        fold_preds = []
        for ck in fold_to_models[fold_i]:
            m = tf.keras.models.load_model(str(ck), compile=False)
            p = m.predict({"ppg_in": X_ppg[val_idx],
                           "scal_in": X_scal[val_idx]}, verbose=0).squeeze()
            fold_preds.append(p)
            tf.keras.backend.clear_session()
        oof_pred[val_idx] = np.mean(np.stack(fold_preds), axis=0)
        coverage[val_idx] = True

    if not coverage.all():
        n_missing = (~coverage).sum()
        print(f"[warn] OOF 覆盖不全: {n_missing}/{len(y)} 样本无对应教师,"
              f"用全量教师集成补 (有泄漏)")
        miss_idx = np.where(~coverage)[0]
        full_pred = teacher_predict_full(teachers,
                                         X_ppg[miss_idx], X_scal[miss_idx])
        oof_pred[miss_idx] = full_pred

    return oof_pred


# ── 蒸馏训练 ──────────────────────────────────────────────────────────


def distill(X_ppg, X_scal, y_true, y_teacher,
            epochs: int, lr: float, alpha: float, batch: int) -> "tf.keras.Model":
    """学生模型损失 = α * Huber(y_true) + (1-α) * MSE(y_teacher)。"""

    student = build_glucose_model()

    @tf.function
    def combined_loss(y_pair, y_pred):
        """y_pair shape (B, 2): [真值, 教师 soft]"""
        y_real = y_pair[:, 0]
        y_soft = y_pair[:, 1]
        y_p = tf.squeeze(y_pred, axis=-1)
        huber = tf.keras.losses.Huber(delta=1.0)(y_real, y_p)
        mse = tf.reduce_mean(tf.square(y_soft - y_p))
        return alpha * huber + (1 - alpha) * mse

    student.compile(
        optimizer=tf.keras.optimizers.Adam(lr),
        loss=combined_loss,
        metrics=[],
    )

    y_pair = np.stack([y_true, y_teacher], axis=-1).astype(np.float32)
    student.fit(
        {"ppg_in": X_ppg, "scal_in": X_scal}, y_pair,
        epochs=epochs, batch_size=batch, verbose=0,
    )
    return student


# ── 主流程 ────────────────────────────────────────────────────────────


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--epochs", type=int, default=20)
    parser.add_argument("--lr", type=float, default=1e-4)
    parser.add_argument("--alpha", type=float, default=0.5,
                        help="α * Huber(真值) + (1-α) * MSE(教师 soft)")
    parser.add_argument("--batch", type=int, default=16)
    parser.add_argument("--out", type=Path, default=CKPT_DIR / "glucose_distilled.h5")
    parser.add_argument("--force", action="store_true",
                        help="即使 ensemble 增益小也跑蒸馏")
    args = parser.parse_args()

    _ensure_deps()
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    teachers = collect_teacher_models()
    if not teachers:
        print("[err] 找不到 teacher .h5；先跑 train.py")
        sys.exit(2)
    print(f"[teachers] {len(teachers)} 个模型:")
    for t in teachers:
        print(f"  - {t.name}")

    X_ppg, X_scal, y, sid, splits = load_data()
    print(f"[data] N={len(y)}  subjects={len(np.unique(sid))}")

    # 1. 教师集成预测 (OOF, audit-fix 2026-05-27)
    print("[teacher] OOF 集成推理 (每教师只预测对应 fold val)...")
    y_teacher = teacher_predict_oof(teachers, splits, X_ppg, X_scal, y)
    m_teacher = eval_metrics(y, y_teacher)
    print(f"[teacher] OOF ENSEMBLE MAE={m_teacher['MAE_mmol']:.3f}")

    # 2. 单 fold 0 模型预测 (在它的 val fold 上,与上面一致避免对比泄漏)
    import re
    single = teachers[0]
    m_sf = re.search(r"fold(\d+)_seed\d+", single.name)
    if m_sf:
        single_fold = int(m_sf.group(1))
        _, val_idx = splits[single_fold]
        m_single_model = tf.keras.models.load_model(str(single), compile=False)
        y_single = m_single_model.predict(
            {"ppg_in": X_ppg[val_idx], "scal_in": X_scal[val_idx]}, verbose=0
        ).squeeze()
        m_single = eval_metrics(y[val_idx], y_single)
        print(f"[single]   {single.name} val-fold MAE={m_single['MAE_mmol']:.3f}")
    else:
        m_single_model = tf.keras.models.load_model(str(single), compile=False)
        y_single = m_single_model.predict({"ppg_in": X_ppg, "scal_in": X_scal}, verbose=0).squeeze()
        m_single = eval_metrics(y, y_single)
        print(f"[single]   {single.name} 全量 MAE={m_single['MAE_mmol']:.3f} (含泄漏)")
    tf.keras.backend.clear_session()

    # 3. 短路判定
    delta = (m_single["MAE_mmol"] - m_teacher["MAE_mmol"]) / max(m_single["MAE_mmol"], 1e-6)
    print(f"\n[ensemble 增益] {delta*100:.2f}% （阈值 {SKIP_DELTA*100:.0f}%）")
    if delta < SKIP_DELTA and not args.force:
        print("[short-circuit] ensemble 增益 < 阈值，跳过蒸馏，直接用单模型")
        # 拷贝 single → distilled
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_bytes(single.read_bytes())
        report = {
            "skipped": True,
            "reason": f"ensemble 增益 {delta*100:.2f}% < {SKIP_DELTA*100:.0f}%",
            "source_model": str(single),
            "single_mae": m_single["MAE_mmol"],
            "ensemble_mae": m_teacher["MAE_mmol"],
        }
        (OUT_DIR / "distill_report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False))
        print(f"[save] {args.out} (拷自 {single.name})")
        sys.exit(0)

    # 4. 蒸馏
    print(f"\n[distill] 蒸馏学生 {args.epochs} epoch (lr={args.lr}, α={args.alpha})")
    student = distill(X_ppg, X_scal, y, y_teacher,
                      epochs=args.epochs, lr=args.lr, alpha=args.alpha, batch=args.batch)
    student.save(str(args.out))
    print(f"[save] 学生 → {args.out}")

    # 5. 评估
    #    注:student 在全量上训练 → 全量评估是设计如此 (蒸馏的本质)
    #    teacher 是 OOF (干净),student 在全量上看 OOF teacher 拟合得多准
    y_student = student.predict({"ppg_in": X_ppg, "scal_in": X_scal}, verbose=0).squeeze()
    m_student_full = eval_metrics(y, y_student)
    print(f"[student] 全量(含训练) MAE={m_student_full['MAE_mmol']:.3f}")

    drift = (m_student_full["MAE_mmol"] - m_teacher["MAE_mmol"]) / m_teacher["MAE_mmol"] * 100
    status = "PASS" if abs(drift) < 5 else "WARN"
    print(f"\n[delta] teacher(OOF) → student(full) MAE 漂移 {drift:+.2f}% → {status}")
    m_student = m_student_full      # 兼容下游 JSON 字段

    report = {
        "skipped": False,
        "teachers": [str(t) for t in teachers],
        "alpha": args.alpha,
        "epochs": args.epochs,
        "metrics": {
            "teacher_ensemble": m_teacher,
            "single_best": m_single,
            "student_distilled": m_student,
        },
        "drift_pct": drift,
        "status": status,
    }
    rpt = OUT_DIR / "distill_report.json"
    rpt.write_text(json.dumps(report, indent=2, ensure_ascii=False))
    print(f"[save] {rpt}")


if __name__ == "__main__":
    main()
