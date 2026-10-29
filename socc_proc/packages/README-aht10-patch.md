# AHT10/AHT20 包的本地修改说明

`aht10-latest/` 是 RT-Thread 软件包（git submodule），本项目对它做了 **AHT20 适配与
bug 修复**，但这些修改在 submodule 的工作区里处于**未提交**状态。

## 为什么要有这个 patch

git submodule 的父仓库**只记录 submodule 的 commit hash，不记录其工作区内容**。
`git status` 里看到的 `m socc_proc/packages/aht10-latest`（小写 m = dirty）意味着：
本地改了但没提交，**父仓库的任何 commit 都保不住这些改动**。一旦这个包被
`pkgs --update` 重新拉取、或误 `git checkout`，修复会静默消失，症状是 AHT20
读到上一次的旧数据（见 memory `2026-socc-aht20-pitfalls`）。

`aht10-aht20-local-fixes.patch` 就是这些修改的存档（2026-07-16 导出）。

## 修改内容要点

- 触发采样命令改为 `0xAC 0x33 0x00`（原包是 `0xAC 0x00 0x00`，AHT20 不认）
- 触发后必须等 >=75ms（用 80ms）再读数据，否则读到上一次的旧值
- 校准状态位判据 `(status & 0x68) == 0x08`（bit7=0 空闲 且 bit3=1 校准完成）
- 注释补全 + AHT20 地址/命令字说明

## 丢失后如何恢复

```bash
cd socc_proc/packages/aht10-latest
git apply ../aht10-aht20-local-fixes.patch
```

## 更彻底的方案（未做，留给后续决定）

当前是"外挂 patch"兜底。更稳的做法是把驱动从 submodule 里拿出来，
放进 `socc_proc/applications/sensor/`，像 max30102/mlx90614 一样由本仓库直接管理，
彻底摆脱 submodule 的内容不入库问题。竞赛提交前不动，避免破坏当前可演示状态。
