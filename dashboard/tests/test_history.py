import csv
import io
import time
import unittest
from offline_app import create_app


class HistoryTests(unittest.TestCase):
    def setUp(self):
        self.app, self.ns, self.db = create_app()
        self.client = self.app.test_client()
        self.now = int(time.time()*1000)

    def tearDown(self):
        self.db.close()

    def insert(self, device="A", ts=None, **fields):
        row = {"device_id": device, "ts": self.now-1000 if ts is None else ts, **fields}
        self.db.execute(f"INSERT INTO telemetry ({','.join(row)}) VALUES ({','.join('?' for _ in row)})", tuple(row.values()))
        self.db.commit()

    def test_routes_render_and_devices(self):
        self.assertEqual(self.client.get("/").status_code, 200)
        with self.client.get("/static/vendor/chart.umd.js") as response:
            self.assertEqual(response.status_code, 200)
        self.assertEqual(self.client.get("/api/devices").json, [])
        self.insert("A", self.now-60000)
        self.insert("B")
        devices = self.client.get("/api/devices").json
        self.assertEqual([d["device_id"] for d in devices], ["B", "A"])
        self.assertTrue(devices[0]["online"])
        self.assertFalse(devices[1]["online"])

    def test_default_and_explicit_device_never_mix(self):
        self.insert("A", self.now-5000, heart_rate=60, hr_valid=1)
        self.insert("B", heart_rate=90, hr_valid=1)
        self.assertEqual([r["device_id"] for r in self.client.get("/api/history").json], ["B"])
        rows = self.client.get("/api/history?device_id=A").json
        self.assertEqual([r["heart_rate"] for r in rows], [60])
        self.assertEqual(self.client.get("/api/history?device_id=unknown").json, [])
        self.assertEqual(self.client.get("/api/history", query_string={"device_id": "' OR 1=1 --"}).json, [])

    def test_invalid_time_is_400_not_server_failure(self):
        for hours in ("nan", "inf", "-inf", "0", "-1", "169", "bad", "1e10000", ""):
            for endpoint in ("history", "export.csv"):
                with self.subTest(hours=hours, endpoint=endpoint):
                    res = self.client.get(f"/api/{endpoint}", query_string={"hours": hours})
                    self.assertEqual(res.status_code, 400)
                    self.assertIn("error", res.json)

    def test_window_order_and_future_exclusion(self):
        self.insert(ts=self.now-7200000)
        self.insert(ts=self.now-1000)
        self.insert(ts=self.now-2000)
        self.insert(ts=self.now+3600000)
        rows = self.client.get("/api/history?hours=1&device_id=A").json
        self.assertEqual([r["ts"] for r in rows], [self.now-2000, self.now-1000])

    def test_over_limit_is_explicit_for_history_and_export(self):
        self.ns["HISTORY_MAX_ROWS"] = 2
        for _ in range(3): self.insert()
        for endpoint in ("history", "export.csv"):
            self.assertEqual(self.client.get(f"/api/{endpoint}").status_code, 413)

    def test_csv_preserves_flags_and_blank_reference_fields(self):
        self.insert(heart_rate=75, hr_valid=1, spo2=99, spo2_valid=0,
                    env_temp=-0.5, env_humidity=50, env_valid=1)
        res = self.client.get("/api/export.csv?device_id=A")
        self.assertEqual(res.status_code, 200)
        self.assertTrue(res.data.startswith(b"\xef\xbb\xbf"))
        self.assertIn("attachment", res.headers["Content-Disposition"])
        rows = list(csv.DictReader(io.StringIO(res.data.decode("utf-8-sig"))))
        self.assertEqual(len(rows), 1)
        row = rows[0]
        self.assertEqual(row["heart_rate"], "75.0")
        self.assertEqual(row["spo2"], "")
        self.assertEqual(row["spo2_valid"], "0")
        self.assertEqual(row["env_temp"], "-0.5")
        self.assertTrue(row["received_at_utc"].endswith("+00:00"))
        for key in ("reference_heart_rate_bpm", "reference_spo2_pct", "reference_notes"):
            self.assertEqual(row[key], "")

    def test_csv_device_formula_is_inert_and_quotes_roundtrip(self):
        device = '=HYPERLINK("bad"),中文\n设备'
        self.insert(device=device)
        res = self.client.get("/api/export.csv", query_string={"device_id": device})
        row = next(csv.DictReader(io.StringIO(res.data.decode("utf-8-sig"))))
        self.assertEqual(row["device_id"], "'"+device)

    def test_empty_export_has_useful_error(self):
        self.assertEqual(self.client.get("/api/history").json, [])
        self.assertEqual(self.client.get("/api/export.csv").status_code, 404)

    def test_ai_cache_includes_device_not_just_timestamp(self):
        self.insert("A", heart_rate=60, hr_valid=1)
        self.insert("B", heart_rate=90, hr_valid=1)
        self.ns["DEEPSEEK_API_KEY"] = "offline-test-only"
        self.ns["AI_MIN_INTERVAL_S"] = 0
        calls = []
        def fake_report(snapshot):
            calls.append(snapshot["device_id"])
            return "Report for " + snapshot["device_id"]
        self.ns["_call_deepseek"] = fake_report
        a = self.client.post("/api/ai_report?device_id=A", json={}).json
        b = self.client.post("/api/ai_report?device_id=B", json={}).json
        cached = self.client.post("/api/ai_report?device_id=B", json={}).json
        self.assertEqual(a["report"], "Report for A")
        self.assertEqual(b["report"], "Report for B")
        self.assertTrue(cached["cached"])
        self.assertEqual(calls, ["A", "B"])


if __name__ == "__main__":
    unittest.main()
