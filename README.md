# PSoC Edge Health Monitor

**基于 PSoC Edge E84 的双核多参数健康监测终端**

[English](README_EN.md) · [完整图文介绍](docs/project-overview.zh-CN.md) · [构建与依赖](BUILDING.md) · [许可证说明](THIRD_PARTY_NOTICES.md)

2026 嵌入式芯片与系统设计竞赛作品的源码公开版本。Cortex-M33 负责传感器采集，Cortex-M55 负责心电分析、LVGL 交互和联网扩展，两核运行独立 RT-Thread 实例。

这是**应用源码与配置覆盖层**，不是包含全部厂商 SDK 的即开即编译整包。原开发环境的双核构建与离线回归已完成；公开版需要恢复外部 BSP、库及生成资源，未做公开版实板验收。具体缺项见 [BUILDING.md](BUILDING.md)。

![作品功能全景](docs/images/04-capabilities.png)

## 功能

- 引导式体检：一键开始，依次完成手指心率/血氧和心电/HRV 检测。
- 接触及质量提示：稳定等待、失败、超时、重试、跳过、取消和单组重测。
- 本次报告：轮次隔离、冻结快照、项目完成状态；缺项不提供规则总评分。
- 信号处理：双 PPG 质量选择、采样连续性检查、ECG 速率适配、HRV 与呼吸率估计。
- 辅助监测：红外温度、环境温湿度和 GSR 相对变化指标。
- LVGL 中文触屏界面：监测、报告、设置、Wi-Fi 扫描与手动连接、各项数据状态。
- 异步语音播报逻辑：只播有效项；音频资源需自行生成。
- 网页配套：按设备查看趋势、有效上传记录占比、对照 CSV 导出、可选 DeepSeek 文本解读。
- 工程工具：算法/驱动/流程回归、协议布局检查、双核配套构建与 SHA256 校验。

本次快照重启后清除；网页遥测历史不等于冻结报告存储。血糖、血压、BMR 和血流速度研究代码不代表已经验证的测量能力。原型不用于医疗诊断，离线测试不证明真人测量精度。

## 界面示例

![监测与本次报告](docs/images/03-interface.png)

图片使用生产 LVGL 页面及模拟数据进行软件渲染，**不是实板测量截图**。

## 目录

| 路径 | 内容 |
| --- | --- |
| `socc_proc/applications/` | M33 采集、驱动、算法和共享数据 |
| `Edgi_Talk_M55_LVGL/applications/` | M55 分析、推理适配、UI、语音与联网 |
| `dashboard/` | Flask、MQTT、网页趋势与导出 |
| `arrhythmia_training/scripts/` | 心律数据预处理、训练、量化与导出脚本 |
| `glucose_training/scripts/` | 血糖估算研究脚本；不是有效产品输出 |
| `socc_proc/tools/accuracy_sil/` | 算法、驱动、跨层、体检与 UI 离线验证 |
| `tools/firmware/` | 双核应用包与 SDK 修复工具 |
| `docs/` | 作品介绍、配图及工程记录 |

## 先运行网页离线测试

Python 3.11+，在仓库根目录执行：

```sh
python -m venv .venv
# 激活环境后：Windows .venv\Scripts\activate；Linux/macOS source .venv/bin/activate
python -m pip install -r dashboard/requirements.txt
python -m unittest discover -s dashboard/tests -v
```

测试使用内存数据库，不需要设备、MQTT 服务或 AI 密钥。已提供 [GitHub Actions 模板](docs/ci/github-actions-checks.yml)，仅运行网页测试；本次发布账号凭据无法写入工作流，因此尚未启用自动测试。仓库维护者取得工作流写入权限后，可将模板复制到 `.github/workflows/checks.yml` 启用。该配置不执行 MCU 编译。

运行网页前，参照 [dashboard/README.md](dashboard/README.md) 配置自己的 MQTT 服务。公开版固件与网页默认使用 `localhost` 示例地址，Wi-Fi 预设为空，AI 密钥为空。网页当前没有完备的用户认证和访问控制，MQTT 示例也未配置 TLS；仅在受控开发网络中使用模拟数据，正式处理健康数据前需要补齐部署保护。

## 验证状态

原开发环境 2026-10-29 记录：算法 2898 项、驱动 142 项检查通过；心率 5 档采样率共 40 个合成场景通过；六组固件回归及双核完整构建通过。LVGL 软件渲染检查覆盖 7 种引导状态。这些记录来自含外部依赖的原工程，不能等同于全新克隆后的构建结果。

公开快照在网络默认值、第三方声明和资源分发范围上与原固件包不同。公开版测试情况见 [RELEASE_NOTES.md](RELEASE_NOTES.md)。硬件联调、持续运行与参考仪器精度对照仍待完成。

## 许可证与贡献

原创代码采用 [MIT](LICENSE)。RT-Thread 衍生文件、Maxim 参考算法/驱动、Noto 字体、Chart.js 等保持各自许可，见 [第三方说明](THIRD_PARTY_NOTICES.md)。不分发个人采集数据、密钥、模型权重、真实心拍样本、厂商 SDK 和原历史记录。

欢迎通过 GitHub Issues 提交问题或 Pull Request 改进。报告检测问题时请附软件版本、采样条件、有效标志和去标识化的复现方法，不上传个人健康记录或凭据。
