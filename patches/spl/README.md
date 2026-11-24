# SharpPluginLoader (`winmm.dll`) WndProc Re-Subclassing Fix

简体中文 | English

---

## 问题与原理 / Root Cause & Fix

当游戏中同时使用 **SharpPluginLoader (`winmm.dll`)** 与会在运行期子类化游戏窗口过程 (`GWLP_WNDPROC`) 的叠加层模组（例如 `mhw_rng.dll`、Better-IN-Q 等）时：

- 返回标题画面（或切换显示模式）触发 DXGI `ResizeBuffers` 后，SharpPluginLoader 会重新执行 `InitializeD3D12` / `InitializeD3D11`。
- 原版 `winmm.dll` 只检查当前链头是否等于自身的 `WndProc`；一旦被其他叠加层压在下方，就会再次调用 `SetWindowLongPtrW` 并覆写其保存的前驱指针 `m_origWndProc`，使窗口过程链陷入死环，导致游戏原版 `Global::WndProc`（以及命令行终端 `ConsoleWndProc`）被环路保护机制永久跳过，引发返回标题后游戏输入法、终端输入法与普通字母键盘输入同时失效。

本目录下的 `winmm.dll` 对两处重挂逻辑做了最小字节补丁，仅在 `m_origWndProc == nullptr` 时才首次接管窗口过程：

- **D3D12 (`InitializeD3D12`) @ `RVA 0x48E0B`**: 原地替换为 `cmp qword ptr [rbx+498h], 0; jne 0x48E46`，若已保存前驱则跳过 `SetWindowLongPtrW`。
- **D3D11 (`InitializeD3D11`) @ `RVA 0x49209 -> 0x92EE0`**: 跳转至 `.text` 尾部空隙桩执行 `cmp qword ptr [rdi+498h], 0; jne 0x92F0A`，同等保护 D3D11 路径。

---

## 校验与安装 / Checksums & Installation

- **Original `winmm.dll` SHA256**: `0B0780C757EBE2D2EA25B43EFC5E9063C1FF47174201E819585F413CD2DA4BFD`
- **Patched `winmm.dll` SHA256**: `64DAAE7CC00EC2ADA0113EDEE1B95A551383968212E4C2E85EFFFFB8402CED34`

使用时将本目录下的 `winmm.dll` 复制到游戏根目录（与 `MonsterHunterWorld.exe` 同级，覆盖原有 SharpPluginLoader 的 `winmm.dll` 即可）。若未使用 SharpPluginLoader，则无需放置此文件。
