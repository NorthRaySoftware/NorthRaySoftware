// Worldbuilding › Languages (0.36): a world's (or an independent book's) constructed languages.
//
// Each language is a list of words with their meanings, kept in the library's settings (studio/conlang). Words of
// these languages aren't marked as misspellings in Writing, and a near miss of one is marked with the right word as the
// suggestion. Languages come in from files made elsewhere (CSV or TSV spreadsheets, SFM lexicons from Toolbox or Lexique
// Pro, plain word lists, NorthRay's .nrlang) and go out as .nrlang or CSV. The page lists the words; the panel edits the
// language or the word picked.
#include "studio_app.hpp"
#include "cjui/dialog.hpp"
#include "cjui/menu.hpp"

#include <shobjidl.h>

#include <algorithm>
#include <fstream>

namespace nrs {

using nlohmann::json;
using namespace studio;

static std::wstring w(const std::string& s) { return widen(s); }

static int popupAt(HWND owner, const std::vector<std::wstring>& items, int checked = -1) {
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < items.size(); ++i)
        AppendMenuW(m, MF_STRING | ((int)i == checked ? MF_CHECKED : 0), 1 + (UINT)i, items[i].c_str());
    POINT pt;
    GetCursorPos(&pt);
    const int cmd = (int)cjui::trackMenu(m, pt, owner);
    DestroyMenu(m);
    return cmd - 1;
}

