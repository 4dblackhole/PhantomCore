#pragma once
#include "Input/TextInput.h"
#include <Windows.h>
#include <memory>

namespace PhantomCore
{
std::unique_ptr<ITextInput> CreateWin32TextInput(HWND parent);
bool IsWin32TextInput(HWND window) noexcept;
bool NotifyWin32TextInputChanged(HWND window) noexcept;
HBRUSH PaintWin32TextInput(HWND window, HDC dc) noexcept;

} // namespace PhantomCore
