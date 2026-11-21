# `mhw_rng` 微内核与热重载插件框架（v2）

面向《怪物猎人：世界 冰原》Steam 15.23（Build 15539686）的全局随机数接管与条件分流框架。采用微内核（`mhw_rng.dll`）与热插拔业务插件（`nativePC\plugins\mhw_rng\*.dll`）分离的结构。

## 架构概览

1. **核心模块 (`nativePC\plugins\mhw_rng.dll`)**
   - 拦截游戏内全部 10 处伪随机数入口（`MtRandom`、`mt19937`、`std::_Random_device`、`CRT_rand`），使用 W^X 跳板页并支持多插件链式挂钩。
   - 维护 8,192 项 64 位复合条件槽表 `Slot[(CondID << 32) | CallSiteRVA]`，按调用点与条件隔离随机流，支持 `MasterDerived`、`CustomSeed`、`FixedValue`、`Sequence`、`Bypass` 五种模式及 `O(1)` 轮次重置（`Epoch`）。
   - 同时支持异步状态切换（Push 模式：`SetActiveCondition`）与抽签时同步回调（Pull 模式：`RegisterSlotHook`）。
   - 采用 `.hotload\` 影拷贝加载子插件，游戏运行期间源 DLL 文件不被系统锁定，可直接重新编译覆盖并通过 `$rng reload [插件名]` 热更新。
   - 所有核心与插件聊天指令统一收口于 `$rng <subcmd>`（兼容 `$rng_<subcmd>`）前缀下。

2. **官方业务插件（以 Git Submodule 挂载于 `plugins/`）**
   - **[`mhw-rng-size`](https://github.com/Miraboreasyu/mhw-rng-size)**（`plugins/size` $\rightarrow$ `rng_size.dll`）：挂载 `0x169BA15` 同步回调，通过读取栈帧 `Monster*` 支持冰原全 71 种大型怪物按 `MonsterID` 条件分流、开区间 `[0x00000001U, 0xFFFFFF00U]` 零权重非法行规避，以及读取实时 `cSizeTable` 反算目标体型百分比。
   - **[`mhw-rng-inspector`](https://github.com/Miraboreasyu/mhw-rng-inspector)**（`plugins/inspector` $\rightarrow$ `rng_inspector.dll`）：提供 1,697 处静态调用点与 793 个外层函数区间的兜底词典，以及 `top`、`find`、`names`、`list`、`info`、`dump`、`fix`、`seq`、`slotseed`、`bypass`、`clear` 命令。

## 克隆与构建

```bash
# 包含两个官方插件 submodule 一起克隆
git clone --recursive https://github.com/Miraboreasyu/mhw-rng.git
```

- 构建全部模块：执行 [`build.bat`](build.bat)，产出 `build\mhw_rng.dll`、`build\mhw_rng\rng_inspector.dll` 与 `build\mhw_rng\rng_size.dll`。
- 运行脱机回归测试：执行 [`run_tests.bat`](run_tests.bat)（需要提供解壳后的 15.23 `.text` 段文件 `text_1523.bin`，可通过命令行参数或环境变量 `MHW_TEXT_BIN` 指定）。

## 技术文档导航

- 核心框架与插件 SDK 规范：[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)（头文件定义：[include/mhw_rng_api.h](include/mhw_rng_api.h)）
- 官方插件 `rng_size` 技术文档：[plugins/size/README.md](plugins/size/README.md)（独立仓库：[Miraboreasyu/mhw-rng-size](https://github.com/Miraboreasyu/mhw-rng-size)）
- 官方插件 `rng_inspector` 技术文档：[plugins/inspector/README.md](plugins/inspector/README.md)（独立仓库：[Miraboreasyu/mhw-rng-inspector](https://github.com/Miraboreasyu/mhw-rng-inspector)）