static std::string lowerAscii(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string StudioApp::conlangKey(const Id& world, const Id& book) const {
    if (!world.empty()) return "conlangs:world:" + world;
    if (!book.empty()) return "conlangs:book:" + book;
    return {};
}

const std::vector<conlang::Language>& StudioApp::conlangs(const Id& world, const Id& book) const {
    static const std::vector<conlang::Language> none;
    const std::string key = conlangKey(world, book);
    if (key.empty() || !lib_) return none;
    // Read again only when what's stored has changed (a language can hold thousands of words; this is asked each frame).
    std::string raw = lib_->setting(key);
    auto& c = conlangCache_[key];
    if (c.first != raw) {
        c.second = conlang::fromJson(raw);
        c.first = std::move(raw);
    }
    return c.second;
}

void StudioApp::saveConlangs(std::vector<conlang::Language> langs) {
    const std::string key = conlangKey(wb_.world, wb_.world.empty() ? wb_.book : Id{});
    if (key.empty()) return;
    for (auto& l : langs)   // kept in alphabetical order, so the page and exports read like a dictionary
        std::stable_sort(l.words.begin(), l.words.end(), [](const conlang::Word& a, const conlang::Word& b) { return lowerAscii(a.word) < lowerAscii(b.word); });
    if (!lib_->setSetting(key, conlang::toJson(langs))) flash(w("Couldn't save the languages: " + lib_->lastError()), 8);
    sendView();   // the spell checker in Writing knows the words at once
    invalidate();
}

void StudioApp::openLanguages() {
    openEntry({});
    wb_.languages = true;
    wb_.scroll = 0;
    const auto& langs = conlangs(wb_.world, wb_.world.empty() ? wb_.book : Id{});
    if (std::none_of(langs.begin(), langs.end(), [&](const conlang::Language& l) { return l.id == wb_.language; }))
        wb_.language = langs.empty() ? std::string() : langs[0].id;
    setFocus(wb_.language.empty() ? Focus{} : Focus{Focus::Language, wb_.language, {}});
    invalidate();
}

void StudioApp::newLanguage() {
    commitFields();
    auto langs = conlangs(wb_.world, wb_.world.empty() ? wb_.book : Id{});
    if (langs.size() >= conlang::kMaxLanguages) {
        flash(L"There are already 50 languages here.", 5);
        return;
    }
    conlang::Language l;
    l.id = conlang::newId(langs);
    l.name = "New language";
    langs.push_back(l);
    saveConlangs(langs);
    wb_.language = l.id;
    setFocus({Focus::Language, l.id, {}});
    renameFocused();
}

void StudioApp::addLanguageWord() {
    commitFields();
    auto langs = conlangs(wb_.world, wb_.world.empty() ? wb_.book : Id{});
    for (auto& l : langs)
        if (l.id == wb_.language) {
            if (std::any_of(l.words.begin(), l.words.end(), [](const conlang::Word& x) { return x.word == "new word"; })) {
                setFocus({Focus::Language, l.id, "w:new word"});   // one waiting already: that one
                renameFocused();
                return;
            }
            l.words.insert(l.words.begin(), conlang::Word{"new word"});
            // Kept unsorted until saved with its real spelling: it's found by its place at the top.
            const std::string key = conlangKey(wb_.world, wb_.world.empty() ? wb_.book : Id{});
            lib_->setSetting(key, conlang::toJson(langs));
            setFocus({Focus::Language, l.id, "w:new word"});
            renameFocused();
            return;
        }
}

void StudioApp::importLanguages() {
    commitFields();
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC types[] = {{L"Word lists and lexicons (CSV, TSV, SFM, text, .nrlang)", L"*.csv;*.tsv;*.txt;*.sfm;*.db;*.mdf;*.lex;*.nrlang;*.json"},
                                 {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, types);
    dlg->SetTitle(L"Import a language");
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
        if (size > conlang::kMaxFileBytes) {
            cjui::messageBox(hwnd(), L"That file is too big to be a word list (over 32 MB).", L"Import a language", MB_OK | MB_ICONWARNING);
            return;
        }
        f.seekg(0);
        bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    std::vector<conlang::Language> got;
    std::string err;
    if (!conlang::importFile(narrow(path), bytes, got, err)) {
        cjui::messageBox(hwnd(), w(err).c_str(), L"Import a language", MB_OK | MB_ICONWARNING);
        return;
    }
    auto langs = conlangs(wb_.world, wb_.world.empty() ? wb_.book : Id{});
    // One language in the file and one picked here: add to it, or keep it as a language of its own.
    const conlang::Language* picked = nullptr;
    for (auto& l : langs) if (l.id == wb_.language) picked = &l;
    std::size_t words = 0;
    for (auto& g : got) words += g.words.size();
    bool into = false;
    if (got.size() == 1 && picked) {
        const int c = popupAt(hwnd(), {L"A new language: " + w(got[0].name) + L" (" + std::to_wstring(words) + L" words)",
                                       L"Add the words to " + w(picked->name)});
        if (c < 0) return;
        into = c == 1;
    }
    std::string firstNew;
    if (into) {
        for (auto& l : langs)
            if (l.id == wb_.language) {
                const std::size_t added = conlang::merge(l, got[0]);
                flash(L"Added " + std::to_wstring(added) + L" new words to " + w(l.name) +
                          (added < got[0].words.size() ? L" (the others were there already; any missing meanings were filled in)." : L"."),
                      7);
            }
    } else {
        for (auto& g : got) {
            if (langs.size() >= conlang::kMaxLanguages) break;
            g.id = conlang::newId(langs);
            if (firstNew.empty()) firstNew = g.id;
            langs.push_back(std::move(g));
        }
        flash(L"Imported " + std::to_wstring(got.size()) + (got.size() == 1 ? L" language" : L" languages") + L" with " + std::to_wstring(words) +
                  L" words. Writing's spell checker knows them now.",
              7);
    }
    saveConlangs(langs);
    if (!firstNew.empty()) wb_.language = firstNew;
    setFocus({Focus::Language, wb_.language, {}});
}

void StudioApp::exportLanguage(const std::string& id) {
    commitFields();
    const auto langs = conlangs(wb_.world, wb_.world.empty() ? wb_.book : Id{});
    const conlang::Language* l = nullptr;
    for (auto& x : langs) if (x.id == id) l = &x;
    if (!l) return;
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC types[] = {{L"NorthRay language (.nrlang)", L"*.nrlang"}, {L"Spreadsheet (CSV)", L"*.csv"}};
    dlg->SetFileTypes(2, types);
    dlg->SetDefaultExtension(L"nrlang");
    std::wstring name = w(l->name);
    for (auto& c : name)
        if (wcschr(L"\\/:*?\"<>|", c)) c = L'-';
    dlg->SetFileName(name.c_str());
    dlg->SetTitle(L"Export the language");
    std::wstring path;
    UINT type = 1;
    if (SUCCEEDED(dlg->Show(hwnd()))) {
        dlg->GetFileTypeIndex(&type);
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { path = p; CoTaskMemFree(p); }
            item->Release();
        }
    }
    dlg->Release();
    if (path.empty()) return;
    std::wstring lowPath = path;
    for (auto& c : lowPath) c = (wchar_t)towlower(c);
    // "words.csv" typed while the .nrlang type was picked: some dialogs add .nrlang after it; it's a CSV.
    if (lowPath.size() > 11 && lowPath.substr(lowPath.size() - 11) == L".csv.nrlang") {
        path.resize(path.size() - 7);
        lowPath.resize(lowPath.size() - 7);
    }
    const bool csv = type == 2 || (lowPath.size() > 4 && lowPath.substr(lowPath.size() - 4) == L".csv");
    const std::string data = csv ? conlang::exportCsv(*l) : conlang::exportJson(*l);
    std::ofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
    f << data;
    f.close();
    if (!f) {
        cjui::messageBox(hwnd(), L"The file couldn't be written.", L"Export the language", MB_OK | MB_ICONWARNING);
        return;
    }
    flash(L"Exported " + w(l->name) + L" (" + std::to_wstring(l->words.size()) + L" words).", 5);
}

void StudioApp::removeLanguage(const std::string& id) {
    commitFields();
    auto langs = conlangs(wb_.world, wb_.world.empty() ? wb_.book : Id{});
    auto it = std::find_if(langs.begin(), langs.end(), [&](const conlang::Language& l) { return l.id == id; });
    if (it == langs.end()) return;
    // At once, with Undo in the note (0.65).
    const auto before = langs;
    const std::wstring name = w(it->name);
    langs.erase(it);
    loadedFocus_ = focus_ = Focus{};   // what the panel showed is gone (nothing to save)
    saveConlangs(langs);
    wb_.language = langs.empty() ? std::string() : langs[0].id;
    if (!wb_.language.empty()) setFocus({Focus::Language, wb_.language, {}});
    flashUndo(L"Removed " + name + L".", [this, before, id] {
        saveConlangs(before);
        wb_.language = id;
        setFocus({Focus::Language, id, {}});
        invalidate();
    });
}

// --- the panel's fields ---

void StudioApp::loadLanguageFields() {
    for (const auto& l : conlangs(wb_.world, wb_.world.empty() ? wb_.book : Id{})) {
        if (l.id != focus_.id) continue;
        if (focus_.contextSeries.rfind("w:", 0) == 0) {
            const std::string word = focus_.contextSeries.substr(2);
            for (auto& wd : l.words)
                if (wd.word == word) {
                    setFieldText(FName, wd.word);
                    setFieldText(FDescription, wd.meaning);
                    setFieldText(FGenre, wd.part);
                    setFieldText(FAlt, wd.pronunciation);
                    setFieldText(FNotes, wd.notes);
                    return;
                }
            return;
        }
        setFieldText(FName, l.name);
        setFieldText(FNotes, l.notes);
        return;
    }
}

void StudioApp::commitLanguageFields(const Focus& f) {
    auto langs = conlangs(wb_.world, wb_.world.empty() ? wb_.book : Id{});
    for (auto& l : langs) {
        if (l.id != f.id) continue;
        if (f.contextSeries.rfind("w:", 0) == 0) {
            const std::string word = f.contextSeries.substr(2);
            for (auto& wd : l.words)
                if (wd.word == word) {
                    const std::string nw = fieldText(FName);
                    if (!nw.empty()) wd.word = nw;   // never an empty word
                    wd.meaning = fieldText(FDescription);
                    wd.part = fieldText(FGenre);
                    wd.pronunciation = fieldText(FAlt);
                    wd.notes = fieldText(FNotes);
                    // Its new spelling is what the panel now edits.
                    if (focus_ == f) focus_.contextSeries = loadedFocus_.contextSeries = "w:" + wd.word;
                    break;
                }
        } else {
            const std::string nm = fieldText(FName);
            if (!nm.empty()) l.name = nm;
            l.notes = fieldText(FNotes);
        }
        saveConlangs(langs);
        return;
    }
}

// --- the page ---

void StudioApp::drawLanguages(const Rect& r) {
    const auto& langs = conlangs(wb_.world, wb_.world.empty() ? wb_.book : Id{});
    gfx_.pushClip(r);
    setHitClip(r);
    const float pad = r.w > 900 ? 40 : 28;
    const float cw = std::min(r.w - pad * 2, 900.0f);
    const Rect m{r.x + pad, r.y + 26 - wb_.scroll, cw, 0};
    float y = m.y;
    y += pageHeader(m, L"WORLDBUILDING", L"Languages",
                    L"Your world's own languages: Writing knows their words, and suggests them for near misses.");
    float bx = m.x, bw = 0;
    smallButton("lang:import", bx, y, L"Import a language…", [this] { importLanguages(); }, &bw);
    bx += bw + 10;
    smallButton("lang:new", bx, y, L"New language", [this] { newLanguage(); }, &bw);
    y += 46;
    if (langs.empty()) {
        y += emptyState({m.x, y, cw, 260}, cjui::Icon::Comment, L"No languages yet.", L"+ New language", [this] { newLanguage(); });
    } else {
        // The languages, as a row of tabs (wrapping).
        float tx = m.x;
        for (auto& l : langs) {
            const std::wstring label = w(l.name) + L"  " + std::to_wstring(l.words.size());
            const float tw = std::min(cw, gfx_.measureWidth(label, Font::BodyStrong) + 28);
            if (tx + tw > m.right() && tx > m.x) {
                tx = m.x;
                y += 40;
            }
            const Rect tr{tx, y, tw, 32};
            const std::string id = "lang:tab:" + l.id;
            addHit(id, tr, [this, lid = l.id] {
                commitFields();
                wb_.language = lid;
                wb_.scroll = 0;
                setFocus({Focus::Language, lid, {}});
            });
            const bool sel = wb_.language == l.id;
            const float hv = hoverAmount(id);
            if (sel) gfx_.fillRounded(tr, 3, pal_.surface);
            else if (hv > 0.01f) gfx_.fillRounded(tr, 3, pal_.surface.withAlpha(0.6f * hv));
            if (sel) gfx_.fill({tr.x, tr.bottom() - 3, tr.w, 3}, pal_.ribbon);
            gfx_.text(w(l.name), {tr.x + 14, tr.y, tw - 28, tr.h}, sel ? Font::BodyStrong : Font::Body, pal_.ink, {Align::Left, false, true, true});
            const float nw = gfx_.measureWidth(w(l.name), sel ? Font::BodyStrong : Font::Body);
            gfx_.text(std::to_wstring(l.words.size()), {tr.x + 14 + nw + 8, tr.y, tw, tr.h}, Font::Small, pal_.inkFaint, {Align::Left, false, false, true});
            if (focused(id)) focusRing(tr);
            tx += tw + 8;
        }
        y += 50;
        const conlang::Language* l = nullptr;
        for (auto& x : langs) if (x.id == wb_.language) l = &x;
        if (l) {
            bx = m.x;
            smallButton("lang:addword", bx, y, L"Add a word", [this] { addLanguageWord(); }, &bw);
            bx += bw + 10;
            smallButton("lang:export", bx, y, L"Export…", [this, id = l->id] { exportLanguage(id); }, &bw);
            bx += bw + 10;
            smallButton("lang:edit", bx, y, L"Name and notes", [this, id = l->id] { commitFields(); setFocus({Focus::Language, id, {}}); }, &bw);
            linkText("lang:remove", m.right() - gfx_.measureWidth(L"Remove this language", Font::Small), y + 8, L"Remove this language",
                     [this, id = l->id] { removeLanguage(id); });
            y += 50;
            if (l->words.empty()) {
                y += gfx_.text(L"No words yet: add some, or import a word list into it.", {m.x, y, cw, 24}, Font::Small, pal_.inkFaint) + 20;
            } else {
                // The words, like a dictionary: only the rows on screen are drawn (a language can have thousands).
                const float rowH = 30;
                const float wordW = std::min(220.0f, cw * 0.3f), partW = 90;
                gfx_.text(L"WORD", {m.x, y, wordW, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
                gfx_.text(L"MEANING", {m.x + wordW + partW + 16, y, cw - wordW - partW - 16, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
                y += 22;
                gfx_.line(m.x, y - 3.5f, m.right(), y - 3.5f, pal_.rule);
                std::string affixNote;
                for (std::size_t i = 0; i < l->words.size(); ++i, y += rowH) {
                    if (y + rowH < r.y || y > r.bottom()) continue;
                    const conlang::Word& wd = l->words[i];
                    const Rect row{m.x - 8, y, cw + 16, rowH};
                    const std::string id = "lang:w:" + std::to_string(i);
                    const bool sel = focus_.kind == Focus::Language && focus_.id == l->id && focus_.contextSeries == "w:" + wd.word;
                    addHit(id, row, [this, lid = l->id, word = wd.word] {
                        commitFields();
                        setFocus({Focus::Language, lid, "w:" + word});
                    });
                    const float hv = hoverAmount(id);
                    if (sel) gfx_.fillRounded(row, 3, pal_.surface);
                    else if (hv > 0.01f) gfx_.fillRounded(row, 3, pal_.surface.withAlpha(0.6f * hv));
                    const bool affix = wd.word.size() > 1 && (wd.word[0] == '-' || wd.word.back() == '-');
                    gfx_.text(w(wd.word), {m.x, y, wordW - 8, rowH}, Font::BodyStrong, affix ? pal_.ribbon : pal_.ink, {Align::Left, false, true, true});
                    gfx_.text(w(wd.part), {m.x + wordW, y, partW, rowH}, Font::Small, pal_.inkFaint, {Align::Left, false, true, true});
                    gfx_.text(w(wd.meaning), {m.x + wordW + partW + 16, y, cw - wordW - partW - 16, rowH}, Font::Body, pal_.inkMuted,
                              {Align::Left, false, true, true});
                    if (focused(id)) focusRing(row);
                }
                y += 14;
                y += gfx_.text(L"Words that start or end with a hyphen are parts of words: \u201C-in\u201D is an ending and \u201Cve-\u201D a "
                               L"beginning. A word of the language with its parts on counts as spelt right.",
                               {m.x, y, cw, 40}, Font::Small, pal_.inkFaint) +
                     20;
            }
        }
    }
    wb_.content = y + wb_.scroll - r.y + 40;
    wb_.scroll = std::clamp(wb_.scroll, 0.0f, std::max(0.0f, wb_.content - r.h));
    clearHitClip();
    gfx_.popClip();
}

void StudioApp::drawLanguagePanel(const Rect& r) {
    if (!(focus_ == loadedFocus_)) loadFields();
    gfx_.pushClip(r);
    setHitClip(r);
    const Rect c{r.x + 20, r.y, r.w - 40, 0};
    float y = r.y + 18 - wb_.panelScroll;
    if (focus_.kind == Focus::Language && focus_.contextSeries.rfind("w:", 0) == 0) {
        panelHeading(c, y, L"WORD", true);
        y = panelField(c, y, L"Word", FName);
        y = panelField(c, y, L"Meaning", FDescription, 80);
        y = panelField(c, y, L"Part of speech (noun, verb…)", FGenre);
        y = panelField(c, y, L"How it's said", FAlt);
        y = panelField(c, y, L"Notes", FNotes, 110);
        linkText("lang:delword", c.x, y, L"Remove this word", [this] {
            const Focus f = focus_;
            loadedFocus_ = focus_ = Focus{};   // nothing to save from the panel
            auto langs = conlangs(wb_.world, wb_.world.empty() ? wb_.book : Id{});
            for (auto& l : langs)
                if (l.id == f.id) {
                    const std::string word = f.contextSeries.substr(2);
                    l.words.erase(std::remove_if(l.words.begin(), l.words.end(), [&](const conlang::Word& x) { return x.word == word; }), l.words.end());
                }
            saveConlangs(langs);
            setFocus({Focus::Language, f.id, {}});
        });
        y += 30;
    } else if (focus_.kind == Focus::Language) {
        panelHeading(c, y, L"LANGUAGE", true);
        y = panelField(c, y, L"Name", FName);
        y = panelField(c, y, L"Notes (grammar, sounds, where it's spoken)", FNotes, 220);
    } else {
        panelHeading(c, y, L"LANGUAGES", true);
        y += gfx_.text(L"Pick a language or a word to edit it here. Changes save as you type.", {c.x, y, c.w, 60}, Font::Small, pal_.inkMuted) + 12;
    }
    wb_.panelContent = y + wb_.panelScroll - r.y + 20;
    clearHitClip();
    gfx_.popClip();
}

}  // namespace nrs
