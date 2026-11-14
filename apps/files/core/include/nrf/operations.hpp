// NorthRay Files: the rules for changing files, separate from doing it (the Windows layer does that with
// the Shell's own file operations). Portable, so it's tested on every platform.
#pragma once

#include <string>
#include <vector>

namespace nrf {

// A name for something new in a folder that doesn't clash with what's there, the way File Explorer
// numbers them: "New folder", "New folder (2)", ...; for files the number goes before the extension
// ("Notes (2).txt"). Names are compared without regard to case.
std::wstring uniqueName(const std::wstring& wanted, const std::vector<std::wstring>& existing, bool isFolder);

// The name File Explorer gives a copy made in the same folder: "cover - Copy.png", then
// "cover - Copy (2).png", ...; folders "Sketches - Copy".
std::wstring copyName(const std::wstring& name, const std::vector<std::wstring>& existing, bool isFolder);

// Why a new name can't be used, in plain words, or "" when it can. `existing` are the folder's other names
// (renaming "a" to "A" is allowed: only the case changes).
std::wstring renameProblem(const std::wstring& oldName, const std::wstring& newName, const std::vector<std::wstring>& existing);

// Copying or moving paths into a folder: which can go, and why the others can't.
struct TransferPlan {
    std::vector<std::wstring> sources;   // the ones to do
    std::wstring problem;                // why some (or all) can't; "" if none
    bool nothingToDo = false;            // moving items to the folder they're already in
};
TransferPlan planTransfer(const std::vector<std::wstring>& sources, const std::wstring& destination, bool move);

// Whether `path` is `folder` or inside it ("C:\A\B" is inside "C:\A"; "C:\AB" isn't). Case doesn't matter.
bool isInside(const std::wstring& path, const std::wstring& folder);

}  // namespace nrf
