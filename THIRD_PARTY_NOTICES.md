# 第三方来源与许可范围

顶层 MIT 许可只覆盖本项目有权授权的原创代码及文档，不覆盖第三方原作、数据集、厂商 SDK 或模型训练数据。

| 内容 | 来源与许可 |
| --- | --- |
| `board/`、部分驱动及模板的 RT-Thread 衍生文件 | 保留文件内版权与 SPDX 标记，Apache-2.0 正文见 `LICENSES/Apache-2.0.txt` |
| `socc_proc/applications/algo/ppg_process.*`、`sensor/max30102.c`、`sensor/MAX30102.h` | Maxim MAXREFDES117 参考算法/驱动移植；算法另有 SocChina BL616 参考工程移植记录。Maxim 原始授权声明见 `LICENSES/Maxim-Integrated.txt`，不以顶层 MIT 抹去原条款 |
| `Edgi_Talk_M55_LVGL/applications/ui/fonts/noto_gb2312*`、`ui_font_*.c` | Noto Sans SC 子集及 LVGL 转换数据，SIL OFL 1.1，见 `LICENSES/NotoSansSC-OFL-1.1.txt`；修改/子集化及生成步骤见同目录脚本 |
| `dashboard/static/vendor/` | Chart.js 4.4.8，MIT，保留同目录 LICENSE 与库内声明 |
| 外部 RT-Thread / Infineon BSP / LVGL / TFLite Micro / CMSIS-NN / Ethos-U | 未分发；从原提供方取得并遵守其许可，见 `BUILDING.md` |
| 公开数据集、训练权重、真实心拍夹具 | 不随仓库分发，训练与导出脚本保留数据来源说明；自行取得、核查许可与归属 |
| 原 SAPI 音频输出 | 不分发；语音播放源码和 PCM 接口保留 |

核对参考：

- Maxim 声明保留于 [SparkFun MAX3010x 的 spo2_algorithm.h](https://github.com/sparkfun/SparkFun_MAX3010x_Sensor_Library/blob/master/src/spo2_algorithm.h)。本仓库算法包含时间口径、边界、比值及溢出修正，不宣称与上游逐字相同。
- [Google Fonts Noto Sans SC OFL](https://github.com/google/fonts/blob/main/ofl/notosanssc/OFL.txt)。

保留文件内其他版权声明；不因文件位于应用目录就将第三方内容重新许可。数据与模型的研究描述不构成其分发许可。
