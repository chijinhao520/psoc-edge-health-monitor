# 开发环境与构建指南

## 工程组织

两个工程分别提供 `applications/`、`board/`、RT-Thread 配置及 Studio 工程文件。开发时在对应的 Edgi Talk / PSoC Edge E84 BSP 环境中集成这些源码与配置。RT-Thread、厂商 SDK、预编译库由开发者从原提供方获取；模型与语音资源按下文准备。

## 固件开发步骤

1. 从 RT-Thread Studio 官方资源及开发板提供方取得 **Edgi Talk / PSoC Edge E84 对应的 M33、M55 工程/BSP**。平台资料入口：[RT-Thread Studio](https://www.rt-thread.org/studio.html)、[Infineon ModusToolbox](https://www.infineon.com/design-resources/development-tools/sdk/modustoolbox-software)。开发目标名为 `TARGET_APP_KIT_PSE84_EVAL_EPC2`，BSP 版本以开发板提供方的配套工程为准。
2. 备份 BSP 工程，准备 `rt-thread/`、`libraries/`、`libs/` 和所需包，再集成本仓库对应目录的应用、板级文件与配置。显示驱动、触摸及缓存配置需与目标板配套。
3. 配置 TFLite Micro、CMSIS-NN、Ethos-U55 和 LVGL 9.2 依赖，以及对应的预编译库与链接脚本。
4. 按下表生成模型、测试与语音资源。训练脚本引用的公开数据集须自行取得并遵守对应条款，不随本仓库分发。
5. 参照 `socc_proc/packages/README-aht10-patch.md` 应用 AHT 修复；`python tools/firmware/sdk_patch.py apply` 仅接受已登记哈希的 GPU SDK 源文件，未知版本会停止。
6. 在 Studio 重新导入两个工程，检查工具链、include 路径和链接库，重新生成 `Debug` 构建文件。`.cproject` 保留了原环境配置供对照，其中绝对路径必须适配当前机器。
7. 使用 [配套构建入口](tools/firmware/README.md) 运行回归、完整编译与打包。工具不会烧录；两核必须使用同一次构建的产物。

开发环境使用 ARM GCC 13.3.Rel1，离线 C 测试使用 Windows MSVC。按本机安装位置配置工具路径；PC LVGL 渲染与 GPU 测试使用配套 SDK。

## 生成资源

| 生成文件 | 用途与准备方式 |
| --- | --- |
| `Edgi_Talk_M55_LVGL/applications/arrhythmia_model.h` | 心律模型；按 `arrhythmia_training/scripts/` 训练、量化、Vela 导出，自行验证数据与模型许可 |
| `.../glucose_model.h` | 研究模型；按 `glucose_training/scripts/` 的流程生成，不代表可用血糖测量 |
| `.../arrhythmia_test_data.h` | 真实 MIT-BIH 心拍测试夹具；取得数据后运行 `gen_test_beats.py`，仅用于等价测试 |
| `.../voice_data.c` | 根据 `voice_data.h` 的枚举与 16 kHz/16-bit/mono 契约，录制或生成有许可的 PCM 数组 |

模型、测试心拍和语音资源按上述流程生成，并由 `.gitignore` 排除。中文字体及其生成代码随 OFL 许可分发。

## 网页

```sh
python -m pip install -r dashboard/requirements.txt
python -m unittest discover -s dashboard/tests -v
```

复制 `dashboard/.env.example` 到 `.env` 后配置自己的服务。固件 `MQTT_BROKER_HOST` 与网页 `MQTT_BROKER` 要指向同一受控服务；固件中的 `localhost` 不是你的电脑地址，必须按网络环境修改。AI 密钥为空时不使用外部 AI 服务。

网页包含健康数据查看及可触发服务请求的接口，目前没有完整鉴权，不应直接暴露到公网。部署脚本会安装 `static/` 图表资源，执行它属于服务器修改操作，本次开源未执行部署。
