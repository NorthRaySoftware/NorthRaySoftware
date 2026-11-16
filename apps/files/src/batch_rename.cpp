// NorthRay Files: batch rename (the selection's right-click menu, Ctrl+Shift+F2). A card over the window with
// the rules (pattern, find and replace, counter, case, extensions) and every new name previewed with any
// problem; nrf::batch plans the names and the order. The renames are plain moves within the folder
// (MoveFileEx without replacing), one at a time; if one fails, those already done are put back. Ctrl+Z undoes
// the last batch while its folder is shown.
#include "files_window.hpp"

#include "nrf/batchrename.hpp"
#include "nrf/paths.hpp"

#include "cjui/gfx.hpp"

#include <algorithm>
#include <cmath>

namespace nrf {

using cjui::Align;
using cjui::Color;
using cjui::Font;
using cjui::Rect;

namespace {
std::wstring dayOf(std::uint64_t filetime) {
    if (!filetime) return L"";
    FILETIME ft{(DWORD)(filetime & 0xFFFFFFFF), (DWORD)(filetime >> 32)};
    SYSTEMTIME utc, local;
    if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return L"";
    wchar_t b[16];
    swprintf(b, 16, L"%04d-%02d-%02d", local.wYear, local.wMonth, local.wDay);
    return b;
}
std::wstring textOf(HWND h) {
    const int n = GetWindowTextLengthW(h);
    std::wstring t((size_t)n, L'\0');
    if (n) GetWindowTextW(h, t.data(), n + 1);
    return t;
}
}  // namespace

void FilesWindow::openBatchRename() {
    if (here_.thisPC || here_.bin) return;
    const auto rows = sel_.indices();
    if (rows.size() < 2) {
        flash(L"Select two or more things to rename together (F2 renames one).", 5);
        return;
    }
    // One folder at a time (search results can come from many).
    std::wstring folder;
    batch_.files.clear();
    for (std::size_t r : rows) {
        if (r >= shown_.size()) continue;
        const Entry& e = shown_[r];
        const std::wstring in = e.location.empty() ? here_.path : e.location;
        if (folder.empty()) folder = in;
        else if (_wcsicmp(folder.c_str(), in.c_str()) != 0) {
            flash(L"Batch rename works on one folder at a time: these are in different folders.", 6);
            return;
        }
        batch_.files.push_back({e.name, dayOf(e.modified), e.folder()});
    }
    if (batch_.files.size() < 2 || folder.empty()) return;
    if (renaming_) endRename(false);
    batch_.folder = folder;
    batch_.open = true;
    batch_.rules = {};
    batch_.rules.pattern = L"{name}";
    const wchar_t* initial[] = {L"{name}", L"", L"", L"1", L"0"};
    for (int i = 0; i < 5; ++i) SetWindowTextW(batch_.fields[i], initial[i]);
    batch_.focus = 0;
    invalidate();
    UpdateWindow(hwnd());
    for (auto f : batch_.fields) ShowWindow(f, SW_SHOW);
    SetFocus(batch_.fields[0]);
    SendMessageW(batch_.fields[0], EM_SETSEL, 0, -1);
}

void FilesWindow::closeBatchRename() {
    if (!batch_.open) return;
    batch_.open = false;
    for (auto f : batch_.fields) ShowWindow(f, SW_HIDE);
    SetFocus(hwnd());
    invalidate();
}

// The rules from the fields and buttons, and the plan from them.
std::vector<batch::Change> FilesWindow::batchPlan() {
    batch::Rules& r = batch_.rules;
    r.pattern = textOf(batch_.fields[0]);
    r.find = textOf(batch_.fields[1]);
    r.replace = textOf(batch_.fields[2]);
    r.start = _wtoi(textOf(batch_.fields[3]).c_str());
    r.digits = std::clamp(_wtoi(textOf(batch_.fields[4]).c_str()), 0, 9);
    // The folder's other names: what's listed there that isn't in the batch.
    std::vector<std::wstring> others;
    for (const Entry& e : listed_) {
        const std::wstring in = e.location.empty() ? here_.path : e.location;
        if (_wcsicmp(in.c_str(), batch_.folder.c_str()) != 0) continue;
        const bool inBatch = std::any_of(batch_.files.begin(), batch_.files.end(), [&](const batch::File& f) { return _wcsicmp(f.name.c_str(), e.name.c_str()) == 0; });
        if (!inBatch) others.push_back(e.name);
    }
    batchOthers_ = others;
    return batch::plan(batch_.files, r, nameOf(batch_.folder), others);
}

void FilesWindow::doBatchRename() {
    const auto changes = batchPlan();
    if (!batch::canDo(changes)) return;
    std::vector<std::wstring> names = batchOthers_;
    for (auto& f : batch_.files) names.push_back(f.name);
    const auto steps = batch::order(changes, names);
    std::vector<batch::Step> done;
    std::wstring failure;
    for (auto& s : steps) {
        if (!MoveFileExW(extendedPath(join(batch_.folder, s.from)).c_str(), extendedPath(join(batch_.folder, s.to)).c_str(), 0)) {
            const DWORD err = GetLastError();
            wchar_t* msg = nullptr;
            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0, (LPWSTR)&msg, 0, nullptr);
            failure = L"“" + s.from + L"” couldn't be renamed" + (msg ? L": " + std::wstring(msg) : L".");
            if (msg) LocalFree(msg);
            while (!failure.empty() && (failure.back() == L'\n' || failure.back() == L'\r')) failure.pop_back();
            break;
        }
        done.push_back(s);
    }
    if (!failure.empty()) {
        // Put back what was done, last first.
        bool allBack = true;
        for (auto it = done.rbegin(); it != done.rend(); ++it)
            allBack &= MoveFileExW(extendedPath(join(batch_.folder, it->to)).c_str(), extendedPath(join(batch_.folder, it->from)).c_str(), 0) != 0;
        closeBatchRename();
        flash(failure + (allBack ? L" Nothing was changed." : L" Some names couldn't be put back: check the folder."), 10);
        reloadQuietly();
        return;
    }
    // Labels follow their files; the undo is the steps backwards.
    for (auto& c : changes)
        if (!c.same) labels_.moved(join(batch_.folder, c.from), join(batch_.folder, c.to));
    saveLabels();
    undoBatch_.clear();
    for (auto it = done.rbegin(); it != done.rend(); ++it) undoBatch_.push_back({it->to, it->from});
    undoBatchFolder_ = batch_.folder;
    int n = 0;
    std::vector<std::wstring> select;
    for (auto& c : changes) {
        n += !c.same;
        select.push_back(c.to);
    }
    closeBatchRename();
    flash(L"Renamed " + std::to_wstring(n) + (n == 1 ? L" item" : L" items") + L". Ctrl+Z puts the old names back.", 7);
    reloadQuietly(select);
}

