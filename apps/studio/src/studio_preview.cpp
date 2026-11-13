// The Preview workspace: the book as an ebook, on a simulated phone, e-reader
// or tablet. The page itself (web/ebook.html) flows each chapter into pages;
// this side chooses the device, theme, text size and font, and the chapter.
#include "studio_app.hpp"

#include "studio/ebook.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace nrs {

using nlohmann::json;
using namespace studio;

static std::wstring w(const std::string& s) { return widen(s); }

static std::wstring thousands(std::int64_t n) {
    std::wstring s = std::to_wstring(n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, L",");
    return s;
}

void StudioApp::openPreview(const Id& bookId) {
    if (!lib_ || !lib_->book(bookId)) return;
    commitFields();
    if (ws_ == Workspace::Writing && editor_.ready()) editor_.send(R"({"type":"flush"})");
    if (pv_.book != bookId) { pv_ = {}; pv_.book = bookId; }
    ws_ = Workspace::Preview;
    setFocus({Focus::Book, bookId, {}});
    ensurePreview();
    sendPreview();   // always: the text may have changed since last time
    invalidate();
}

void StudioApp::ensurePreview() {
    if (previewStarted_ || !lib_) return;
    previewStarted_ = true;
    previewNote_.clear();
    CreateDirectoryW((stateDir() + L"\\WebView2").c_str(), nullptr);
    const std::wstring objects = w(lib_->folder()) + L"\\objects";
    if (!preview_.create(hwnd(), exeDir(), stateDir() + L"\\WebView2", objects, L"ebook.html",
                         [this](const std::string& m) { previewMessage(m); }))
    {
        previewNote_ = preview_.error();
        const size_t at = previewNote_.find(L"The editor");
        if (at != std::wstring::npos) previewNote_.replace(at, 10, L"The ebook preview");
    }
}

void StudioApp::sendPreview() {
    if (!lib_ || !lib_->book(pv_.book)) return;
    const auto src = typeset::snapshot(*lib_, pv_.book);
    const auto content = ebook::build(src, [](const std::string& h) {
        return "https://objects.nrstudio.localhost/" + h.substr(0, 2) + "/" + h.substr(2);
    });
    pv_.chapters.clear();
    pv_.words = 0;
    json chapters = json::array();
    for (auto& c : content.chapters) {
        chapters.push_back({{"title", c.title}, {"part", c.part}, {"html", c.html}});
        pv_.chapters.push_back({c.title, c.part});
        pv_.words += c.words;
    }
    // The book's typeface (from its typesetting design), for "Book" font.
    const std::string saved = lib_->setting("typeset:" + pv_.book);
    const auto design = typeset::designFromJson(saved);
    json fonts = json::object();
    {
        namespace fs = std::filesystem;
        const fs::path dir = fs::path(exeDir() + L"\\fonts") / w(design.font);
        std::error_code ec;
        for (auto& e : fs::directory_iterator(dir, ec)) {
            const std::string n = narrow(e.path().filename().wstring());
            const std::string rel = design.font + "/" + n;
            auto ends = [&](const char* s) { const std::string t = s; return n.size() > t.size() && n.compare(n.size() - t.size(), t.size(), t) == 0; };
            if (ends("-Regular.ttf")) fonts["regular"] = rel;
            else if (ends("-BoldItalic.ttf")) fonts["boldItalic"] = rel;
            else if (ends("-Italic.ttf")) fonts["italic"] = rel;
            else if (ends("-Bold.ttf")) fonts["bold"] = rel;
        }
    }
    pv_.chapter = std::clamp(pv_.chapter, 0, std::max(0, (int)pv_.chapters.size() - 1));
    if (!preview_.ready()) { pv_.sent = false; return; }   // sent when the page says it's ready
    json msg = {{"type", "load"},
                {"chapters", chapters},
                {"css", ebook::stylesheet()},
                {"title", content.title},
                {"language", content.language},
                {"fonts", fonts},
                {"chapter", pv_.chapter},
                {"settings",
                 {{"device", settings_.ebookDevice}, {"theme", settings_.ebookTheme}, {"scale", settings_.ebookScale},
                  {"font", settings_.ebookFont}, {"dark", pal_.dark}}}};
    pv_.sent = preview_.send(msg.dump());
}

void StudioApp::sendPreviewSettings() {
    saveSettings();
    if (preview_.ready())
        preview_.send(json{{"type", "settings"}, {"device", settings_.ebookDevice}, {"theme", settings_.ebookTheme},
                           {"scale", settings_.ebookScale}, {"font", settings_.ebookFont}, {"dark", pal_.dark}}
                          .dump());
    invalidate();
}

