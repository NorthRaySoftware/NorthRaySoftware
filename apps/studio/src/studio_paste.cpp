// Pasting (0.37): tidier text in the panel's boxes, and a whole bible page pasted in one go.
//
// Every box in the panel cleans what's pasted into it: line breaks become what the box shows (several lines pasted
// into a one-line box like a name are joined, not cut off at the first), and no-break and zero-width spaces from web
// pages and documents go. A whole page pasted onto an entry (a character sheet with Name, Summary, Details, sections,
// notes per book, family and relationships; see studio/entry_paste) is spread over the entry instead: onto the open
// entry from any of its boxes or with Ctrl+V on its page, or as a new entry with Ctrl+V on the bible's cards.
// What's pasted replaces what was there, part by part. Ctrl+Z (or "Undo the paste" in the panel) puts the entry
// back as it was, links and all.
#include "studio_app.hpp"
#include "cjui/dialog.hpp"
#include "cjui/menu.hpp"
#include "studio/entry_paste.hpp"
#include "studio/import.hpp"

#include <shobjidl.h>

#include <commctrl.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>

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
    if (!lib_ || ws_ != Workspace::Worldbuilding || wb_.entry.empty() || h == fields_[FTemplate] || h == fields_[FSearch] || h == fields_[FFind]) return false;
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
    // On the cards, several pages: each its own entry.
    if (paste::looksLikeManyPages(t)) { importPagesText(t, L"what you copied"); return true; }
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
    undo.made = report.created;
    for (auto& l : linksBefore)   // links of a kind the paste replaced
        if (lib_->entry(l.from) && lib_->entry(l.to)) {
            const auto now = lib_->linksOf(entryId);
            if (std::none_of(now.begin(), now.end(), [&](const EntryLink& x) { return x.from == l.from && x.to == l.to && x.label == l.label; }))
                undo.removed.push_back(l);
        }
    const std::string said = report.words();
    if (said.empty()) { flash(L"Everything you pasted is already on this page.", 5); return; }
    wb_.pasteUndo = undo;
    std::wstring msg = L"Replaced " + w(said) + L" with what you pasted.";
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
    for (auto& l : u.removed) lib_->addLink(l.from, l.to, l.label);
    for (const Id& m : u.made) lib_->trashEntry(m);   // the entries made for names in Family and Relationships
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

// A blank page for the open entry's kind (its world's sections, its books), to fill in elsewhere and paste back.
void StudioApp::copyBlankPage() {
    const Entry* e = lib_ ? lib_->entry(wb_.entry) : nullptr;
    if (!e) return;
    std::vector<std::string> sections;
    for (auto& [t, text] : lib_->entrySections(*e)) sections.push_back(t);
    copyBlankPageText(e->kind, sections);
}

// A blank page of a kind with the world's template sections, from the overview (0.43).
void StudioApp::copyBlankPageFor(const std::string& kind) {
    if (!lib_) return;
    std::vector<std::string> sections;
    for (const std::string& line : lib_->entryTemplate(wb_.world.empty() ? wb_.book : wb_.world, kind)) sections.push_back(sectionSpec(line).title);
    copyBlankPageText(kind, sections);
}

