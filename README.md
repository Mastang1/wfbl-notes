# wolfBoot 学习笔记（wfbl-notes）

本仓库记录 wolfBoot（v2.10.0）的学习笔记与实验，基于 STM32F407 探索者板。

## 仓库构成

| 路径 | 说明 |
|---|---|
| `wolfBoot/` | wolfBoot 上游源码（git 子模块，固定到提交 `9bdc6cb3`） |
| `wolfboot-explorer-tutorial.md` | 教学总纲：用探索者 STM32F407 学会 wolfBoot |
| `lesson_1.md` | 第 1 课：wolfBoot 构建系统深度剖析（config / build / tools / demo） |
| `daily_notes.md` | 日常学习笔记 |
| `lab/` | 教学配套移植文件（探索者板配置、测试应用、移植脚本） |
| `.gitmodules` | 子模块配置 |

`lab/` 目录：

- `stm32f407-explorer.config` — 探索者板配置（源自官方 `stm32f407-discovery.config`）
- `app_stm32f4.c` — 简化版测试应用（LED PF9/PF10 + KEY0）
- `apply-to-wolfboot.sh` — 一键把 lab 文件应用到 wolfBoot 仓库

## 克隆与初始化子模块

```bash
# 1. 克隆本仓库
git clone git@github.com:Mastang1/wfbl-notes.git
cd wfbl-notes

# 2. 初始化并拉取 wolfBoot 子模块
git submodule update --init --recursive
```

## 子模块常用命令

```bash
# 查看子模块状态（- 未初始化，+ 版本不一致，空格 正常）
git submodule status

# 更新子模块到本仓库记录的提交
git submodule update

# 更新子模块到其上游最新（会移动 wolfBoot 的 HEAD）
git submodule update --remote

# 把子模块切换到新的提交并记录到本仓库：
cd wolfBoot
git checkout <commit-or-tag>
cd ..
git add wolfBoot
git commit -m "Update wolfBoot submodule to <commit-or-tag>"
```

## 说明

- 子模块 `wolfBoot` 固定到上游 `master` 的提交 `9bdc6cb3`（v2.10.0 之后的提交），保持与当前学习基线一致。
- 运行 `lab/apply-to-wolfboot.sh` 前，wolfBoot 需处于 `v2.10.0` tag（脚本内部会校验）。
