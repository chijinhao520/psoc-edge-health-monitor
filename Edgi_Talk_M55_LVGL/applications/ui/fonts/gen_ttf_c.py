#!/usr/bin/env python3
"""
把子集化后的 noto_gb2312.ttf 转成 C const 数组源文件 noto_gb2312_ttf.c。

- const uint8_t[] 落 .rodata -> linker m55_nvm region -> 外部 QSPI flash(0x60580000)，不占内部 SRAM。
- __attribute__((used)) 防止 -ffunction-sections/--gc-sections 把未直接引用的数组 strip 掉。
- 供 ui_cn_font.c 的 lv_tiny_ttf_create_data_ex() 运行时栅格化任意 GB2312 汉字。

先跑 subset_ttf.py 产出 noto_gb2312.ttf，再跑本脚本。
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
TTF = os.path.join(HERE, "noto_gb2312.ttf")
OUT = os.path.join(HERE, "noto_gb2312_ttf.c")

def main():
    with open(TTF, "rb") as f:
        data = f.read()

    lines = []
    lines.append("/* 自动生成 by gen_ttf_c.py —— 勿手改 */")
    lines.append("/* GB2312 全集 NotoSansSC 子集 TTF, 供 lv_tiny_ttf 运行时栅格化中文 */")
    lines.append("/* const 数组落 .rodata -> m55_nvm region -> 外部 QSPI flash, 不占内部 SRAM */")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append("__attribute__((used, aligned(4)))")
    lines.append("const uint8_t noto_gb2312_ttf[] = {")
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append("  " + ",".join("0x%02x" % b for b in chunk) + ",")
    lines.append("};")
    lines.append("const uint32_t noto_gb2312_ttf_size = sizeof(noto_gb2312_ttf);")
    lines.append("")

    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))

    print(f"输入 TTF : {len(data)} bytes ({len(data)/1024/1024:.2f} MB)")
    print(f"输出 .c  : {OUT} ({os.path.getsize(OUT)/1024/1024:.2f} MB)")

if __name__ == "__main__":
    main()
