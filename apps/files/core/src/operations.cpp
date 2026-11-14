#include "nrf/operations.hpp"

#include "nrf/paths.hpp"

#include <algorithm>
#include <cwctype>
#include <set>

namespace nrf {

namespace {
std::wstring lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)std::towlower(c);
    return s;
}
}  // namespace

std::wstring uniqueName(const std::wstring& wanted, const std::vector<std::wstring>& existing, bool isFolder) {
    std::set<std::wstring> taken;
    for (auto& e : existing) taken.insert(lower(e));
    if (!taken.count(lower(wanted))) return wanted;
    std::wstring stem = wanted, ext;
    if (!isFolder) {
        const std::wstring e = extensionOf(wanted);
        if (!e.empty() && e.size() < wanted.size()) {
            stem = wanted.substr(0, wanted.size() - e.size());
            ext = wanted.substr(wanted.size() - e.size());
        }
    }
    for (int n = 2; n < 100000; ++n) {
        const std::wstring candidate = stem + L" (" + std::to_wstring(n) + L")" + ext;
        if (!taken.count(lower(candidate))) return candidate;
    }
    return wanted;
}

std::wstring copyName(const std::wstring& name, const std::vector<std::wstring>& existing, bool isFolder) {
    std::wstring stem = name, ext;
    if (!isFolder) {
        const std::wstring e = extensionOf(name);
        if (!e.empty() && e.size() < name.size()) { stem = name.substr(0, name.size() - e.size()); ext = e; }
    }
    std::set<std::wstring> taken;
    for (auto& x : existing) taken.insert(lower(x));
    const std::wstring first = stem + L" - Copy" + ext;
    if (!taken.count(lower(first))) return first;
    for (int n = 2; n < 100000; ++n) {
        const std::wstring c = stem + L" - Copy (" + std::to_wstring(n) + L")" + ext;
        if (!taken.count(lower(c))) return c;
    }
    return first;
}

std::wstring renameProblem(const std::wstring& oldName, const std::wstring& newName, const std::vector<std::wstring>& existing) {
    if (newName.empty()) return L"A name can't be empty.";
    if (newName == L"." || newName == L"..") return L"That name is kept for Windows' own use.";
    for (wchar_t c : newName) {
        if (c < 32) return L"Names can't contain control characters.";
        if (std::wstring(L"\\/:*?\"<>|").find(c) != std::wstring::npos)
            return std::wstring(L"Names can't contain ") + c + L" (or any of \\ / : * ? \" < > |).";
    }
    if (newName.back() == L' ' || newName.back() == L'.') return L"Names can't end with a space or a full stop.";
    {
        // Device names Windows keeps for itself, with or without an extension.
        std::wstring base = lower(newName);
        if (const size_t dot = base.find(L'.'); dot != std::wstring::npos) base = base.substr(0, dot);
        while (!base.empty() && base.back() == L' ') base.pop_back();
        static const std::set<std::wstring> reserved{L"con", L"prn", L"aux", L"nul", L"com1", L"com2", L"com3", L"com4", L"com5",
                                                     L"com6", L"com7", L"com8", L"com9", L"lpt1", L"lpt2", L"lpt3", L"lpt4",
                                                     L"lpt5", L"lpt6", L"lpt7", L"lpt8", L"lpt9"};
        if (reserved.count(base)) return L"\u201C" + newName + L"\u201D is a name Windows keeps for devices.";
    }
    if (newName.size() > 255) return L"That name is too long (255 characters at most).";
    if (lower(newName) == lower(oldName)) return L"";   // the same, or only its capitals change
    for (auto& e : existing)
        if (lower(e) == lower(newName)) return L"There's already something called \u201C" + e + L"\u201D here.";
    return L"";
}

bool isInside(const std::wstring& path, const std::wstring& folder) {
    if (folder.empty()) return false;
    const std::wstring p = lower(path), f = lower(folder);
    if (p == f) return true;
    std::wstring prefix = f;
    if (prefix.back() != L'\\') prefix += L'\\';
    return p.size() > prefix.size() && p.compare(0, prefix.size(), prefix) == 0;
}

TransferPlan planTransfer(const std::vector<std::wstring>& sources, const std::wstring& destination, bool move) {
    TransferPlan plan;
    int alreadyThere = 0;
    for (const auto& s : sources) {
        if (s.empty()) continue;
        if (isInside(destination, s)) {
            plan.problem = L"\u201C" + nameOf(s) + L"\u201D can't go inside itself.";
            continue;
        }
        if (move && lower(parentOf(s)) == lower(destination)) { ++alreadyThere; continue; }
        plan.sources.push_back(s);
    }
    plan.nothingToDo = plan.sources.empty() && alreadyThere > 0 && plan.problem.empty();
    return plan;
}

}  // namespace nrf
