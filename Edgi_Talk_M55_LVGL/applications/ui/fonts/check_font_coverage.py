# -*- coding: utf-8 -*-
r"""校验 UI 源码里的中文文案是否都在预生成字库子集内。

为什么需要它
------------
ui_font_*.c 是 lv_font_conv 生成的**固定子集**（字表写死在文件头的
`--symbols` 参数里），不是 tiny_ttf 全集。子集外的字在屏上渲染成空白/方块，
而**编译期毫无征兆** —— 只有上板点进那个界面才看得见。

2026-07-16 实例：给测量失败态加提示"请勿移动"，"勿"和"移"都不在 365 字的
子集里，差点上板才发现是一排方块。

用法
----
    python -X utf8 check_font_coverage.py            # 扫全部 UI 源码
    python -X utf8 check_font_coverage.py "新文案"    # 只校验一段候选文案

注意源码里的中文是 "\xe6\xb5\x8b" 这种 UTF-8 转义串（项目惯例，避免源文件
编码问题），本脚本会解码后再比对。
"""
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
UI_DIR = os.path.dirname(HERE)


def load_symbols(font_c):
    """从 lv_font_conv 生成的 .c 文件头提取 --symbols 字表 + --range。"""
    head = open(font_c, "rb").read(6000).decode("utf-8", errors="replace")
    syms = set()
    m = re.search(r"--symbols\s+(.+?)\s+--(?:no-compress|range|format|lcd)", head, re.S)
    if m:
        syms |= set(m.group(1))
    for rng in re.finditer(r"--range\s+0x([0-9A-Fa-f]+)-0x([0-9A-Fa-f]+)", head):
        lo, hi = int(rng.group(1), 16), int(rng.group(2), 16)
        syms |= {chr(c) for c in range(lo, hi + 1)}
    for rng in re.finditer(r"--range\s+0x([0-9A-Fa-f]+)(?!\s*-)", head):
        syms.add(chr(int(rng.group(1), 16)))
    return syms


def strip_comments(src):
    """剥掉 C 注释，只留真正会上屏的字符串字面量。

    必须先剥 —— 本项目注释全是中文且大量引用文案，不剥的话满屏假阳性，
    真正的缺字反而被淹没。用空格替换以保持行号不变。
    """
    def repl(m):
        s = m.group(0)
        if s.startswith("/"):
            return "".join(c if c == "\n" else " " for c in s)
        return s
    # 字符串字面量优先匹配，避免把 "http://x" 里的 // 当注释
    pattern = r'"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)*\'|/\*.*?\*/|//[^\n]*'
    return re.sub(pattern, repl, src, flags=re.S)


def decode_escapes(src):
    r"""把源码里的 "\xe6\xb5\x8b..." 转义串还原成中文。

    只认连续的 \xNN 序列；相邻字符串字面量(C 会自动拼接)也一并还原。
    """
    out = []
    for m in re.finditer(r'((?:"(?:[^"\\]|\\x[0-9A-Fa-f]{2}|\\.)*"\s*)+)', src):
        blob = m.group(1)
        raw = bytearray()
        for lit in re.finditer(r'"((?:[^"\\]|\\x[0-9A-Fa-f]{2}|\\.)*)"', blob):
            body = lit.group(1)
            i = 0
            while i < len(body):
                if body[i] == "\\" and i + 1 < len(body) and body[i + 1] == "x":
                    raw.append(int(body[i + 2:i + 4], 16))
                    i += 4
                elif body[i] == "\\":
                    i += 2
                else:
                    # 源码里偶有裸中文字面量（非 \x 转义），按 UTF-8 收进来
                    raw.extend(body[i].encode("utf-8"))
                    i += 1
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            continue
        if any(ord(c) > 0x7F for c in text):
            out.append((m.start(), text))
    return out


def main():
    fonts = sorted(glob.glob(os.path.join(HERE, "ui_font_*.c")))
    if not fonts:
        print("!! 没找到 ui_font_*.c")
        return 1

    tables = {}
    for f in fonts:
        syms = load_symbols(f)
        if syms:
            tables[os.path.basename(f)[:-2]] = syms
    print("已加载字库子集：")
    for name, syms in tables.items():
        print("  %-24s %d 个字符" % (name, len(syms)))
    print()

    # 命令行给了候选文案 → 只校验它
    if len(sys.argv) > 1:
        text = " ".join(sys.argv[1:])
        print('校验文案: "%s"\n' % text)
        for name, syms in tables.items():
            missing = [c for c in text if not c.isspace() and c not in syms]
            status = "缺字: " + " ".join(missing) if missing else "全部覆盖"
            print("  %-24s %s" % (name, status))
        return 0

    # 否则扫全部 UI 源码。用 Subtitle 子集当基准 —— 状态栏/卡片标题都用它
    base = tables.get("ui_font_Subtitle")
    if base is None:
        print("!! 没有 ui_font_Subtitle，跳过全量扫描")
        return 1

    bad = 0
    maybe = 0
    for c_file in sorted(glob.glob(os.path.join(UI_DIR, "*.c"))):
        raw = open(c_file, "rb").read().decode("utf-8", errors="replace")
        src = strip_comments(raw)
        # 该文件是否混用了 tiny_ttf(GB2312 全集)？用了的话缺字判定不作数 ——
        # 本脚本无法静态知道每个 label 绑的是哪个字体，只能标出来让人确认。
        uses_ttf = "ui_cn_font_get" in src
        for pos, text in decode_escapes(src):
            missing = sorted({c for c in text
                              if not c.isspace() and ord(c) > 0x7F and c not in base})
            if not missing:
                continue
            line = src[:pos].count("\n") + 1
            if uses_ttf:
                maybe += 1
                print("  ? %s:%d  (本文件有 ui_cn_font_get，若该 label 用 tiny_ttf 则可忽略)"
                      % (os.path.basename(c_file), line))
            else:
                bad += 1
                print("  ✗ %s:%d" % (os.path.basename(c_file), line))
            print('      "%s"' % text)
            print("      不在 ui_font_Subtitle 子集: %s" % " ".join(missing))
    print()
    if bad == 0 and maybe == 0:
        print("✓ 所有转义中文文案都在 ui_font_Subtitle 子集内")
    else:
        print("确定缺字 %d 处；需人工确认 %d 处（文件混用了 tiny_ttf）。" % (bad, maybe))
        print("修法二选一：")
        print("  a) 换用子集内已有的字改写文案（最快，无需重新生成字库）")
        print("  b) 该 label 改用 ui_cn_font_get(px) 的 tiny_ttf 全集字体")
    return 0


if __name__ == "__main__":
    sys.exit(main())
