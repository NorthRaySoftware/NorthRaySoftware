#include "nrf/listing.hpp"

#include <algorithm>
#include <cwctype>

namespace nrf {

bool isVisible(const Entry& e, const Visibility& v) {
    if (e.protectedSystem()) return v.showProtected;
    if (e.hidden()) return v.showHidden;
    return true;
}

int naturalCompare(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const wchar_t ca = a[i], cb = b[j];
        const bool da = ca >= L'0' && ca <= L'9', db = cb >= L'0' && cb <= L'9';
        if (da && db) {
            // Compare the numbers: skip leading zeros, then the longer run is larger.
            size_t ia = i, jb = j;
            while (ia < a.size() && a[ia] == L'0') ++ia;
            while (jb < b.size() && b[jb] == L'0') ++jb;
            size_t ea = ia, eb = jb;
            while (ea < a.size() && a[ea] >= L'0' && a[ea] <= L'9') ++ea;
            while (eb < b.size() && b[eb] >= L'0' && b[eb] <= L'9') ++eb;
            if (ea - ia != eb - jb) return ea - ia < eb - jb ? -1 : 1;
            for (size_t k = 0; k < ea - ia; ++k)
                if (a[ia + k] != b[jb + k]) return a[ia + k] < b[jb + k] ? -1 : 1;
            // Same value: fewer leading zeros first ("1" before "01").
            if (ia - i != jb - j) return ia - i < jb - j ? -1 : 1;
            i = ea;
            j = eb;
            continue;
        }
        const wchar_t la = (wchar_t)std::towlower(ca), lb = (wchar_t)std::towlower(cb);
        if (la != lb) return la < lb ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return a < b ? -1 : a > b ? 1 : 0;   // only case differs: a steady order
}

void sortEntries(std::vector<Entry>& entries, const Sort& sort) {
    std::stable_sort(entries.begin(), entries.end(), [&](const Entry& x, const Entry& y) {
        if (x.folder() != y.folder()) return x.folder();
        int c = 0;
        switch (sort.key) {
            case SortKey::Name: break;
            case SortKey::Modified: c = x.modified < y.modified ? -1 : x.modified > y.modified ? 1 : 0; break;
            case SortKey::Size: c = x.size < y.size ? -1 : x.size > y.size ? 1 : 0; break;
            case SortKey::Type: c = naturalCompare(x.type, y.type); break;
        }
        if (c == 0) c = naturalCompare(x.name, y.name);
        return sort.descending ? c > 0 : c < 0;
    });
}

namespace {

std::wstring withSeparators(std::uint64_t n) {
    std::wstring d = std::to_wstring(n), out;
    int k = 0;
    for (size_t i = d.size(); i-- > 0;) {
        out.insert(out.begin(), d[i]);
        if (++k % 3 == 0 && i > 0) out.insert(out.begin(), L',');
    }
    return out;
}

}  // namespace

std::wstring formatSize(std::uint64_t bytes) {
    if (bytes < 1024) return std::to_wstring(bytes) + (bytes == 1 ? L" byte" : L" bytes");
    const wchar_t* units[] = {L"KB", L"MB", L"GB", L"TB", L"PB", L"EB"};
    double v = (double)bytes / 1024.0;
    int u = 0;
    while (v >= 1000.0 && u < 5) { v /= 1024.0; ++u; }
    // Three significant figures, truncated like File Explorer ("1.99 KB", not "2.00 KB").
    std::wstring num;
    if (v < 10) {
        const auto h = (std::uint64_t)(v * 100);
        num = std::to_wstring(h / 100) + L"." + (h % 100 < 10 ? L"0" : L"") + std::to_wstring(h % 100);
    } else if (v < 100) {
        const auto t = (std::uint64_t)(v * 10);
        num = std::to_wstring(t / 10) + L"." + std::to_wstring(t % 10);
    } else {
        num = std::to_wstring((std::uint64_t)v);
    }
    return num + L" " + units[u];
}

std::wstring formatSizeKb(std::uint64_t bytes) {
    return withSeparators((bytes + 1023) / 1024) + L" KB";
}

// ---------------------------------------------------------------- selection

void Selection::reset(std::size_t count) {
    selected_.assign(count, false);
    focus_ = anchor_ = -1;
}

std::size_t Selection::count() const { return (std::size_t)std::count(selected_.begin(), selected_.end(), true); }

std::vector<std::size_t> Selection::indices() const {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < selected_.size(); ++i)
        if (selected_[i]) out.push_back(i);
    return out;
}

void Selection::click(std::size_t i, bool ctrl, bool shift) {
    if (i >= selected_.size()) return;
    if (shift && anchor_ >= 0) {
        if (!ctrl) std::fill(selected_.begin(), selected_.end(), false);
        const std::size_t a = std::min((std::size_t)anchor_, i), b = std::max((std::size_t)anchor_, i);
        for (std::size_t k = a; k <= b; ++k) selected_[k] = true;
        focus_ = (int)i;
        return;
    }
    if (ctrl) {
        selected_[i] = !selected_[i];
    } else {
        std::fill(selected_.begin(), selected_.end(), false);
        selected_[i] = true;
    }
    focus_ = anchor_ = (int)i;
}

void Selection::clickEmpty(bool ctrl) {
    if (!ctrl) std::fill(selected_.begin(), selected_.end(), false);
}

void Selection::selectAll() { std::fill(selected_.begin(), selected_.end(), true); }

void Selection::clear() { std::fill(selected_.begin(), selected_.end(), false); }

void Selection::moveTo(std::size_t i, bool ctrl, bool shift) {
    if (selected_.empty()) return;
    i = std::min(i, selected_.size() - 1);
    if (shift) {
        if (anchor_ < 0) anchor_ = focus_ >= 0 ? focus_ : (int)i;
        std::fill(selected_.begin(), selected_.end(), false);
        const std::size_t a = std::min((std::size_t)anchor_, i), b = std::max((std::size_t)anchor_, i);
        for (std::size_t k = a; k <= b; ++k) selected_[k] = true;
        focus_ = (int)i;
        return;
    }
    focus_ = (int)i;
    if (ctrl) return;
    std::fill(selected_.begin(), selected_.end(), false);
    selected_[i] = true;
    anchor_ = (int)i;
}

void Selection::toggleFocused() {
    if (focus_ < 0 || (std::size_t)focus_ >= selected_.size()) return;
    selected_[(std::size_t)focus_] = !selected_[(std::size_t)focus_];
    anchor_ = focus_;
}

void Selection::select(std::size_t i) {
    std::fill(selected_.begin(), selected_.end(), false);
    if (i < selected_.size()) {
        selected_[i] = true;
        focus_ = anchor_ = (int)i;
    }
}

}  // namespace nrf
