// Pasting (0.37): tidier text in the panel's boxes, and a whole bible page pasted in one go.
//
// Every box in the panel cleans what's pasted into it: line breaks become what the box shows (several lines pasted
// into a one-line box like a name are joined, not cut off at the first), and no-break and zero-width spaces from web
// pages and documents go. A whole page pasted onto an entry (a character sheet with Name, Summary, Details, sections,
// notes per book, family and relationships; see studio/entry_paste) is spread over the entry instead: onto the open
// entry from any of its boxes or with Ctrl+V on its page, or as a new entry with Ctrl+V on the bible's cards.
// Ctrl+Z (or "Undo" in the panel) puts the entry back as it was, links and all.
#include "studio_app.hpp"
#include "cjui/menu.hpp"
#include "studio/entry_paste.hpp"

#include <commctrl.h>

#include <algorithm>

namespace nrs {

using namespace studio;

std::vector<Id> booksInReadingOrder(const Library& lib, const Id& world, const Id& book);   // studio_world.cpp

static std::wstring w(const std::string& s) { return widen(s); }

static bool clipboardText(HWND owner, std::wstring& out) {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(owner)) return false;
    bool ok = false;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
        if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            out = p;
            GlobalUnlock(h);
            ok = true;
        }
    CloseClipboard();
    return ok;
}

// What's pasted into a box: no-break spaces plain, zero-width ones gone, and line breaks to suit the box.
std::wstring cleanPastedText(const std::wstring& in, bool multiLine, bool list) {
    std::wstring s;
    s.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const wchar_t c = in[i];
        if (c == 0x200B || c == 0x200C || c == 0x200D || c == 0xFEFF || c == 0x00AD) continue;   // zero-width, soft hyphen
        if (c == 0x00A0 || c == 0x202F || c == 0x2007) { s += L' '; continue; }
        if (c == L'\r') { if (i + 1 < in.size() && in[i + 1] == L'\n') continue; s += L'\n'; continue; }
        if (c == 0x2028 || c == 0x2029 || c == 0x0B || c == 0x0C) { s += L'\n'; continue; }
        s += c;
    }
    // Trailing spaces off each line; no blank lines at either end.
    std::vector<std::wstring> lines;
    {
        std::wstring cur;
        for (wchar_t c : s + L"\n") {
            if (c == L'\n') {
                while (!cur.empty() && (cur.back() == L' ' || cur.back() == L'\t')) cur.pop_back();
                lines.push_back(cur);
                cur.clear();
            } else cur += c;
        }
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    while (!lines.empty() && lines.front().empty()) lines.erase(lines.begin());
    std::wstring out;
    if (multiLine) {
        for (size_t i = 0; i < lines.size(); ++i) out += (i ? L"\r\n" : L"") + lines[i];
        return out;
    }
    // One line: the lines joined ("Tom\nTommy" into other names: "Tom, Tommy").
    for (auto& l : lines) {
        std::wstring t = l;
        size_t a = 0;
        while (a < t.size() && (t[a] == L' ' || t[a] == L'\t')) ++a;
        t = t.substr(a);
        if (list) {
            for (const wchar_t* b : {L"- ", L"* ", L"• "}) if (t.rfind(b, 0) == 0) { t = t.substr(wcslen(b)); break; }
        }
        if (t.empty()) continue;
        if (!out.empty()) out += list && out.back() != L',' ? L", " : L" ";
        out += t;
    }
    for (auto& c : out) if (c == L'\t') c = L' ';
    return out;
}

