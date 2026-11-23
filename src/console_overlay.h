#pragma once
#include <cstdint>

namespace console_overlay {

// Called from cD3DRender::present on the render thread. Fast atomic exit when console is closed.
void OnPresent(std::uintptr_t exeBase, std::uintptr_t d3dRender) noexcept;

// Mouse text-selection & clipboard copy (called from Win32 message thread when console is open).
void OnMouseDown(void* hwnd, int clientX, int clientY, bool isDoubleClick) noexcept;
void OnMouseMove(void* hwnd, int clientX, int clientY, bool leftButtonDown) noexcept;
void OnMouseUp(void* hwnd, int clientX, int clientY) noexcept;
bool HasSelection() noexcept;
void ClearSelection() noexcept;
bool CopySelectionToClipboard(bool clearAfterCopy) noexcept;

// Releases all GDI / D3D11 / D3D12 / shader resources cleanly.
void Shutdown() noexcept;

} // namespace console_overlay