void StudioApp::previewMessage(const std::string& text) {
    const json m = json::parse(text, nullptr, false);
    if (!m.is_object()) return;
    const std::string type = m.value("type", "");
    if (type == "ready") {
        if (ws_ == Workspace::Preview) sendPreview();
    } else if (type == "position") {
        pv_.chapter = m.value("chapter", 0);
        pv_.page = m.value("page", 0);
        pv_.pages = std::max(1, m.value("pages", 1));
        invalidate();
    } else if (type == "failed") {
        previewNote_ = L"The preview stopped working. Close and reopen the Studio to try again.";
        invalidate();
    }
}

// ---------------------------------------------------------------- drawing

void StudioApp::drawPreviewSidebar(const Rect& r) {
    gfx_.fill(r, pal_.sunken);
    gfx_.line(r.right() - 0.5f, r.y, r.right() - 0.5f, r.bottom(), pal_.rule);
    const studio::Book* b = lib_->book(pv_.book);
    if (!b) return;
    gfx_.pushClip(r);
    setHitClip(r);
    const float x = r.x + 10, wd = r.w - 20, tx0 = x + 12, cw = wd - 24;
    float y = r.y + 14 - pv_.sideScroll;
    linkText("side:pv:library", tx0, y, L"‹ Library", [this] { ws_ = Workspace::Library; goTo(View::Book, pv_.book); });
    y += 30;
    gfx_.text(L"EBOOK PREVIEW", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 20;
    y += gfx_.text(w(b->title), {tx0, y, cw, 90}, Font::SerifTitle, pal_.ink) + 32;

    // Devices.
    gfx_.text(L"DEVICE", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 24;
    struct D { const char* id; const wchar_t* name; const wchar_t* note; };
    static const D devices[] = {{"phone", L"Phone", L"A phone held upright"},
                                {"ereader", L"E-reader", L"A 6-inch e-ink reader: greys, no colour"},
                                {"tablet", L"Tablet", L"A tablet held upright"}};
    for (const D& d : devices) {
        const std::string id = std::string("side:pv:dev:") + d.id;
        const Rect rr{x, y, wd, 46};
        addHit(id, rr, [this, dev = std::string(d.id)] { settings_.ebookDevice = dev; sendPreviewSettings(); });
        const bool sel = settings_.ebookDevice == d.id;
        const float hv = hoverAmount(id);
        if (sel) { gfx_.fillRounded(rr, 3, pal_.surface); gfx_.fill({rr.x, rr.y + 7, 3, rr.h - 14}, pal_.ribbon); }
        else if (hv > 0.01f) gfx_.fillRounded(rr, 3, pal_.surface.withAlpha(0.6f * hv));
        gfx_.text(d.name, {tx0, rr.y + 5, cw, 20}, sel ? Font::BodyStrong : Font::Body, pal_.ink, {Align::Left, false});
        gfx_.text(d.note, {tx0, rr.y + 24, cw, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
        if (focused(id)) focusRing(rr);
        y += rr.h + 4;
    }
    y += 16;
    // The reader's own choices.
    gfx_.text(L"READER'S SETTINGS", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 24;
    gfx_.text(L"Page", {tx0, y, cw, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
    y += 22;
    const int ti = settings_.ebookTheme == "sepia" ? 1 : settings_.ebookTheme == "night" ? 2 : 0;
    segmented("side:pv:theme", {tx0, y, cw, 28}, {L"White", L"Sepia", L"Night"}, ti, [this](int i) {
        settings_.ebookTheme = i == 1 ? "sepia" : i == 2 ? "night" : "white";
        sendPreviewSettings();
    });
    y += 40;
    {
        wchar_t v[32];
        swprintf(v, 32, L"%d%%", (int)std::lround(settings_.ebookScale * 100));
        gfx_.text(L"Text size", {tx0, y, cw * 0.6f, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
        gfx_.text(v, {tx0 + cw * 0.4f, y, cw * 0.6f, 18}, Font::Small, pal_.ink, {Align::Right, false});
        y += 22;
        slider("side:pv:size", {tx0, y, cw, 22}, (float)((settings_.ebookScale - 0.7) / 1.1), [this](float f) {
            settings_.ebookScale = std::round((0.7 + 1.1 * f) * 20) / 20;
            sendPreviewSettings();
        });
        y += 36;
    }
    gfx_.text(L"Font", {tx0, y, cw, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
    y += 22;
    const int fi = settings_.ebookFont == "serif" ? 1 : settings_.ebookFont == "sans" ? 2 : 0;
    segmented("side:pv:font", {tx0, y, cw, 28}, {L"Book's", L"Serif", L"Sans"}, fi, [this](int i) {
        settings_.ebookFont = i == 1 ? "serif" : i == 2 ? "sans" : "book";
        sendPreviewSettings();
    });
    y += 44;
    // Chapters.
    gfx_.text(L"CHAPTERS", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 24;
    for (size_t i = 0; i < pv_.chapters.size(); ++i) {
        const std::string id = "side:pv:ch:" + std::to_string(i);
        const Rect rr{x, y, wd, 28};
        addHit(id, rr, [this, i] {
            pv_.chapter = (int)i;
            preview_.send(json{{"type", "goto"}, {"chapter", (int)i}}.dump());
            invalidate();
        });
        const bool sel = (int)i == pv_.chapter;
        const float hv = hoverAmount(id);
        if (sel) gfx_.fillRounded(rr, 3, pal_.surface);
        else if (hv > 0.01f) gfx_.fillRounded(rr, 3, pal_.surface.withAlpha(0.6f * hv));
        std::wstring t = w(pv_.chapters[i].first);
        if (pv_.chapters[i].second) CharUpperBuffW(t.data(), (DWORD)t.size());
        gfx_.text(t, {tx0 + (pv_.chapters[i].second ? 0.0f : 10.0f), rr.y, cw - 10, rr.h},
                  pv_.chapters[i].second ? Font::SmallStrong : (sel ? Font::BodyStrong : Font::Body), sel ? pal_.ink : pal_.inkMuted.mix(pal_.ink, 0.35f),
                  {Align::Left, false, true, true});
        if (focused(id)) focusRing(rr);
        y += 30;
    }
    pv_.sideContent = y + pv_.sideScroll - r.y + 20;
    clearHitClip();
    gfx_.popClip();
}

void StudioApp::drawPreviewPanel(const Rect& r) {
    gfx_.fill(r, pal_.paper.mix(pal_.surface, 0.5f));
    gfx_.line(r.x + 0.5f, r.y, r.x + 0.5f, r.bottom(), pal_.rule);
    gfx_.pushClip(r);
    setHitClip(r);
    const Rect c{r.x + 20, r.y, r.w - 40, 0};
    float y = r.y + 18;
    panelHeading(c, y, L"WHERE YOU ARE", true);
    if (!pv_.chapters.empty()) {
        const int ch = std::clamp(pv_.chapter, 0, (int)pv_.chapters.size() - 1);
        y += gfx_.text(w(pv_.chapters[ch].first), {c.x, y, c.w, 40}, Font::BodyStrong, pal_.ink) + 2;
        gfx_.text(L"Screen " + std::to_wstring(pv_.page + 1) + L" of " + std::to_wstring(pv_.pages) + L" in this chapter",
                  {c.x, y, c.w, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
        y += 30;
        const float bw = (c.w - 10) / 2;
        button("pv:prev", {c.x, y, bw, 32}, L"‹ Back", [this] { preview_.send(R"({"type":"turn","delta":-1})"); });
        button("pv:next", {c.x + bw + 10, y, bw, 32}, L"Next ›", [this] { preview_.send(R"({"type":"turn","delta":1})"); }, true);
        y += 44;
        y += gfx_.text(L"Or click the right of the screen to turn the page, the left to go back. The arrow keys work too.",
                       {c.x, y, c.w, 60}, Font::Small, pal_.inkFaint) + 10;
    }
    panelHeading(c, y, L"ABOUT EBOOKS", false);
    y += gfx_.text(L"Ebooks reflow: each reader chooses the text size, font and page colour, so where pages begin and end changes "
                   L"from device to device. The trim size, margins and page numbers from Typesetting only apply to print.",
                   {c.x, y, c.w, 120}, Font::Small, pal_.inkMuted) + 10;
    y += gfx_.text(L"This is an approximation of common reading apps; real devices differ a little. The EPUB itself is made in "
                   L"Publishing.",
                   {c.x, y, c.w, 80}, Font::Small, pal_.inkFaint) + 10;
    panelHeading(c, y, L"THIS BOOK", false);
    auto row = [&](const std::wstring& k, const std::wstring& v) {
        gfx_.text(k, {c.x, y, c.w * 0.5f, 20}, Font::Small, pal_.inkMuted, {Align::Left, false});
        gfx_.text(v, {c.x + c.w * 0.4f, y, c.w * 0.6f, 20}, Font::Small, pal_.ink, {Align::Right, false, true});
        y += 22;
    };
    int chapters = 0;
    for (auto& ch : pv_.chapters) if (!ch.second) ++chapters;
    row(L"Chapters", std::to_wstring(chapters));
    row(L"Words", thousands(pv_.words));
    button("pv:refresh", {c.x, y + 10, c.w, 32}, L"Refresh from the text", [this] { sendPreview(); flash(L"The preview shows the latest text.", 3); });
    clearHitClip();
    gfx_.popClip();
}

}  // namespace nrs
