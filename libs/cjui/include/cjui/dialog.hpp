// A message box drawn in the app's own look (see dialog.cpp). Same arguments and answers as MessageBoxW.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace cjui {

int messageBox(HWND owner, const std::wstring& text, const std::wstring& title, UINT flags = MB_OK);

// The same box with buttons named for what they do (cjui, suite 0.170): `choices` left to right, then Cancel. Returns the
// index of the choice (`defaultChoice` for Enter), or -1 for Cancel or Esc.
int choiceBox(HWND owner, const std::wstring& text, const std::wstring& title, const std::vector<std::wstring>& choices,
              int defaultChoice = 0, UINT icon = MB_ICONQUESTION);

// The same box with a line to type in (0.91): returns true with `value` changed on OK (or Enter), false on Cancel (or Esc).
bool askText(HWND owner, const std::wstring& title, const std::wstring& text, std::wstring& value, const std::wstring& okLabel = L"OK");

}  // namespace cjui