void StudioApp::copyBlankPageText(const std::string& kind, const std::vector<std::string>& sections) {
    std::vector<std::string> books;
    for (const Id& b : booksInReadingOrder(*lib_, wb_.world, wb_.book))
        if (const studio::Book* bk = lib_->book(b)) books.push_back(bk->title);
    std::string text = paste::blankPage(kind, sections, books);
    std::wstring wt;
    for (wchar_t ch : w(text)) { if (ch == L'\n') wt += L'\r'; wt += ch; }
    if (!OpenClipboard(hwnd())) { flash(L"The clipboard is busy; try again.", 4); return; }
    EmptyClipboard();
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (wt.size() + 1) * sizeof(wchar_t))) {
        if (void* p = GlobalLock(h)) {
            memcpy(p, wt.c_str(), (wt.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(h);
            if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
        }
    }
    CloseClipboard();
    flash(L"A blank " + w(entryKindLabel(kind)) + L" page is copied. Fill it in anywhere, copy it all, and paste it back here.", 8);
}

// ------------------------------------------------------------------ starting the bible again

void StudioApp::resetBible() {
    if (!lib_) return;
    commitFields();
    const auto entries = lib_->entriesOf(wb_.world, wb_.world.empty() ? wb_.book : Id{});
    std::wstring name;
    if (const World* wo = lib_->world(wb_.world)) name = w(wo->name);
    else if (const studio::Book* bk = lib_->book(wb_.book)) name = w(bk->title);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, (L"Move all " + std::to_wstring(entries.size()) + L" entries to the Trash").c_str());
    AppendMenuW(m, MF_STRING, 2, L"Start afresh: entries, languages, boards and templates");
    POINT pt;
    GetCursorPos(&pt);
    const int cmd = (int)cjui::trackMenu(m, pt, hwnd());
    DestroyMenu(m);
    if (cmd < 1) return;
    const bool everything = cmd == 2;
    const std::wstring q = L"Reset " + name + L"'s bible?\n\n" +
                           (everything ? L"All " + std::to_wstring(entries.size()) +
                                             L" entries go to the Trash, and its languages, image boards, page templates and Checks and sharing "
                                             L"choices are cleared."
                                       : L"All " + std::to_wstring(entries.size()) + L" entries go to the Trash. Its languages, image boards and templates stay.") +
                           L"\n\nA backup of the whole library is made first, and Ctrl+Z takes the reset back straight away. Your books' text "
                           L"isn't touched.";
    if (cjui::messageBox(hwnd(), q.c_str(), L"Reset the bible", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) return;
    std::string err;
    if (!lib_->backup(err, 20)) {
        cjui::messageBox(hwnd(), w("The backup couldn't be made, so nothing was reset.\n\n" + err).c_str(), L"Reset the bible", MB_OK | MB_ICONWARNING);
        return;
    }
    mirrorBackups();
    studio::Library::BibleReset rs;
    if (!lib_->resetBible(wb_.world, wb_.world.empty() ? wb_.book : Id{}, everything, rs)) {
        flash(w("Couldn't reset the bible: " + lib_->lastError()), 8);
        return;
    }
    wb_.resetUndo = rs;
    wb_.pasteUndo.reset();
    openEntry({});
    wb_.kind.clear();
    wb_.facet.clear();
    wb_.shareKey.clear();
    wb_.mentionsFor.clear();
    if (everything) sendView();   // the languages are gone from the spell checker too
    flash(L"The bible is empty: " + std::to_wstring(rs.trashed.size()) + L" entries are in the Trash. Ctrl+Z puts everything back.", 10);
    SetFocus(hwnd());
    invalidate();
}

bool StudioApp::undoBibleReset() {
    if (!wb_.resetUndo || !lib_) return false;
    const studio::Library::BibleReset rs = *wb_.resetUndo;
    wb_.resetUndo.reset();
    lib_->undoBibleReset(rs);
    wb_.shareKey.clear();
    sendView();
    flash(L"The bible is back: " + std::to_wstring(rs.trashed.size()) + L" entries restored.", 5);
    invalidate();
    return true;
}

// ------------------------------------------------------------------ many pages at once (0.39)

namespace {

// Text from a file's bytes: UTF-8 (a BOM taken off) or UTF-16 with a BOM.
std::string textFromBytes(const std::string& b) {
    if (b.size() >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB && (unsigned char)b[2] == 0xBF) return b.substr(3);
    if (b.size() >= 2 && ((unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xFE || (unsigned char)b[0] == 0xFE && (unsigned char)b[1] == 0xFF)) {
        const bool be = (unsigned char)b[0] == 0xFE;
        std::wstring ws;
        for (size_t i = 2; i + 1 < b.size(); i += 2)
            ws += (wchar_t)(be ? ((unsigned char)b[i] << 8 | (unsigned char)b[i + 1]) : ((unsigned char)b[i + 1] << 8 | (unsigned char)b[i]));
        return narrow(ws);
    }
    return b;
}

}  // namespace

void StudioApp::importManyPages(bool fromClipboard) {
    if (!lib_) return;
    commitFields();
    if (!fromClipboard) {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, 1, L"From a file (text, Markdown, Word or OpenDocument)…");
        AppendMenuW(m, MF_STRING, 2, L"From what you've copied");
        POINT pt;
        GetCursorPos(&pt);
        const int cmd = (int)cjui::trackMenu(m, pt, hwnd());
        DestroyMenu(m);
        if (cmd < 1) return;
        fromClipboard = cmd == 2;
    }
    if (fromClipboard) {
        std::wstring text;
        if (!clipboardText(hwnd(), text) || text.empty()) { flash(L"There's no text on the clipboard. Copy the pages first.", 6); return; }
        importPagesText(narrow(text), L"what you copied");
        return;
    }
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC types[] = {{L"Bible pages (text, Markdown, Word, OpenDocument)", L"*.txt;*.md;*.markdown;*.text;*.docx;*.odt"},
                                 {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, types);
    dlg->SetTitle(L"Import many bible pages");
    std::wstring path;
    if (SUCCEEDED(dlg->Show(hwnd()))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { path = p; CoTaskMemFree(p); }
            item->Release();
        }
    }
    dlg->Release();
    if (path.empty()) return;
    std::ifstream f(path.c_str(), std::ios::binary);
    std::string bytes;
    if (f) {
        f.seekg(0, std::ios::end);
        const auto size = (std::size_t)std::max<std::streamoff>(0, f.tellg());
        if (size > (64u << 20)) {
            cjui::messageBox(hwnd(), L"That file is too big to read as bible pages (over 64 MB).", L"Import many pages", MB_OK | MB_ICONWARNING);
            return;
        }
        f.seekg(0);
        bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    if (bytes.empty()) { cjui::messageBox(hwnd(), L"That file couldn't be read, or it's empty.", L"Import many pages", MB_OK | MB_ICONWARNING); return; }
    std::string lower = narrow(path);
    for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
    auto endsWith = [&](const char* e) { const size_t n = strlen(e); return lower.size() >= n && lower.compare(lower.size() - n, n, e) == 0; };
    std::string text;
    if (endsWith(".docx") || endsWith(".odt")) {
        // A document's headings become "# Heading" lines, so a heading for each kind or each page counts.
        const std::string name = narrow(path.substr(path.find_last_of(L"\\/") + 1));
        const studio::import::Plan plan = endsWith(".docx") ? studio::import::readDocx(bytes, name) : studio::import::readOdt(bytes, name);
        if (!plan.ok) {
            cjui::messageBox(hwnd(), w("That document couldn't be read.\n\n" + plan.error).c_str(), L"Import many pages", MB_OK | MB_ICONWARNING);
            return;
        }
        std::vector<std::pair<std::string, std::string>> secs;
        for (auto& sec : plan.sections) secs.emplace_back(sec.title, sec.content);
        text = paste::documentPagesText(secs);
    } else {
        text = textFromBytes(bytes);
    }
    importPagesText(text, w(narrow(path.substr(path.find_last_of(L"\\/") + 1))));
}

void StudioApp::importPagesText(const std::string& text, const std::wstring& from) {
    std::vector<paste::Batch> pages = paste::splitPages(text, "");
    if (pages.empty()) {
        cjui::messageBox(hwnd(),
                         (L"No pages were found in " + from +
                          L".\n\nEach page needs to start with a “Name:” line (or a title line such as “# Tom Smith”). A heading such as "
                          L"“# Characters” or a “Kind: Place” line says what kind the pages are. The manual has a template for every kind.")
                             .c_str(),
                         L"Import many pages", MB_OK | MB_ICONINFORMATION);
        return;
    }
    // Pages with no kind given: ask once for all of them.
    const size_t kindless = (size_t)std::count_if(pages.begin(), pages.end(), [](const paste::Batch& b) { return b.kind.empty(); });
    if (kindless > 0) {
        HMENU m = CreatePopupMenu();
        const auto& kinds = entryKinds();
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0,
                    (std::to_wstring(kindless) + (kindless == 1 ? L" page doesn't say its kind. It's a:" : L" pages don't say their kind. They're:")).c_str());
        for (size_t i = 0; i < kinds.size(); ++i) AppendMenuW(m, MF_STRING, 1 + (UINT)i, w(entryKindLabel(kinds[i])).c_str());
        POINT pt;
        GetCursorPos(&pt);
        const int cmd = (int)cjui::trackMenu(m, pt, hwnd());
        DestroyMenu(m);
        if (cmd < 1) return;
        for (auto& b : pages) if (b.kind.empty()) b.kind = kinds[(size_t)cmd - 1];
    }
    const Id scopeBook = wb_.world.empty() ? wb_.book : Id{};
    std::map<std::string, int> byKind;
    int existing = 0;
    for (auto& b : pages) {
        ++byKind[b.kind];
        if (lib_->findEntryNamed(wb_.world, scopeBook, b.name, b.kind) || lib_->findEntryNamed(wb_.world, scopeBook, b.name)) ++existing;
    }
    std::wstring kinds;
    for (const auto& k : entryKinds())
        if (auto it = byKind.find(k); it != byKind.end()) {
            std::string label = it->second == 1 ? entryKindLabel(k) : entryKindPlural(k);
            if (k == "lore" && it->second > 1) label = "lore entries";
            for (auto& c : label) c = (char)std::tolower((unsigned char)c);
            kinds += (kinds.empty() ? L"" : L", ") + std::to_wstring(it->second) + L" " + w(label);
        }
    std::wstring q = L"Bring " + std::to_wstring(pages.size()) + (pages.size() == 1 ? L" page" : L" pages") + L" from " + from +
                     L" into the bible?\n\n" + kinds + L".";
    if (existing > 0)
        q += L"\n\n" + std::to_wstring(existing) + (existing == 1 ? L" of them has the name of an entry already in the bible: its"
                                                                 : L" of them have the names of entries already in the bible: their") +
             L" parts on the page replace what's there, as a paste does.";
    q += L"\n\nA backup of the library is made first, and Ctrl+Z takes the import back.";
    if (cjui::messageBox(hwnd(), q.c_str(), L"Import many pages", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    std::string err;
    if (!lib_->backup(err, 20)) {
        cjui::messageBox(hwnd(), w("The backup couldn't be made, so nothing was imported.\n\n" + err).c_str(), L"Import many pages", MB_OK | MB_ICONWARNING);
        return;
    }
    mirrorBackups();
    paste::Import imp;
    const bool ok = paste::importPages(*lib_, wb_.world, wb_.world.empty() ? wb_.book : Id{}, pages, booksInReadingOrder(*lib_, wb_.world, wb_.book), imp);
    wb_.importUndo = imp;
    wb_.pasteUndo.reset();
    wb_.mentionsFor.clear();
    loadedFocus_ = {Focus::None, "-", ""};
    if (!ok) {
        flash(w("The import stopped part way: " + lib_->lastError() + ". Ctrl+Z takes back what was done."), 10);
    } else {
        const std::string said = imp.words();
        flash(said.empty() ? std::wstring(L"Every page was already in the bible as it is.")
                           : L"Imported: " + w(said) + L". Ctrl+Z takes it back.",
              10);
    }
    sendView();   // the names, for Writing's links and spell checker
    SetFocus(hwnd());
    invalidate();
}

bool StudioApp::undoPagesImport() {
    if (!wb_.importUndo || !lib_) return false;
    const paste::Import imp = *wb_.importUndo;
    wb_.importUndo.reset();
    // Entries that were there: as they were, with the links they had.
    for (size_t i = 0; i < imp.updated.size(); ++i) {
        const Id& id = imp.updated[i];
        if (!lib_->entry(id)) continue;
        lib_->updateEntry(imp.before[i]);
        const auto& old = imp.linksBefore[i];
        auto same = [](const EntryLink& a, const EntryLink& b) { return a.from == b.from && a.to == b.to && a.label == b.label; };
        for (auto& l : lib_->linksOf(id))
            if (std::none_of(old.begin(), old.end(), [&](const EntryLink& x) { return same(x, l); })) lib_->removeLink(l.from, l.to, l.label);
        const auto now = lib_->linksOf(id);
        for (auto& l : old)
            if (std::none_of(now.begin(), now.end(), [&](const EntryLink& x) { return same(x, l); })) lib_->addLink(l.from, l.to, l.label);
    }
    // Entries the import made: to the Trash.
    for (const Id& id : imp.extra) lib_->trashEntry(id);
    for (const Id& id : imp.made) lib_->trashEntry(id);
    if (std::find(imp.made.begin(), imp.made.end(), wb_.entry) != imp.made.end() ||
        std::find(imp.extra.begin(), imp.extra.end(), wb_.entry) != imp.extra.end())
        openEntry({});
    loadedFocus_ = {Focus::None, "-", ""};
    sendView();
    flash(L"The import is taken back: " + std::to_wstring(imp.made.size() + imp.extra.size()) + L" entries to the Trash, " +
              std::to_wstring(imp.updated.size()) + L" put back as they were.",
          6);
    invalidate();
    return true;
}

}  // namespace nrs
