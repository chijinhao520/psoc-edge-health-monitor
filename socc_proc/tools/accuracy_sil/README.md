# 准确性与设备流程离线回归

在工程根目录执行，生成物统一写入 `cache/accuracy_audit/`。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File socc_proc/tools/accuracy_sil/run.ps1
C:/Python314/python.exe socc_proc/tools/accuracy_sil/device_system_sil.py
C:/Python314/python.exe socc_proc/tools/accuracy_sil/cross_layer_sil.py
C:/Python314/python.exe -m unittest discover -s dashboard/tests -v
```

- `accuracy_sil.c`：直接编译PPG、SQI、GSR、会话状态机、HRV和ECG前处理；2898项检查。覆盖SQI宽平台不重复计峰、大量程统计值、非法输入清旧值、非有限数门控和接触恢复后的提示清除。
- `driver_sil.c`：直接编译MAX30102、ADS1015、SHT30、MLX90614生产驱动，以模拟总线检查错误返回、FIFO、超时、配置复位和CRC；142项检查。
- `device_system_sil.py`：执行提取的生产PPG任务循环与M55数据层；模拟取消、重试、缺设备、掉电配置复位、确认超时及结果过期。另检查等待总线超时与tick回绕，禁止超期FIFO数据进入窗口。替换的硬件依赖在脚本中明示。
- `cross_layer_sil.py`：生产报文编译后验证JSON/容量/有效标志，检查PVC统计窗口及两核IPC布局。
- `gpu_cleanup_sil.py`：执行SDK的实际初始化/清理函数，覆盖6种部分初始化失败、重复清理及恢复；检查不释放静态对象、没有遗留分配。模拟内存地址，不验证真实GPU硬件。
- `checkup_sil.py`：引导式体检的轮次隔离、报告冻结、单项重试、跳过、取消及超时回绕。`device_system_sil.py`另验证真实UI命令及本次快照规则评分；`cross_layer_sil.py`验证心电重建窗口确认。
- `checkup_preview.py`：PC编译项目LVGL、生产引导卡及监测/报告页，使用现有字体软件渲染7种状态；检查字形覆盖、卡片/按钮边界和按键行为。图片在`cache/checkup-preview/`，数据为明确的测试输入，不验证板级GPU或触摸。
- `dashboard/tests`：生产定义与路由的离线测试，使用内存数据库；不启动MQTT、不读取`.env`，不调用外部AI。

固件回归及引导界面验证于2026-10-29通过；dashboard检查沿用2026-10-28结果。PC编译使用本机MSVC，布局检查使用ARM GCC；换机器请调整脚本工具路径。测试通过不代表实板时序、缓存一致性、触摸布局或真人精度已验证。

引导式体检的使用、报告边界与实板验收见 `Edgi_Talk_M55_LVGL/applications/ui/CHECKUP.md`（工程根目录下）。

完整本地说明：`docs/设备系统完善与功能建议.md`。该仓库原有策略忽略`docs/`，上述测试源码与README可随代码纳入版本管理。

双核应用固件统一构建、回归与打包入口见 `tools/firmware/README.md`。
