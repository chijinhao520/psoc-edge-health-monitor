#!/usr/bin/env python3
"""
生成 lv_tiny_ttf 用的中文子集 TTF: noto_gb2312.ttf

流程: 系统 NotoSansSC-VF.ttf --instancer wght=500--> 静态 Medium
      --pyftsubset(GB2312 全集)--> 子集 TTF(约 2MB)

关键约束: 必须 TrueType(glyf) 轮廓 —— 本工程打包的 stb_truetype 不支持 CFF/OTF，
          故不能用工程内 NotoSansSC-Medium.otf；改用系统 VF 实例化。
依赖: fonttools (pip install fonttools)
产出后跑 `python gen_ttf_c.py` 转成 noto_gb2312_ttf.c(C 数组) 供编译。
"""
import os, sys, subprocess

HERE     = os.path.dirname(os.path.abspath(__file__))
SRC_VF   = "C:/Windows/Fonts/NotoSansSC-VF.ttf"
INSTANCE = os.path.join(HERE, "_noto_medium_static.ttf")   # 中间产物
SUBSET   = os.path.join(HERE, "noto_gb2312.ttf")
UNIFILE  = os.path.join(HERE, "_gb2312_unicodes.txt")

def build_charset():
    chars = set()
    # GB2312 全集(一级+二级 6763 字)
    for hi in range(0xB0, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                chars.add(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    # ASCII 可见字符
    for c in range(0x20, 0x7F):
        chars.add(chr(c))
    # 常用中文标点/符号
    for c in "，。、；：？！“”‘’（）《》【】—…·％°℃±×÷≥≤～":
        chars.add(c)
    return chars

def main():
    chars = build_charset()
    print(f"字符集: {len(chars)} 个 (GB2312 全集 + ASCII + 标点)")

    with open(UNIFILE, "w", encoding="utf-8") as f:
        f.write(",".join("U+%04X" % ord(c) for c in sorted(chars)))

    if not os.path.isfile(SRC_VF):
        print(f"ERROR: 找不到字体源 {SRC_VF}", file=sys.stderr)
        sys.exit(1)

    print("[1/2] 实例化 VF -> 静态 Medium(wght=500) ...", flush=True)
    subprocess.run([sys.executable, "-m", "fontTools.varLib.instancer",
                    SRC_VF, "wght=500", "-o", INSTANCE],
                   check=True, capture_output=True, text=True)

    print("[2/2] 子集化到 GB2312 全集 ...", flush=True)
    subprocess.run(["pyftsubset", INSTANCE,
                    f"--unicodes-file={UNIFILE}",
                    f"--output-file={SUBSET}",
                    "--no-hinting", "--desubroutinize",
                    "--drop-tables+=DSIG", "--name-IDs=",
                    "--notdef-outline", "--layout-features=", "--glyph-names"],
                   check=True, capture_output=True, text=True)

    # 清理中间产物
    for p in (INSTANCE, UNIFILE):
        if os.path.isfile(p):
            os.remove(p)

    sz = os.path.getsize(SUBSET)
    print(f"\n产出: {SUBSET}  ({sz/1024/1024:.2f} MB)")
    print("下一步: python gen_ttf_c.py  (转 C 数组)")

if __name__ == "__main__":
    main()
