#!/usr/bin/env python3
"""Generate LVGL font .c files with Chinese characters using lv_font_conv."""
import subprocess
import shutil
import os
import sys

FONT_SRC = os.path.normpath(os.path.join(
    os.path.dirname(__file__), "..", "..", "..",
    "libraries/components/lvgl_9.2.0/demos/multilang/assets/fonts/NotoSansSC-Medium.otf"
))
OUT_DIR = os.path.dirname(os.path.abspath(__file__))

# All Chinese characters needed for UI
CN_CHARS = (
    "实时监测血糖分析设置心率氧饱和度体温基础代谢环境湿"
    "次分正常偏高低危险量中最近趋势当前值预设备信息间隔"
    "关于版本连接状态蓝牙未已秒钟开始停止等待数据有效无"
    "红外指环保存确定取消载传感器健康终端创智慧医疗核"
    "平均更新前固件名称主控亮屏幕采样配置赛嵌入式竞"
    "全国大学生芯片系统计英飞凌题选人工能多参融合型号速上限下模推理部署"
    "返回在扫描失败可用网络找到密码输入搜索断重号"  # WiFi 配网相关
    "综评检报告律呼吸验性"  # 体检报告 tab(2026-07-08 新增)
    # 体检报告解读层(2026-07-09 新增)：评级/状态徽标/建议句/免责声明
    # (由 scratchpad/gen_report_strings.py 生成的完整字形集，含重复，lv_font_conv 自动去重)
    "生命体征综合评估健康建议优秀良好一般需关注中正常偏高低快慢等异未测窦性"
    "心率血氧律呼吸应激糖参考次分无早搏项检报告各指标均在范围，请继续保持的"
    "活方式。意休息避免剧烈运动如或伴有不适及时就医安静深后复明显尽查到信号"
    "结症状频离态属实验放松变与压力调节水平情、规作值为仅供数据采集将手稳定"
    "置于传感器本基算疗诊断依"
)
SPECIAL = "°²"

# Font definitions: (name, size, ascii_range, symbols)
FONTS = [
    ("ui_font_H1",           24, "0x20-0x7E", CN_CHARS + SPECIAL),
    ("ui_font_Title",        20, "0x20-0x7E", CN_CHARS + SPECIAL),
    ("ui_font_Subtitle",     16, "0x20-0x7E", CN_CHARS + SPECIAL),
    ("ui_font_Number_big",   40, "0x20-0x7E", "°²"),
    ("ui_font_Number_extra", 60, None,         "0123456789.-"),  # "-" 供"--"占位符用(体检报告评分数据不够时)
]

# lv_font_conv 生成的 ifdef include 块。Eclipse CDT 的 subdir.mk 不含 lvgl 父目录,
# "lvgl/lvgl.h" 编译不过(见 CLAUDE.md 构建陷阱)，生成后必须压回单行 #include "lvgl.h"
IFDEF_INCLUDE_BLOCK = (
    '#ifdef LV_LVGL_H_INCLUDE_SIMPLE\n'
    '#include "lvgl.h"\n'
    '#else\n'
    '#include "lvgl/lvgl.h"\n'
    '#endif\n'
)

def fix_include(path):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    fixed = text.replace(IFDEF_INCLUDE_BLOCK, '#include "lvgl.h"\n')
    if fixed != text:
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(fixed)
        return True
    return False

def find_npx():
    """Find npx executable (on Windows it's npx.cmd)."""
    npx = shutil.which("npx")
    if npx:
        return npx
    # Try common locations
    for p in [r"C:\Program Files\nodejs\npx.cmd", r"C:\Users\cjh\AppData\Roaming\npm\npx.cmd"]:
        if os.path.isfile(p):
            return p
    return "npx"

def main():
    if not os.path.isfile(FONT_SRC):
        print(f"ERROR: Font file not found: {FONT_SRC}", file=sys.stderr)
        sys.exit(1)

    npx = find_npx()
    print(f"Font source: {FONT_SRC}")
    print(f"Output dir:  {OUT_DIR}")
    print(f"npx path:    {npx}")
    print()

    for name, size, ascii_range, symbols in FONTS:
        out_path = os.path.join(OUT_DIR, f"{name}.c")
        cmd = [
            npx, "lv_font_conv",
            "--font", FONT_SRC,
            "--size", str(size),
            "--bpp", "4",
            "--format", "lvgl",
            "-o", out_path,
            "--symbols", symbols,
            "--no-compress",
        ]
        if ascii_range:
            cmd.extend(["--range", ascii_range])

        print(f"[{name}] size={size}px ...", end=" ", flush=True)
        try:
            result = subprocess.run(cmd, check=True, capture_output=True, text=True,
                                    encoding="utf-8", shell=(os.name == 'nt'))
            # Verify output file
            if os.path.isfile(out_path):
                fix_include(out_path)
                sz = os.path.getsize(out_path)
                print(f"OK ({sz // 1024}KB)")
            else:
                print("WARN: file not created!")
        except subprocess.CalledProcessError as e:
            print(f"FAILED!")
            print(f"  stderr: {e.stderr}")
            sys.exit(1)

    print("\nAll fonts generated successfully!")

if __name__ == "__main__":
    main()
