# 构建与依赖恢复

## 公开源码范围

本仓库为当前应用与配置覆盖层，保留两个工程的 `applications/`、`board/`、RT-Thread 配置及 Studio 工程文件。它不包含原工程的 RT-Thread、`libraries/`、`libs/`、厂商工具、生成的 Debug 文件或已训练资源。原 BSP 层的显示/驱动修改也不作为本次已审查的公开代码分发，因此不能保证只安装一个上游 BSP 就复现原机行为。

## 固件恢复顺序

1. 从 RT-Thread Studio 官方资源及开发板提供方取得 **Edgi Talk / PSoC Edge E84 对应的 M33、M55 工程/BSP**。平台资料入口：[RT-Thread Studio](https://www.rt-thread.org/studio.html)、[Infineon ModusToolbox](https://www.infineon.com/design-resources/development-tools/sdk/modustoolbox-software)。原机目标名为 `TARGET_APP_KIT_PSE84_EVAL_EPC2`；当前没有可验证的、锁定版本的一键 SDK 下载入口。
2. 在合法取得的 BSP 工程中恢复 `rt-thread/`、`libraries/`、`libs/` 和所需包，再应用本仓库对应目录的应用、板级文件与配置。先备份原 BSP。不要假定任意最新版 SDK 与本代码兼容。
3. 补齐 TFLite Micro、CMSIS-NN、Ethos-U55 和 LVGL 9.2 依赖；恢复外部预编译库、链接脚本、板级显示/触摸/缓存配置。单有应用源码不足以链接。
4. 恢复下表的生成资源。训练脚本引用的公开数据集须自行取得并遵守对应条款，不随本仓库分发。
5. 参照 `socc_proc/packages/README-aht10-patch.md` 应用 AHT 修复；`python tools/firmware/sdk_patch.py apply` 仅接受已登记哈希的 GPU SDK 源文件，未知版本会停止。
6. 在 Studio 重新导入两个工程，检查工具链、include 路径和链接库，重新生成 `Debug` 构建文件。`.cproject` 保留了原环境配置供对照，其中绝对路径必须适配当前机器。
7. 使用 [配套构建入口](tools/firmware/README.md) 运行回归、完整编译与打包。工具不会烧录；两核必须使用同一次构建的产物。

原机 ARM GCC 为 13.3.Rel1，测试使用 Windows MSVC。部分测试脚本仍记录原机工具位置，换机须调整；PC LVGL 渲染与 GPU 测试需要外部 SDK。不要把本仓库描述为全平台一键编译。

## 生成资源

| 未分发文件 | 用途与恢复方式 |
| --- | --- |
| `Edgi_Talk_M55_LVGL/applications/arrhythmia_model.h` | 心律模型；按 `arrhythmia_training/scripts/` 训练、量化、Vela 导出，自行验证数据与模型许可 |
| `.../glucose_model.h` | 研究模型；按 `glucose_training/scripts/` 的流程生成，不代表可用血糖测量 |
| `.../arrhythmia_test_data.h` | 真实 MIT-BIH 心拍测试夹具；取得数据后运行 `gen_test_beats.py`，仅用于等价测试 |
| `.../voice_data.c` | 原文件是 Windows SAPI 音频输出，未确认独立分发条款；根据 `voice_data.h` 的枚举与 16 kHz/16-bit/mono 契约自行录制或生成有许可的 PCM 数组 |

上述资源被 `.gitignore` 排除。代码仍明确引用它们，缺少时编译会报错；没有静默替换成假模型、假心拍或无声资源。中文字体及其生成代码已随 OFL 许可分发。

## 网页

```sh
python -m pip install -r dashboard/requirements.txt
python -m unittest discover -s dashboard/tests -v
```

复制 `dashboard/.env.example` 到 `.env` 后配置自己的服务。固件 `MQTT_BROKER_HOST` 与网页 `MQTT_BROKER` 要指向同一受控服务；固件中的 `localhost` 不是你的电脑地址，必须按网络环境修改。AI 密钥为空时不使用外部 AI 服务。

网页包含健康数据查看及可触发服务请求的接口，目前没有完整鉴权，不应直接暴露到公网。部署脚本会安装 `static/` 图表资源，执行它属于服务器修改操作，本次开源未执行部署。
