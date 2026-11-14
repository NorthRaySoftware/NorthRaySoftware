// A message box drawn in the app's own look (see dialog.cpp). Same arguments and answers as MessageBoxW.
#pragma once

#include <windows.h>

#include <string>

namespace cjui {

int messageBox(HWND owner, const std::wstring& text, const std::wstring& title, UINT flags = MB_OK);

}  // namespace cjui
