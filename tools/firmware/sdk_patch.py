"""对登记的SDK源码应用可审阅、幂等的最小修复；未知版本拒绝改写。"""
import argparse
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TARGET = Path("Edgi_Talk_M55_LVGL/libraries/components/mtb-device-support-pse8xxgp/pdl/drivers/third_party/COMPONENT_GFXSS/vsi/gcnano/vg_lite_hal.c")
ORIGINAL_SHA256 = "c074709a44102f28d7a4fee96aa611643840a0e4c34953e3d8adb59c27ae78b8"
MARKER = "PROJECT_GPU_CLEANUP_FIX_V1"
REPLACEMENTS = (
    ("        /* Free up the device structure. */\n        vg_lite_hal_free(device);",
     "        /* PROJECT_GPU_CLEANUP_FIX_V1: Device is static in baremetal. */\n"
     "#if !_BAREMETAL\n        vg_lite_hal_free(device);\n#endif\n        device = NULL;"),
    ("    /* Setup register memory. **********************************************/",
     "    /* Cleanup must be safe even if the first heap allocation fails. */\n"
     "    for (i = 0; i < VG_SYSTEM_RESERVE_COUNT; i++) {\n"
     "        INIT_LIST_HEAD(&device->heap[i].list);\n    }\n\n"
     "    /* Setup register memory. **********************************************/"),
)


def patched_text(text):
    for old, new in REPLACEMENTS:
        if text.count(old) != 1:
            raise ValueError("SDK修复锚点不唯一或不存在，拒绝应用")
        text = text.replace(old, new)
    return text


def check(root=ROOT):
    text = (root / TARGET).read_text(encoding="utf-8")
    if not (MARKER in text and all(text.count(new) == 1 for _, new in REPLACEMENTS)):
        raise ValueError("GPU SDK清理修复缺失，请运行 python tools/firmware/sdk_patch.py apply")


def apply(root=ROOT):
    target = root / TARGET
    raw = target.read_bytes()
    text = raw.decode("utf-8").replace("\r\n", "\n")
    if MARKER in text:
        check(root)
        return "already-applied"
    if hashlib.sha256(raw).hexdigest() != ORIGINAL_SHA256:
        raise ValueError("GPU SDK版本或本地内容已变化，需审查补丁，拒绝覆盖")
    updated = patched_text(text)
    backup = root / "cache/sdk-patches" / (ORIGINAL_SHA256 + ".c")
    backup.parent.mkdir(parents=True, exist_ok=True)
    if backup.exists() and backup.read_bytes() != raw:
        raise ValueError("原文件备份不匹配")
    backup.write_bytes(raw)
    target.write_bytes(updated.replace("\n", "\r\n").encode("utf-8") if b"\r\n" in raw else updated.encode("utf-8"))
    check(root)
    return "applied"


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("command", choices=("apply", "check"))
    args = p.parse_args()
    try:
        print(apply() if args.command == "apply" else (check() or "checked"))
    except (ValueError, OSError) as exc:
        p.exit(1, f"{exc}\n")