static LRESULT CALLBACK fieldPaste(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    if (msg == WM_PASTE) {
        auto* app = reinterpret_cast<StudioApp*>(ref);
        std::wstring text;
        if (clipboardText(GetParent(h), text)) {
            if (app->pastedIntoField(h, text)) return 0;
            const bool multi = (GetWindowLongW(h, GWL_STYLE) & ES_MULTILINE) != 0;
            const bool list = app->isListField(h);
            std::wstring clean = cleanPastedText(text, multi, list);
            if (list && !clean.empty()) {
                // After a name already there: a comma between them.
                DWORD start = 0, end = 0;
                SendMessageW(h, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
                if (start > 0) {
                    std::wstring before(GetWindowTextLengthW(h) + 1, L'\0');
                    GetWindowTextW(h, before.data(), (int)before.size());
                    size_t i = std::min<size_t>(start, wcslen(before.c_str()));
                    while (i > 0 && before[i - 1] == L' ') --i;
                    if (i > 0 && before[i - 1] != L',') clean = L", " + clean;
                    else if (i > 0 && start == i) clean = L" " + clean;
                }
            }
            SendMessageW(h, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(clean.c_str()));
            return 0;
        }
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, fieldPaste, id);
    return DefSubclassProc(h, msg, wp, lp);
}

void StudioApp::watchFieldPastes() {
    for (int f = 0; f < FieldCount; ++f) SetWindowSubclass(fields_[f], fieldPaste, 2, reinterpret_cast<DWORD_PTR>(this));
}

bool StudioApp::isListField(HWND h) const { return h == fields_[FTags]; }

bool StudioApp::pastedIntoField(HWND h, const std::wstring& text) {
    if (!lib_ || ws_ != Workspace::Worldbuilding || wb_.entry.empty() || h == fields_[FTemplate] || h == fields_[FSearch]) return false;
    const std::string t = narrow(text);
    if (!paste::looksLikePage(t)) return false;
    pasteEntryPage(wb_.entry, t, false);
    return true;
}

bool StudioApp::pasteFromClipboard() {
    if (!lib_ || ws_ != Workspace::Worldbuilding || !wb_.board.empty() || wb_.languages) return false;
    std::wstring text;
    if (!clipboardText(hwnd(), text)) return false;
    const std::string t = narrow(text);
    if (!paste::looksLikePage(t)) {
        flash(L"To fill a page from what you've copied, it needs parts like Name:, Summary and Details on lines of their own.", 7);
        return true;
    }
    if (!wb_.entry.empty()) { pasteEntryPage(wb_.entry, t, false); return true; }
    // On the cards: a new entry, of the kind chosen (the kind shown, if one is).
    std::string kind = wb_.kind;
    if (kind.empty()) {
        std::vector<std::wstring> items;
        for (auto& k : entryKinds()) items.push_back(L"New " + w(entryKindLabel(k)) + L" from what you copied");
        HMENU m = CreatePopupMenu();
        for (size_t i = 0; i < items.size(); ++i) AppendMenuW(m, MF_STRING, 1 + (UINT)i, items[i].c_str());
        POINT pt;
        GetCursorPos(&pt);
        const int cmd = (int)cjui::trackMenu(m, pt, hwnd());
        DestroyMenu(m);
        if (cmd < 1) return true;
        kind = entryKinds()[(size_t)cmd - 1];
    }
    std::string label = entryKindLabel(kind);
    for (auto& c : label) c = (char)std::tolower((unsigned char)c);
    const Id id = lib_->createEntry(wb_.world, wb_.book, kind, "New " + label);
    if (id.empty()) { flash(w("Couldn't create the entry: " + lib_->lastError()), 8); return true; }
    openEntry(id);
    pasteEntryPage(id, t, true);
    return true;
}

void StudioApp::pasteEntryPage(const Id& entryId, const std::string& text, bool created) {
    commitFields();
    const Entry* e = lib_->entry(entryId);
    if (!e) return;
    PasteUndo undo;
    undo.entry = entryId;
    undo.before = *e;
    undo.created = created;
    const auto linksBefore = lib_->linksOf(entryId);
    const paste::Page page = paste::parse(text);
    paste::Report report;
    std::vector<Id> order = booksInReadingOrder(*lib_, wb_.world, wb_.book);
    if (!paste::apply(*lib_, entryId, page, order, report)) { flash(w("Couldn't fill the page: " + lib_->lastError()), 8); return; }
    for (auto& l : lib_->linksOf(entryId))
        if (std::none_of(linksBefore.begin(), linksBefore.end(), [&](const EntryLink& x) { return x.from == l.from && x.to == l.to && x.label == l.label; }))
            undo.added.push_back(l);
    const std::string said = report.words();
    if (said.empty()) { flash(L"Everything you pasted is already on this page.", 5); return; }
    wb_.pasteUndo = undo;
    std::wstring msg = L"Filled in " + w(said) + L".";
    if (!report.unmatchedBooks.empty())
        msg += L" Notes for " + w(report.unmatchedBooks[0]) + (report.unmatchedBooks.size() > 1 ? L" and others" : L"") +
               L" went to the notes (no book of that name).";
    msg += L" Ctrl+Z puts it back.";
    flash(msg, 10);
    wb_.mentionsFor.clear();
    loadedFocus_ = {Focus::None, "-", ""};
    SetFocus(hwnd());   // so Ctrl+Z comes here, not to a box
    invalidate();
}

bool StudioApp::undoEntryPaste() {
    if (!wb_.pasteUndo || !lib_) return false;
    const PasteUndo u = *wb_.pasteUndo;
    wb_.pasteUndo.reset();
    if (!lib_->entry(u.entry)) return false;
    for (auto& l : u.added) lib_->removeLink(l.from, l.to, l.label);
    if (u.created) {
        lib_->trashEntry(u.entry);
        if (wb_.entry == u.entry) openEntry({});
        flash(L"Took away the entry made from what you pasted.", 5);
    } else {
        lib_->updateEntry(u.before);
        flash(L"The page is back as it was before the paste.", 5);
    }
    loadedFocus_ = {Focus::None, "-", ""};
    invalidate();
    return true;
}

}  // namespace nrs
