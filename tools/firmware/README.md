# 双核应用固件构建与交付

在工程根目录运行（Python 3.11+，Windows）：

```powershell
python tools/firmware/build_pair.py build
python tools/firmware/build_pair.py verify cache/firmware/<本次目录>
```

构建入口依次检查GPU SDK修复和两份IPC头、记录本机源码/SDK/静态库输入指纹、运行六组固件离线回归，
再强制重新编译M33与M55。只调用`rtthread.elf`目标，不执行IDE的后处理与烧录。
检查ELF可加载段在当前应用区内、没有重叠，并确认构建期间输入没有变化后，
才写`manifest.json`作为完成标记。失败目录保留日志，无完成标记的目录不能交付。

输出包含两核ELF、map、构建与测试日志、段地址、静态资源统计、IPC协议副本、
源输入SHA256清单及固件SHA256。没有单核打包、跳过回归或复用旧ELF入口。
允许工作区有未提交修改；输入清单描述实际文件，Git提交号仅作背景标识。
不同时间的包不保证逐字节可复现（编译器时间宏、绝对路径等可能不同）。

## 环境

先由RT-Thread Studio生成两个工程的Debug构建文件；现有生成文件包含工程绝对路径，
换目录后须重新生成。工具优先从PATH查找，再使用本机已知安装位置，
可用`--toolchain`、`--make`、`--project-linker`覆盖。离线C测试还依赖MSVC，
工具路径见`socc_proc/tools/accuracy_sil`及`hr_sil`的脚本。
`--output`只能指定新目录，`--jobs`默认4。生成物默认放`cache/firmware/`。

## 应用更新边界

这是**配套应用ELF包**，不包含安全启动镜像、签名或整机出厂恢复包。
沿用已验证的板级启动环境；两核应来自同一个包。更新前执行verify，
保留上一个已实板验收的完整双核包以便回退。

- M33的ELF加载地址使用C-BUS别名`0x08340000`应用区；现有板级烧录流程
  将其映射到S-BUS `0x60340000`（偏移`0x58000000`）。
- M55的ELF加载地址已经是`0x60580000`起的S-BUS地址，不能再加M33偏移。
- 工具只检查并记录地址，不改ELF，也不生成未经验证的新烧录命令。
- 仓库原有M33后处理会合并安全镜像；本工具不调用它，避免混淆应用更新与出厂烧录。

SHA256用于检测传输损坏、缺文件与新旧文件误混，不是对恶意篡改的签名认证。
`size.txt`中的BSS可能包含堆、显存等预留区；不能用它推断实际堆栈峰值。
上板仍需检查双核启动、测量取消/重试、掉线恢复、联网与离线流程，以及参考仪器精度。

## 工具回归

GPU SDK原版本在初始化失败时会释放静态Device对象，并遍历未初始化的堆链表。
本机已修复；换机恢复原SDK后先执行：

```powershell
python tools/firmware/sdk_patch.py apply
```

补丁只接受登记的原始SHA256，先备份到`cache/sdk-patches/`，重复执行不重复修改。
未知SDK版本拒绝覆盖，需先审查。构建前检查修复，并执行生产初始化/清理函数的
内存分配失败模拟；缺少修复不能打包。该修复只覆盖失败清理，不代表GPU全生命周期已验收。

```powershell
python -m unittest discover -s tools/firmware/tests -v
```
