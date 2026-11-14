// NorthRay Files: drag and drop with other programs (0.6), through OLE as File Explorer does it.
//  - Into Files: a DropTarget registered on the window takes files dragged from File Explorer, the
//    desktop, a browser's downloads and so on (CF_HDROP), and asks the window where they'd go.
//  - Out of Files: when a drag leaves the window, the selection is handed to Windows as a Shell data
//    object, so any program that takes files dropped from File Explorer takes them from Files.
// Needs OleInitialize on the window's thread.
#pragma once

#include <windows.h>
#include <ole2.h>

#include <functional>
#include <string>
#include <vector>

namespace nrf::dnd {

struct DropHandlers {
    // Called as files are dragged over (`paths` the first time only): the effect to show
    // (DROPEFFECT_COPY, _MOVE, _LINK or _NONE). `screen` is in screen pixels; `keys` are MK_ flags.
    std::function<DWORD(const std::vector<std::wstring>& paths, POINT screen, DWORD keys, DWORD allowed)> over;
    std::function<void()> leave;
    // The drop: do it, and return the effect used.
    std::function<DWORD(const std::vector<std::wstring>& paths, POINT screen, DWORD keys, DWORD allowed)> drop;
};

// Registers the window as a drop target (RevokeDragDrop on destroy). False if OLE refused.
bool registerTarget(HWND window, DropHandlers handlers);
void revokeTarget(HWND window);

// Starts a drag of these files out of the window; returns when it's dropped (or cancelled) with
// the effect the target chose (DROPEFFECT_NONE if none).
DWORD dragOut(const std::vector<std::wstring>& paths);

std::vector<std::wstring> filesIn(IDataObject* data);

}  // namespace nrf::dnd
