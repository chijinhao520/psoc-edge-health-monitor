"""双核应用固件：检查、强制重编译、离线回归、成套归档；不连接或烧录设备。"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
PROJECTS = {"m33": "socc_proc", "m55": "Edgi_Talk_M55_LVGL"}
DEFAULT_GCC = Path("E:/RT-ThreadStudio/repo/Extract/ToolChain_Support_Packages/ARM/GNU_Tools_for_ARM_Embedded_Processors/13.3/bin")
DEFAULT_MAKE = Path("E:/RT-ThreadStudio/platform/env_released/env-new/tools/BuildTools/2.12-20190422-1053/bin/make.exe")
DEFAULT_LINKER = Path("E:/RT-ThreadStudio/plugins/org.rt-thread.studio.project.gener_1.0.30/builder/project_linker.exe")
SOURCE_DIRS = ("applications", "board", "config", "libs", "libraries", "packages", "rt-thread")
SOURCE_EXTS = {".c", ".cpp", ".cc", ".cxx", ".h", ".hpp", ".s", ".inc", ".ld", ".a", ".json", ".mk", ".py"}


def sha256(path: Path) -> str:
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def write_json(path: Path, value) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def run(args, *, cwd=ROOT, env=None, log=None) -> str:
    if log is not None:
        # 编译器的本机编码原样保留在日志，不把数千行编译命令刷入终端。
        with Path(log).open("wb") as f:
            result = subprocess.run([str(a) for a in args], cwd=cwd, env=env,
                                    stdout=f, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError(f"命令失败 ({result.returncode})，见 {log}")
        return ""
    return subprocess.check_output([str(a) for a in args], cwd=cwd, env=env,
                                   encoding="utf-8", errors="replace").strip()


def resolve_tool(explicit, name, fallback):
    path = Path(explicit or shutil.which(name) or fallback).resolve()
    if not path.is_file():
        raise ValueError(f"找不到工具 {name}: {path}")
    return path


def check_ipc(root: Path) -> dict:
    a = root / PROJECTS["m33"] / "applications/app/ipc_shared_data.h"
    b = root / PROJECTS["m55"] / "applications/ui/ipc_shared_data.h"
    if a.read_bytes() != b.read_bytes():
        raise ValueError("两核IPC头文件不同，禁止生成配套固件")
    names = ("IPC_MAGIC", "IPC_PPG_MAGIC", "IPC_ECG_MAGIC", "IPC_CMD_MAGIC")
    text = a.read_text(encoding="utf-8")
    magic = {}
    for name in names:
        match = re.search(rf"^#define\s+{name}\s+(0x[0-9A-Fa-f]+)", text, re.M)
        if not match:
            raise ValueError(f"无法读取协议标记 {name}")
        magic[name] = match[1]
    return {"sha256": sha256(a), "magic": magic}


def source_snapshot(root: Path) -> dict:
    """含本机SDK/模型头/静态库；内容只记录hash，不复制配置中的凭据。"""
    files = set()
    for project in PROJECTS.values():
        base = root / project
        for directory in SOURCE_DIRS:
            for folder, dirs, names in os.walk(base / directory):
                dirs[:] = [d for d in dirs if d not in {".git", "__pycache__", "Debug", "Release"}]
                for name in names:
                    p = Path(folder) / name
                    if p.suffix.lower() in SOURCE_EXTS or name in {"SConscript", "SConstruct"}:
                        files.add(p)
        for p in base.iterdir():
            if p.is_file() and (p.suffix.lower() in SOURCE_EXTS or p.name in
                               {".cproject", ".project", "SConstruct", "SConscript", "makefile.defs", "makefile.targets"}):
                files.add(p)
        files.update((base / "Debug").rglob("*.mk"))
        files.add(base / "Debug/makefile")
    # 验证逻辑也属于本次构建证据。
    for directory in ("tools/firmware", "socc_proc/tools/accuracy_sil", "socc_proc/tools/hr_sil"):
        files.update(p for p in (root / directory).rglob("*")
                     if p.is_file() and p.suffix.lower() in {".py", ".ps1", ".c", ".h"})
    return {p.relative_to(root).as_posix(): sha256(p) for p in sorted(files)}


def load_sections(output: str, low: int, high: int) -> list:
    """只接受实际有内容的LOAD段；防把RAM调试镜像误当当前XIP固件。"""
    sections = []
    lines = output.splitlines()
    for i, line in enumerate(lines[:-1]):
        m = re.match(r"\s*\d+\s+(\S+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+", line)
        if not m or "LOAD" not in lines[i + 1] or "CONTENTS" not in lines[i + 1]:
            continue
        name, size, vma, lma = m.groups()
        size, vma, lma = int(size, 16), int(vma, 16), int(lma, 16)
        if size == 0:
            continue
        if not low <= lma < lma + size <= high:
            raise ValueError(f"{name}加载地址超出应用区: {lma:#x}+{size:#x}")
        sections.append({"name": name, "size": size, "vma": vma, "lma": lma})
    if not sections:
        raise ValueError("ELF没有可加载的应用段")
    ranges = sorted((s["lma"], s["lma"] + s["size"]) for s in sections)
    if any(a[1] > b[0] for a, b in zip(ranges, ranges[1:])):
        raise ValueError("ELF加载段发生重叠")
    return sections


def verify_package(folder: Path) -> dict:
    folder = folder.resolve()
    manifest = json.loads((folder / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("schema") != 1 or manifest.get("kind") != "paired-application-elf":
        raise ValueError("不是支持的双核应用包")
    files = manifest.get("files", {})
    required = {"m33/rtthread.elf", "m55/rtthread.elf", "ipc_shared_data.h", "source-inputs.json"}
    if not required <= files.keys():
        raise ValueError("配套文件缺失")
    for name, expected in files.items():
        p = (folder / name).resolve()
        if not p.is_relative_to(folder) or not p.is_file() or sha256(p) != expected:
            raise ValueError(f"文件缺失或校验失败: {name}")
    if files["ipc_shared_data.h"] != manifest["ipc"]["sha256"]:
        raise ValueError("IPC协议校验不一致")
    return manifest


def build(args) -> Path:
    from sdk_patch import check as check_sdk_patch
    check_sdk_patch(ROOT)
    gcc = resolve_tool(str(Path(args.toolchain) / "arm-none-eabi-gcc.exe") if args.toolchain else None,
                       "arm-none-eabi-gcc", DEFAULT_GCC / "arm-none-eabi-gcc.exe")
    make = resolve_tool(args.make, "make", DEFAULT_MAKE)
    linker = resolve_tool(args.project_linker, "project_linker.exe", DEFAULT_LINKER)
    objdump = gcc.with_name("arm-none-eabi-objdump.exe")
    size = gcc.with_name("arm-none-eabi-size.exe")
    if not objdump.is_file() or not size.is_file():
        raise ValueError("ARM工具链缺少objdump或size")
    env = os.environ.copy()
    env["PATH"] = os.pathsep.join(map(str, (gcc.parent, make.parent, linker.parent))) + os.pathsep + env.get("PATH", "")
    ipc = check_ipc(ROOT)
    for project in PROJECTS.values():
        if not (ROOT / project / "Debug/makefile").is_file():
            raise ValueError(f"请先在RT-Thread Studio生成 {project} 的构建文件")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    folder = Path(args.output).resolve() if args.output else ROOT / "cache/firmware" / stamp
    folder.mkdir(parents=True, exist_ok=False)  # 不覆盖旧包；失败目录保留日志且无manifest。
    before = source_snapshot(ROOT)
    print(f"输入快照: {len(before)}个文件；输出 {folder}", flush=True)
    tests = [
        ("accuracy", ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "socc_proc/tools/accuracy_sil/run.ps1"]),
        ("device", [sys.executable, "socc_proc/tools/accuracy_sil/device_system_sil.py"]),
        ("cross-layer", [sys.executable, "socc_proc/tools/accuracy_sil/cross_layer_sil.py"]),
        ("heart-rate", ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "socc_proc/tools/hr_sil/run.ps1"]),
        ("gpu-cleanup", [sys.executable, "socc_proc/tools/accuracy_sil/gpu_cleanup_sil.py"]),
        ("checkup", [sys.executable, "socc_proc/tools/accuracy_sil/checkup_sil.py"]),
    ]
    for name, command in tests:
        print(f"离线回归: {name}", flush=True)
        run(command, env=env, log=folder / f"test-{name}.log")
    cores = {}
    for core, project in PROJECTS.items():
        print(f"强制重新编译 {core}（不运行烧录/后处理目标）", flush=True)
        dest = folder / core
        dest.mkdir()
        run([make, "-B", f"-j{args.jobs}", "rtthread.elf"], cwd=ROOT / project / "Debug",
            env=env, log=dest / "build.log")
        elf = ROOT / project / "Debug/rtthread.elf"
        desc = run([objdump, "-h", elf], env=env)
        low, high = (0x08340000, 0x08540000) if core == "m33" else (0x60580000, 0x60D80000)
        sections = load_sections(desc, low, high)
        (dest / "sections.txt").write_text(desc + "\n", encoding="utf-8")
        (dest / "size.txt").write_text(run([size, elf], env=env) + "\n", encoding="utf-8")
        for name in ("rtthread.elf", "rtthread.map"):
            shutil.copy2(ROOT / project / "Debug" / name, dest / name)
        cores[core] = {"project": project, "load_sections": sections,
                       "load_bytes": sum(s["size"] for s in sections),
                       "flash_alias_offset": 0x58000000 if core == "m33" else 0}
    after = source_snapshot(ROOT)
    if before != after:
        changed = [p for p in before.keys() | after.keys() if before.get(p) != after.get(p)]
        write_json(folder / "changed-during-build.json", sorted(changed))
        raise RuntimeError("构建期间输入发生变化，未生成有效固件包；见changed-during-build.json")
    if check_ipc(ROOT) != ipc:
        raise RuntimeError("构建期间IPC发生变化")
    write_json(folder / "source-inputs.json", after)
    shutil.copy2(ROOT / "socc_proc/applications/app/ipc_shared_data.h", folder / "ipc_shared_data.h")
    shutil.copy2(Path(__file__).with_name("README.md"), folder / "README.md")
    files = {p.relative_to(folder).as_posix(): sha256(p) for p in sorted(folder.rglob("*")) if p.is_file()}
    manifest = {"schema": 1, "kind": "paired-application-elf", "created_utc": stamp,
                "compiler": run([gcc, "--version"], env=env).splitlines()[0],
                "make": run([make, "--version"], env=env).splitlines()[0],
                "git_head": run(["git", "rev-parse", "HEAD"]),
                "working_tree_dirty": bool(run(["git", "status", "--porcelain", "--untracked-files=normal"])),
                "build_mode": "force-rebuild", "tests": [name for name, _ in tests],
                "ipc": ipc, "cores": cores, "files": files,
                "limitations": ["未烧录或验证实板；不含安全启动镜像、签名或出厂恢复流程",
                                "校验用于检查配套与损坏，不提供数字签名认证",
                                "资源统计包含链接器预留区，不是运行时堆栈峰值"]}
    write_json(folder / "manifest.json", manifest)  # 只有全部成功才写完成标记。
    verify_package(folder)
    print(f"双核应用包已通过校验: {folder}", flush=True)
    return folder


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    b = sub.add_parser("build", help="离线回归 + 两核强制重编译 + 成套归档")
    b.add_argument("--output", help="必须是尚不存在的目录")
    b.add_argument("--jobs", type=int, default=4)
    b.add_argument("--toolchain", help="ARM GCC bin目录")
    b.add_argument("--make", help="GNU make可执行文件")
    b.add_argument("--project-linker", help="RT-Thread Studio project_linker.exe")
    v = sub.add_parser("verify", help="离线校验已生成包，不需要编译器")
    v.add_argument("folder", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "verify":
            verify_package(args.folder)
            print("配套文件与SHA256校验通过")
        else:
            if not 1 <= args.jobs <= 32:
                raise ValueError("jobs必须在1到32之间")
            build(args)
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f"失败: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