bool FilesWindow::undoBatchRename() {
    if (undoBatch_.empty() || here_.thisPC || here_.bin || _wcsicmp(here_.path.c_str(), undoBatchFolder_.c_str()) != 0) return false;
    std::size_t ok = 0;
    for (auto& s : undoBatch_) {
        if (!MoveFileExW(extendedPath(join(undoBatchFolder_, s.from)).c_str(), extendedPath(join(undoBatchFolder_, s.to)).c_str(), 0)) break;
        labels_.moved(join(undoBatchFolder_, s.from), join(undoBatchFolder_, s.to));
        ++ok;
    }
    saveLabels();
    const bool all = ok == undoBatch_.size();
    undoBatch_.clear();
    flash(all ? L"The old names are back." : L"Only some of the old names could be put back (something else changed the folder).", 7);
    reloadQuietly();
    return true;
}

void FilesWindow::drawBatchRename(float W, float H) {
    gfx_.fill({0, 0, W, H}, cjui::hexColor("#000000").withAlpha(pal_.dark ? 0.45f : 0.25f));
    const float cw = std::min(780.0f, W - 40), ch = std::min(620.0f, H - 60);
    const Rect card{std::floor((W - cw) / 2), std::floor(std::max(30.0f, (H - ch) / 2)), cw, ch};
    batchCard_ = card;
    softShadow(card, 12, 1);
    gfx_.fillRounded(card, 12, pal_.surface);
    gfx_.strokeRounded(card, 12, pal_.rule);
    const Color danger = cjui::hexColor(pal_.dark ? "#F08A80" : "#C0392B");
    const float x = card.x + 22, w = card.w - 44;
    float y = card.y + 18;
    gfx_.text(L"Rename " + std::to_wstring(batch_.files.size()) + L" items", {x, y, w, 26}, Font::Heading, pal_.ink, {Align::Left, false, true, true});
    y += 34;
    gfx_.text(L"{name} the name now   {n} a number (start and digits below)   {date} the day it was changed   {folder} this folder's name",
              {x, y, w, 36}, Font::Small, pal_.inkMuted);
    y += 34;
    const float s = gfx_.dpiScale(), th = 20;
    auto field = [&](int i, const wchar_t* label, float fx, float fy, float fw) {
        gfx_.text(label, {fx, fy, fw, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
        const Rect f{fx, fy + 20, fw, 34};
        gfx_.fillRounded(f, 8, pal_.sunken);
        gfx_.strokeRounded(f, 8, GetFocus() == batch_.fields[i] ? pal_.focus : pal_.rule, GetFocus() == batch_.fields[i] ? 1.5f : 1.0f);
        SetWindowPos(batch_.fields[i], HWND_TOP, (int)std::lround((f.x + 10) * s), (int)std::lround((f.y + (f.h - th) / 2) * s),
                     (int)std::lround((f.w - 20) * s), (int)std::lround(th * s), SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SendMessageW(batch_.fields[i], WM_SETFONT, reinterpret_cast<WPARAM>(addressFont_), FALSE);
    };
    field(0, L"New names", x, y, w);
    y += 62;
    const float half = (w - 16) / 2;
    field(1, L"Find (in the new names)", x, y, half);
    field(2, L"Replace with", x + half + 16, y, half);
    y += 62;
    field(3, L"Numbers start at", x, y, 130);
    field(4, L"Digits (0 = as needed)", x + 146, y, 160);
    // Case and the other choices.
    float bx = x + 330;
    gfx_.text(L"Case", {bx, y, 200, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
    const wchar_t* cases[] = {L"As it is", L"lower", L"UPPER", L"Title"};
    for (int i = 0; i < 4; ++i) {
        const float bw = 82;
        const bool on = (int)batch_.rules.caseChange == i;
        button("br:case:" + std::to_string(i), {bx, y + 20, bw, 34}, cases[i], [this, i] {
            batch_.rules.caseChange = (batch::Case)i;
            invalidate();
        }, on);
        bx += bw + 6;
    }
    y += 62;
    button("br:ext", {x, y, 230, 32}, batch_.rules.changeExtension ? L"✓ Change extensions too" : L"Change extensions too", [this] {
        batch_.rules.changeExtension = !batch_.rules.changeExtension;
        invalidate();
    }, batch_.rules.changeExtension);
    button("br:matchcase", {x + 240, y, 170, 32}, batch_.rules.matchCase ? L"✓ Find matches case" : L"Find matches case", [this] {
        batch_.rules.matchCase = !batch_.rules.matchCase;
        invalidate();
    }, batch_.rules.matchCase);
    y += 46;
    // The preview.
    const auto changes = batchPlan();
    int problems = 0, changing = 0;
    for (auto& c : changes) problems += !c.problem.empty(), changing += !c.same && c.problem.empty();
    const float listBottom = card.bottom() - 70;
    const float rowH = 26;
    gfx_.text(L"NOW", {x, y, w / 2, 18}, Font::Small, pal_.inkFaint, {Align::Left, false, true});
    gfx_.text(L"WILL BE", {x + w / 2, y, w / 2, 18}, Font::Small, pal_.inkFaint, {Align::Left, false, true});
    y += 22;
    const int fit = std::max(1, (int)((listBottom - y) / rowH));
    // Problems first, so they can be seen.
    std::vector<const batch::Change*> rows;
    for (auto& c : changes)
        if (!c.problem.empty()) rows.push_back(&c);
    for (auto& c : changes)
        if (c.problem.empty()) rows.push_back(&c);
    for (int i = 0; i < (int)rows.size() && i < fit; ++i) {
        const batch::Change& c = *rows[(size_t)i];
        const Rect r{x, y, w, rowH};
        if (i % 2) gfx_.fill(r, pal_.ink.withAlpha(0.03f));
        gfx_.text(c.from, {x + 6, y, w / 2 - 12, rowH}, Font::Body, pal_.inkMuted, {Align::Left, false, true, true});
        if (!c.problem.empty())
            gfx_.text(c.to + L"  — " + c.problem, {x + w / 2, y, w / 2, rowH}, Font::Body, danger, {Align::Left, false, true, true});
        else
            gfx_.text(c.same ? L"(unchanged)" : c.to, {x + w / 2, y, w / 2, rowH}, Font::Body, c.same ? pal_.inkFaint : pal_.ink, {Align::Left, false, true, true});
        y += rowH;
    }
    if ((int)rows.size() > fit)
        gfx_.text(L"… and " + std::to_wstring(rows.size() - (size_t)fit) + L" more", {x + 6, y, w, rowH}, Font::Small, pal_.inkFaint, {Align::Left, false, true, true});
    // Buttons.
    const float by = card.bottom() - 54;
    std::wstring note = problems ? std::to_wstring(problems) + (problems == 1 ? L" name can't be used" : L" names can't be used")
                                 : changing ? L"" : L"Nothing changes yet.";
    gfx_.text(note, {x, by, w - 320, 36}, Font::Body, problems ? danger : pal_.inkMuted, {Align::Left, false, true, true});
    const bool ok = batch::canDo(changes);
    const Rect go{card.right() - 22 - 170, by, 170, 36};
    const std::wstring goLabel = L"Rename " + std::to_wstring(changing) + (changing == 1 ? L" item" : L" items");
    if (ok) button("br:go", go, goLabel, [this] { doBatchRename(); }, true);
    else disabledButton(go, goLabel);
    button("br:cancel", {card.right() - 22 - 170 - 10 - 96, by, 96, 36}, L"Cancel", [this] { closeBatchRename(); });
}

}  // namespace nrf
