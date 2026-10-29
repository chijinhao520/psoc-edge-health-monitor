import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("build_pair", Path(__file__).resolve().parents[1] / "build_pair.py")
bp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bp)
sdk_spec = importlib.util.spec_from_file_location("sdk_patch", Path(__file__).resolve().parents[1] / "sdk_patch.py")
sdk = importlib.util.module_from_spec(sdk_spec)
sdk_spec.loader.exec_module(sdk)


class PairTests(unittest.TestCase):
    def test_sdk_apply_and_repeat(self):
        text = (bp.ROOT / sdk.TARGET).read_text(encoding="utf-8")
        for old, new in reversed(sdk.REPLACEMENTS):
            text = text.replace(new, old)
        candidates = [text.encode("utf-8"), text.replace("\n", "\r\n").encode("utf-8")]
        original = next(raw for raw in candidates if sdk.hashlib.sha256(raw).hexdigest() == sdk.ORIGINAL_SHA256)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            p = root / sdk.TARGET
            p.parent.mkdir(parents=True)
            p.write_bytes(original)
            with self.assertRaises(ValueError):
                sdk.check(root)
            self.assertEqual(sdk.apply(root), "applied")
            sdk.check(root)
            updated = p.read_bytes()
            self.assertEqual(sdk.apply(root), "already-applied")
            self.assertEqual(p.read_bytes(), updated)
            self.assertEqual((root / "cache/sdk-patches" / (sdk.ORIGINAL_SHA256 + ".c")).read_bytes(), original)

    def test_sdk_unknown_version_unchanged(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            p = root / sdk.TARGET
            p.parent.mkdir(parents=True)
            p.write_bytes(b"unknown SDK")
            with self.assertRaises(ValueError):
                sdk.apply(root)
            self.assertEqual(p.read_bytes(), b"unknown SDK")
        with self.assertRaises(ValueError):
            sdk.patched_text("no patch anchors")

    def test_ipc_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            a = root / "socc_proc/applications/app/ipc_shared_data.h"
            b = root / "Edgi_Talk_M55_LVGL/applications/ui/ipc_shared_data.h"
            for p in (a, b):
                p.parent.mkdir(parents=True)
                p.write_text("different" if p == a else "header", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "IPC"):
                bp.check_ipc(root)

    def test_real_ipc(self):
        self.assertEqual(len(bp.check_ipc(bp.ROOT)["magic"]), 4)

    @staticmethod
    def section(lma="08340400", size="00000020", flags="CONTENTS, ALLOC, LOAD"):
        return f"  1 .text {size} 08340400 {lma} 00001400 2**2\n    {flags}\n"

    def test_load_address_aliases(self):
        self.assertEqual(bp.load_sections(self.section(), 0x08340000, 0x08540000)[0]["size"], 32)
        with self.assertRaisesRegex(ValueError, "超出"):
            bp.load_sections(self.section("60340400"), 0x08340000, 0x08540000)
        self.assertEqual(len(bp.load_sections(self.section("60580400"), 0x60580000, 0x60D80000)), 1)

    def test_empty_overlap_and_boundary(self):
        with self.assertRaisesRegex(ValueError, "没有"):
            bp.load_sections(self.section(flags="ALLOC"), 0x08340000, 0x08540000)
        with self.assertRaisesRegex(ValueError, "重叠"):
            bp.load_sections(self.section() * 2, 0x08340000, 0x08540000)
        with self.assertRaisesRegex(ValueError, "超出"):
            bp.load_sections(self.section("0853fff0"), 0x08340000, 0x08540000)
        self.assertEqual(len(bp.load_sections(self.section("0853ffe0"), 0x08340000, 0x08540000)), 1)

    def package(self, folder):
        names = ["m33/rtthread.elf", "m55/rtthread.elf", "ipc_shared_data.h", "source-inputs.json"]
        files = {}
        for name in names:
            p = folder / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(name.encode())
            files[name] = bp.sha256(p)
        manifest = {"schema": 1, "kind": "paired-application-elf", "files": files,
                    "ipc": {"sha256": files["ipc_shared_data.h"]}}
        bp.write_json(folder / "manifest.json", manifest)
        return manifest

    def test_verify_corruption_and_missing(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            self.package(folder)
            bp.verify_package(folder)
            (folder / "m33/rtthread.elf").write_bytes(b"old firmware")
            with self.assertRaisesRegex(ValueError, "校验失败"):
                bp.verify_package(folder)
            self.package(folder)
            (folder / "m55/rtthread.elf").unlink()
            with self.assertRaisesRegex(ValueError, "缺失"):
                bp.verify_package(folder)

    def test_manifest_completeness_and_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            manifest = self.package(folder)
            del manifest["files"]["m33/rtthread.elf"]
            bp.write_json(folder / "manifest.json", manifest)
            with self.assertRaisesRegex(ValueError, "缺失"):
                bp.verify_package(folder)
            manifest = self.package(folder)
            manifest["files"]["../outside"] = "invalid"
            bp.write_json(folder / "manifest.json", manifest)
            with self.assertRaisesRegex(ValueError, "校验失败"):
                bp.verify_package(folder)


if __name__ == "__main__":
    unittest.main()
