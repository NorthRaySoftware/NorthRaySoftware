#include "nrf/batchrename.hpp"

#include "nrf/operations.hpp"

#include <algorithm>
#include <cwctype>
#include <map>
#include <set>

namespace nrf::batch {

namespace {
std::wstring lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)std::towlower(c);
    return s;
}
std::wstring upper(std::wstring s) {
    for (auto& c : s) c = (wchar_t)std::towupper(c);
    return s;
}
std::wstring title(const std::wstring& s) {
    std::wstring out = s;
    bool start = true;
    for (auto& c : out) {
        if (std::iswalpha(c)) {
            c = start ? (wchar_t)std::towupper(c) : (wchar_t)std::towlower(c);
            start = false;
        } else {
            start = !(c == L'\'' || c == L'’' || std::iswdigit(c));   // "don't", "3rd" stay together
        }
    }
    return out;
}
// The name and its extension ("cover.png" -> "cover", ".png"); a leading dot isn't an extension; folders have none.
void split(const std::wstring& n, bool folder, std::wstring& base, std::wstring& ext) {
    const auto dot = n.find_last_of(L'.');
    if (folder || dot == std::wstring::npos || dot == 0) {
        base = n;
        ext.clear();
    } else {
        base = n.substr(0, dot);
        ext = n.substr(dot);
    }
}
std::wstring number(int n, int digits) {
    std::wstring s = std::to_wstring(std::abs(n));
    while ((int)s.size() < digits) s.insert(s.begin(), L'0');
    return n < 0 ? L"-" + s : s;
}
std::wstring replaceAll(const std::wstring& s, const std::wstring& find, const std::wstring& with, bool matchCase) {
    if (find.empty()) return s;
    const std::wstring hay = matchCase ? s : lower(s), needle = matchCase ? find : lower(find);
    std::wstring out;
    std::size_t at = 0;
    for (;;) {
        const auto p = hay.find(needle, at);
        if (p == std::wstring::npos) break;
        out += s.substr(at, p - at) + with;
        at = p + needle.size();
    }
    return out + s.substr(at);
}
}  // namespace

std::vector<Change> plan(const std::vector<File>& files, const Rules& r, const std::wstring& folderName, const std::vector<std::wstring>& others) {
    std::vector<Change> out;
    const std::wstring pattern = r.pattern.empty() ? L"{name}" : r.pattern;
    const int digits = std::clamp(r.digits, 0, 9);
    int n = r.start;
    for (auto& f : files) {
        std::wstring base, ext;
        split(f.name, f.folder, base, ext);
        if (r.changeExtension) {   // the rules see the whole name
            base = f.name;
            ext.clear();
        }
        // The pattern, token by token.
        std::wstring name;
        for (std::size_t i = 0; i < pattern.size();) {
            if (pattern[i] == L'{') {
                const auto close = pattern.find(L'}', i);
                if (close != std::wstring::npos) {
                    const std::wstring tok = lower(pattern.substr(i + 1, close - i - 1));
                    bool known = true;
                    if (tok == L"name") name += base;
                    else if (tok == L"n") name += number(n, digits);
                    else if (tok.rfind(L"n:", 0) == 0 && tok.size() == 3 && std::iswdigit(tok[2])) name += number(n, tok[2] - L'0');
                    else if (tok == L"date") name += f.date;
                    else if (tok == L"folder") name += folderName;
                    else if (tok == L"ext") name += ext.empty() ? L"" : ext.substr(1);
                    else known = false;
                    if (known) {
                        i = close + 1;
                        continue;
                    }
                }
            }
            name += pattern[i++];
        }
        name = replaceAll(name, r.find, r.replace, r.matchCase);
        switch (r.caseChange) {
            case Case::Lower: name = lower(name); break;
            case Case::Upper: name = upper(name); break;
            case Case::Title: name = title(name); break;
            case Case::Keep: break;
        }
        Change c{f.name, name + ext, L"", false};
        c.same = c.to == c.from;
        out.push_back(c);
        n += r.step;
    }
    // Problems: each name's own (renameProblem, against the folder's other names), then names the batch gives twice.
    std::map<std::wstring, int> count;
    for (auto& c : out) ++count[lower(c.to)];
    std::set<std::wstring> renamed;
    for (auto& c : out) renamed.insert(lower(c.from));
    for (auto& c : out) {
        if (c.same) continue;
        c.problem = renameProblem(c.from, c.to, others);
        if (c.problem.empty() && count[lower(c.to)] > 1) c.problem = L"Another file in the batch gets this name too.";
    }
    // A file not being renamed that has a new name (it's in `others`, so renameProblem said so), and a batch file
    // keeping a name another wants, are both caught: an unchanged batch name counts as taken.
    for (auto& c : out) {
        if (c.same || !c.problem.empty()) continue;
        for (auto& d : out)
            if (&d != &c && d.same && lower(d.from) == lower(c.to)) c.problem = L"“" + d.from + L"” keeps that name.";
    }
    return out;
}

bool canDo(const std::vector<Change>& changes) {
    bool any = false;
    for (auto& c : changes) {
        if (!c.problem.empty()) return false;
        any = any || !c.same;
    }
    return any;
}

std::vector<Step> order(const std::vector<Change>& changes, const std::vector<std::wstring>& names) {
    // Pending renames by source (lower case). A rename can go when its target isn't a pending source.
    std::map<std::wstring, Change> pending;
    for (auto& c : changes)
        if (!c.same && c.problem.empty()) pending[lower(c.from)] = c;
    std::set<std::wstring> taken;
    for (auto& n : names) taken.insert(lower(n));
    for (auto& c : changes) taken.insert(lower(c.to));
    int tmpN = 0;
    auto tempName = [&] {
        std::wstring t;
        do t = L"~nr-rename-" + std::to_wstring(++tmpN) + L".tmp";
        while (taken.count(lower(t)));
        taken.insert(lower(t));
        return t;
    };
    std::vector<Step> steps;
    while (!pending.empty()) {
        bool progressed = false;
        for (auto it = pending.begin(); it != pending.end();) {
            const std::wstring target = lower(it->second.to);
            // Only its capitals change, or nothing else is still called what it wants: do it.
            if (target == it->first || !pending.count(target)) {
                steps.push_back({it->second.from, it->second.to});
                it = pending.erase(it);
                progressed = true;
            } else {
                ++it;
            }
        }
        if (progressed) continue;
        // Only cycles are left (a -> b -> a): move one aside, and finish it later.
        auto it = pending.begin();
        const std::wstring tmp = tempName();
        steps.push_back({it->second.from, tmp});
        Change rest = it->second;
        rest.from = tmp;
        pending.erase(it);
        pending[lower(tmp)] = rest;
    }
    return steps;
}

}  // namespace nrf::batch
