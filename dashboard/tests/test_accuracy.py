"""Offline regressions; load pure definitions without opening .env/network/user DB."""
import ast
import math
from pathlib import Path
import sqlite3
import threading
import unittest
from datetime import datetime, timezone

SOURCE = Path(__file__).resolve().parents[1] / "server.py"
TREE = ast.parse(SOURCE.read_text(encoding="utf-8"))
CONSTANTS = {"FIELDS", "METRIC_GROUPS", "ARR_CLASS_DESC"}
FUNCTIONS = {"normalize_telemetry", "init_db", "_in_range", "_flag", "_gated",
             "_metric", "_round", "_build_snapshot"}
DEFS = [n for n in TREE.body if
        isinstance(n, ast.FunctionDef) and n.name in FUNCTIONS or
        isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id in CONSTANTS for t in n.targets)]
NS = {"math": math, "datetime": datetime, "timezone": timezone,
      "AI_TREND_HOURS": 24, "ONLINE_TIMEOUT_MS": 30000, "_db_lock": threading.Lock()}
exec(compile(ast.Module(body=DEFS, type_ignores=[]), str(SOURCE), "exec"), NS)
normalize = NS["normalize_telemetry"]


class AccuracyTests(unittest.TestCase):
    def setUp(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        NS["_conn"] = self.db
        NS["init_db"](self.db)

    def tearDown(self):
        self.db.close()

    def insert(self, **values):
        row = {"device_id": "test", "ts": int(datetime.now(timezone.utc).timestamp()*1000), **values}
        self.db.execute(f"INSERT INTO telemetry ({','.join(row)}) VALUES ({','.join('?' for _ in row)})", tuple(row.values()))
        self.db.commit()

    def test_legacy_migration_preserves_data(self):
        db = sqlite3.connect(":memory:")
        db.execute("CREATE TABLE telemetry(id INTEGER PRIMARY KEY, device_id TEXT, ts INTEGER, heart_rate REAL)")
        db.execute("INSERT INTO telemetry VALUES(1,'old',123,75)")
        NS["init_db"](db)
        NS["init_db"](db)  # Idempotent migration.
        self.assertEqual(db.execute("SELECT heart_rate FROM telemetry").fetchone()[0], 75)
        self.assertTrue(set(NS["FIELDS"]) <= {r[1] for r in db.execute("PRAGMA table_info(telemetry)")})
        db.close()

    def test_missing_or_false_flag_is_unmeasured(self):
        for flag in (None, 0, False, "1", 2):
            with self.subTest(flag=flag):
                result = normalize({"heart_rate": 75, "hr_valid": flag})
                self.assertEqual(result["hr_valid"], 0)
                self.assertIsNone(result["heart_rate"])

    def test_each_group_valid_range_and_invalid_values(self):
        for flag, metrics in NS["METRIC_GROUPS"].items():
            base = {flag: 1, **{key: (lo+hi)/2 for key, (lo, hi) in metrics.items()}}
            if flag == "arr_pvc_valid":
                base.update(arr_valid=1, arr_class=0, arr_beat_count=1)
            with self.subTest(flag=flag):
                self.assertEqual(normalize(base)[flag], 1)
                for key, (lo, hi) in metrics.items():
                    for bad in (None, "75", {}, [], True, math.nan, math.inf, -math.inf, 10**1000, lo-1, hi+1):
                        result = normalize({**base, key: bad})
                        self.assertEqual(result[flag], 0)
                        for member in metrics:
                            self.assertIsNone(result[member])

    def test_environment_failure_invalidates_entire_pair(self):
        r = normalize(dict(env_valid=1, env_temp=25, env_humidity=101))
        self.assertIsNone(r["env_temp"])
        self.assertIsNone(r["env_humidity"])

    def test_normalization_is_idempotent(self):
        r = normalize(dict(hr_valid=1, heart_rate=75, env_valid=1, env_temp=-0.5, env_humidity=50))
        self.assertEqual(normalize(r), r)

    def test_snapshot_and_trend_exclude_invalid_legacy_values(self):
        self.insert(ts=1000, heart_rate=200, spo2=50, health_score=999, health_score_valid=1,
                    hrv_rmssd_ms=999, hrv_valid=1, arr_valid=1, arr_pvc_per_min=999)
        self.insert(ts=2000, heart_rate=80, hr_valid=1, spo2=98, spo2_valid=1,
                    health_score=90, health_score_valid=1, hrv_rmssd_ms=40, hrv_valid=1)
        snap, ts, dev = NS["_build_snapshot"]()
        self.assertEqual((ts, dev), (2000, "test"))
        self.assertEqual(snap["trend"]["heart_rate"], {"avg": 80, "min": 80, "max": 80})
        self.assertEqual(snap["trend"]["spo2"]["avg"], 98)
        self.assertEqual(snap["trend"]["health_score"]["avg"], 90)
        self.assertEqual(snap["trend"]["hrv_rmssd"]["avg"], 40)
        self.assertIsNone(snap["trend"]["pvc_per_min_max"])
        self.assertFalse(snap["metrics"]["glucose"]["measured"])

    def test_rule_source_and_pvc_warmup(self):
        self.insert(arr_valid=1, arr_class=2, arr_source=1, arr_beat_count=20,
                    arr_pvc_per_min=0, arr_pvc_valid=0)
        snap, _, _ = NS["_build_snapshot"]()
        self.assertTrue(snap["arrhythmia"]["measured"])
        self.assertIn("规则", snap["arrhythmia"]["source"])
        self.assertIsNone(snap["arrhythmia"]["pvc_per_min"])

    def test_invalid_latest_is_not_invented_in_ai_input(self):
        self.insert(heart_rate=75, spo2=98, glucose=5, hr_valid=0, spo2_valid=0, glucose_valid=0)
        snap, _, _ = NS["_build_snapshot"]()
        for key in ("heart_rate", "spo2", "glucose"):
            self.assertFalse(snap["metrics"][key]["measured"])


if __name__ == "__main__":
    unittest.main()
