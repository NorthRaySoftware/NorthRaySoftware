// NorthRay Browser: reference boards (northray://references) and the colour picker.
//
// Right-click a picture on a page: "Save picture to a reference board" downloads it (on its own
// thread) into User\references\ and lists it on a board with the page it came from. Right-click
// anywhere (or the menu): "Pick a colour from the page" shows a still of the page with a loupe;
// a click copies the colour ("#1F3B2C") and keeps it on the board.
#include "browser_window.hpp"
#include "cjui/dialog.hpp"

#include "nrb/url.hpp"

#include <shellapi.h>
#include <shlobj.h>
#include <urlmon.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <thread>

namespace nrb {

using cjui::Align;
using cjui::Color;
using cjui::Font;
using cjui::MenuItem;
using cjui::Rect;

struct BrowserWindow::PictureJob {
    std::wstring url, page, title, file;   // file: where it was saved
    std::int64_t board = 0;
    std::wstring error;
};

std::wstring BrowserWindow::referencesFolder() const {
    const std::wstring state = settingsPath_.substr(0, settingsPath_.find_last_of(L"\\/"));
    return state + L"\\User\\references";
}

void BrowserWindow::openReferenceStore() {
    const std::wstring state = settingsPath_.substr(0, settingsPath_.find_last_of(L"\\/"));
    CreateDirectoryW((state + L"\\User").c_str(), nullptr);
    CreateDirectoryW(referencesFolder().c_str(), nullptr);
    std::string error;
    refs_ = ReferenceStore::open(narrow(state + L"\\User\\references.db"), error);
    if (!refs_) { flash(L"Reference boards couldn't be opened: " + widen(error), 12); return; }
    refBoard_ = refs_->firstBoard();
}

namespace {

std::string readBytes(const std::wstring& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// What kind of picture the bytes are, as a file extension ("" if not one kept on boards).
const wchar_t* pictureExt(const std::string& b) {
    if (b.size() > 8 && (unsigned char)b[0] == 0x89 && b.compare(1, 3, "PNG") == 0) return L".png";
    if (b.size() > 3 && (unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xD8) return L".jpg";
    if (b.size() > 6 && b.compare(0, 3, "GIF") == 0) return L".gif";
    if (b.size() > 12 && b.compare(0, 4, "RIFF") == 0 && b.compare(8, 4, "WEBP") == 0) return L".webp";
    if (b.size() > 2 && b[0] == 'B' && b[1] == 'M') return L".bmp";
    return L"";
}

std::string base64Decode(const std::string& in) {
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        int d;
        if (c >= 'A' && c <= 'Z') d = c - 'A';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
        else if (c >= '0' && c <= '9') d = c - '0' + 52;
        else if (c == '+' || c == '-') d = 62;
        else if (c == '/' || c == '_') d = 63;
        else continue;
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 0) { out += (char)((val >> bits) & 0xFF); bits -= 8; }
    }
    return out;
}

std::wstring hashName(const std::wstring& url) {
    std::uint64_t h = 1469598103934665603ULL;
    for (wchar_t c : url) { h ^= (std::uint64_t)c; h *= 1099511628211ULL; }
    wchar_t b[24];
    swprintf(b, 24, L"%016llx", (unsigned long long)h);
    return b;
}

void copyText(HWND owner, const std::wstring& t) {
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t))) {
        std::copy_n(t.c_str(), t.size() + 1, static_cast<wchar_t*>(GlobalLock(g)));
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
}

Color rgbColor(std::uint32_t rgb) { return {((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, 1}; }

}  // namespace

void BrowserWindow::savePicture(const std::wstring& picture, const std::wstring& page, const std::wstring& title) {
    if (!refs_) return;
    // Which board: straight to the only one, otherwise a choice at the pointer.
    const auto boards = refs_->boards();
    std::int64_t board = boards.empty() ? 0 : boards[0].id;
    if (boards.size() > 1) {
        std::vector<MenuItem> items{MenuItem::item(L"Save to which board?", 0, {}, false), MenuItem::sep()};
        for (size_t i = 0; i < boards.size(); ++i)
            items.push_back(MenuItem::item(widen(boards[i].name), 1 + (UINT)i, {}, true, boards[i].id == refBoard_));
        POINT pt;
        GetCursorPos(&pt);
        const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
        if (!cmd) return;
        board = boards[cmd - 1].id;
    }
    if (picture.rfind(L"data:", 0) != 0 && refs_->hasSource(board, narrow(picture))) { flash(L"That picture is already on the board.", 5); return; }
    auto* job = new PictureJob{picture, page, title, {}, board, {}};
    const std::wstring folder = referencesFolder();
    const HWND target = hwnd();
    flash(L"Saving the picture…", 3);
    std::thread([job, folder, target] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::string bytes;
        if (job->url.rfind(L"data:", 0) == 0) {
            const size_t comma = job->url.find(L',');
            const std::wstring head = job->url.substr(0, comma);
            if (comma != std::wstring::npos && head.find(L";base64") != std::wstring::npos) bytes = base64Decode(narrow(job->url.substr(comma + 1)));
        } else if (job->url.rfind(L"http://", 0) == 0 || job->url.rfind(L"https://", 0) == 0) {
            const std::wstring tmp = folder + L"\\download-" + hashName(job->url) + L".part";
            if (SUCCEEDED(URLDownloadToFileW(nullptr, job->url.c_str(), tmp.c_str(), 0, nullptr))) bytes = readBytes(tmp);
            DeleteFileW(tmp.c_str());
            if (bytes.empty()) job->error = L"The picture couldn't be downloaded.";
        } else job->error = L"Only pictures from web pages can be saved.";
        if (job->error.empty()) {
            const wchar_t* ext = pictureExt(bytes);
            if (bytes.size() > 50u * 1024 * 1024) job->error = L"The picture is over 50 MB.";
            else if (!*ext) job->error = L"That isn't a picture that can be kept (PNG, JPEG, GIF, WebP or BMP).";
            else {
                job->file = hashName(job->url) + ext;
                std::ofstream out((folder + L"\\" + job->file).c_str(), std::ios::binary);
                out << bytes;
                if (!out) job->error = L"The picture couldn't be saved.";
            }
        }
        CoUninitialize();
        if (!PostMessageW(target, WM_APP + 21, 0, reinterpret_cast<LPARAM>(job))) delete job;
    }).detach();
}

void BrowserWindow::pictureSaved(PictureJob* jobRaw) {
    std::unique_ptr<PictureJob> job(jobRaw);
    if (!refs_) return;
    if (!job->error.empty()) { flash(job->error, 7); return; }
    RefPicture p;
    p.board = job->board;
    p.file = narrow(job->file);
    p.source = job->url.rfind(L"data:", 0) == 0 ? std::string() : narrow(job->url);
    p.page = narrow(job->page);
    p.title = narrow(job->title);
    const auto size = gfx_.imageSize(referencesFolder() + L"\\" + job->file);
    p.width = (int)size.first;
    p.height = (int)size.second;
    if (!p.width) { DeleteFileW((referencesFolder() + L"\\" + job->file).c_str()); flash(L"Windows couldn't read that picture.", 7); return; }
    refs_->addPicture(p);
    std::wstring board;
    for (const auto& b : refs_->boards()) if (b.id == job->board) board = widen(b.name);
    flash(L"Saved to " + board + L". Menu › Reference boards shows it.", 6);
    invalidate();
}

void BrowserWindow::showReferencesPage() {
    if (!refs_) { flash(L"Reference boards aren't available.", 6); return; }
    int target = 0;
    for (TabView& v : views_)
        if (v.showStart && v.referencesPage) target = v.id;
    if (!target)
        if (TabView* a = activeView(); a && a->showStart && !a->bookmarksPage && !a->historyPage && !a->downloadsPage) target = a->id;
    if (!target) target = newTab(L"", true, true);
    TabView* v = view(target);
    if (!v) return;
    if (v->bookmarksPage && v->id == model_.activeId()) finishRename(true);
    v->showStart = true;
    v->bookmarksPage = v->historyPage = v->downloadsPage = false;
    v->referencesPage = true;
    v->error.reset();
    if (Tab* t = model_.find(target)) { t->title = "Reference boards"; t->url = "northray://references"; }
    activateTab(target);
    resetScroll();
    updateAddressText();
    updateTitle();
    invalidate();
}

void BrowserWindow::addMainColours(std::int64_t id) {
    const std::string file = refs_->pictureFile(id);
    int w = 0, h = 0;
    std::vector<unsigned char> px;
    if (file.empty() || !decodeImageFile(referencesFolder() + L"\\" + widen(file), w, h, px)) { flash(L"That picture couldn't be read.", 6); return; }
    const auto cs = mainColours(px.data(), w, h, 6);
    for (auto it = cs.rbegin(); it != cs.rend(); ++it) refs_->addColour({0, refBoard_, *it, {}, 0});   // most used shows first
    flash(L"Added the picture's " + std::to_wstring(cs.size()) + L" main colours to the board.", 5);
    invalidate();
}

void BrowserWindow::drawReferencesPage(const Rect& r) {
    gfx_.fill(r, pal_.paper);
    tileRects_.clear();
    rowRects_.clear();
    if (!refs_) return;
    const auto boards = refs_->boards();
    if (std::none_of(boards.begin(), boards.end(), [&](const RefBoard& b) { return b.id == refBoard_; })) refBoard_ = refs_->firstBoard();
    const float w = std::min(r.w - 48, 1180.0f);
    const float x = r.x + (r.w - w) / 2;
    const Rect clip{r.x, r.y, r.w, r.h};
    float y = beginScroll(clip) + 30;

    gfx_.text(L"Reference boards", {x, y, w, 36}, Font::Title, pal_.ink, {Align::Left, false});
    gfx_.text(L"Pictures and colours you keep from pages. Right-click a picture on a page to save it here, or anywhere on a page to pick a colour.",
              {x, y + 38, w - 260, 20}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
    const std::wstring openF = L"Open the pictures folder";
    const float ow = gfx_.measureWidth(openF, Font::Body) + 28;
    button("ref:folder", {x + w - ow, y + 4, ow, 34}, openF, [this] {
        ShellExecuteW(hwnd(), L"open", referencesFolder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    });
    y += 76;

    // The boards, as chips; the one showing is filled.
    float bx = x;
    std::wstring current;
    for (const RefBoard& b : boards) {
        const std::wstring label = widen(b.name) + L"  " + std::to_wstring(b.pictures);
        const float bw = gfx_.measureWidth(label, Font::Small) + 26;
        if (bx + bw > x + w) { bx = x; y += 38; }
        const Rect br{bx, y, bw, 30};
        const std::string id = "ref:board:" + std::to_string(b.id);
        const bool on = b.id == refBoard_;
        if (on) current = widen(b.name);
        addHit(id, br, [this, bid = b.id] { finishRename(true); refBoard_ = bid; resetScroll(); invalidate(); });
        const float hv = hoverAmount(id);
        gfx_.fillRounded(br, 15, on ? pal_.ribbon.withAlpha(0.18f) : pal_.ink.withAlpha(0.04f + 0.05f * hv));
        gfx_.strokeRounded(br, 15, on ? pal_.ribbon : pal_.rule, on ? 1.4f : 1);
        gfx_.text(label, br, on ? Font::SmallStrong : Font::Small, pal_.ink, {Align::Center, false, true, true});
        if (focused(id)) focusRing(br);
        bx += bw + 8;
    }
    {
        const std::wstring nb = L"+ New board";
        const float nw = gfx_.measureWidth(nb, Font::Small) + 26;
        if (bx + nw > x + w) { bx = x; y += 38; }
        button("ref:new", {bx, y, nw, 30}, nb, [this] {
            for (int n = (int)refs_->boards().size() + 1; n < 1000; ++n)
                if (const auto id = refs_->addBoard("Board " + std::to_string(n))) {
                    refBoard_ = id;
                    renamingBoard_ = id;   // straight into naming it
                    SetWindowTextW(renameEdit_, (L"Board " + std::to_wstring(n)).c_str());
                    invalidate();
                    UpdateWindow(hwnd());
                    ShowWindow(renameEdit_, SW_SHOW);
                    SetFocus(renameEdit_);
                    SendMessageW(renameEdit_, EM_SETSEL, 0, -1);
                    break;
                }
        });
    }
    y += 50;

    // The board's name (or the field renaming it), and what can be done with it.
    if (renamingBoard_ == refBoard_ && renamingBoard_) {
        const Rect fr{x, y, std::min(360.0f, w), 34};
        gfx_.fillRounded(fr, 6, pal_.sunken);
        gfx_.strokeRounded(fr, 6, pal_.focus, 1.5f);
        renameRect_ = {fr.x + 10, fr.y + 7, fr.w - 20, 20};
        renameVisible_ = true;
    } else {
        gfx_.text(current, {x, y, w * 0.6f, 34}, Font::Heading, pal_.ink, {Align::Left, false, true, true});
        float ax = x + std::min(gfx_.measureWidth(current, Font::Heading), w * 0.6f) + 18;
        linkText("ref:rename", ax, y + 9, L"Rename", [this, current] {
            renamingBoard_ = refBoard_;
            SetWindowTextW(renameEdit_, current.c_str());
            invalidate();
            UpdateWindow(hwnd());
            ShowWindow(renameEdit_, SW_SHOW);
            SetFocus(renameEdit_);
            SendMessageW(renameEdit_, EM_SETSEL, 0, -1);
        });
        ax += gfx_.measureWidth(L"Rename", Font::Small) + 26;
        linkText("ref:delete", ax, y + 9, L"Delete board", [this, current] {
            const std::wstring ask = L"Delete the board “" + current + L"” with its pictures and colours?\n\nThis can't be undone.";
            if (cjui::messageBox(hwnd(), ask.c_str(), L"Delete board", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) return;
            for (const std::string& f : refs_->removeBoard(refBoard_)) DeleteFileW((referencesFolder() + L"\\" + widen(f)).c_str());
            refBoard_ = refs_->firstBoard();
            invalidate();
        });
    }
    y += 50;

    // Colours.
    const auto colours = refs_->colours(refBoard_);
    if (!colours.empty()) {
        gfx_.text(L"COLOURS", {x, y, w, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
        y += 26;
        float cx = x;
        for (const RefColour& c : colours) {
            const float sw = 64;
            if (cx + sw > x + w) { cx = x; y += 96; }
            const Rect sr{cx, y, sw, sw};
            const std::string id = "ref:colour:" + std::to_string(c.id);
            const std::wstring hex = widen(hexColour(c.rgb));
            addHit(id, {sr.x, sr.y, sr.w, sr.h + 26}, [this, hex] { copyText(hwnd(), hex); flash(L"Copied " + hex + L".", 3); });
            const float hv = hoverAmount(id);
            gfx_.fillRounded(sr, 10, rgbColor(c.rgb));
            gfx_.strokeRounded(sr, 10, pal_.rule.mix(pal_.ink, 0.4f * hv), 1);
            gfx_.text(hex, {sr.x - 6, sr.bottom() + 4, sr.w + 12, 18}, Font::Small, pal_.inkMuted, {Align::Center, false, true});
            if (hv > 0.01f) {
                const Rect xb{sr.right() - 22, sr.y + 4, 18, 18};
                addHit("ref:uncolour:" + std::to_string(c.id), xb, [this, cid = c.id] { refs_->removeColour(cid); invalidate(); }, false);
                gfx_.fillRounded(xb, 9, pal_.paper.withAlpha(0.85f));
                gfx_.line(xb.x + 5.5f, xb.y + 5.5f, xb.right() - 5.5f, xb.bottom() - 5.5f, pal_.ink, 1.3f);
                gfx_.line(xb.right() - 5.5f, xb.y + 5.5f, xb.x + 5.5f, xb.bottom() - 5.5f, pal_.ink, 1.3f);
            }
            if (focused(id)) focusRing(sr);
            cx += sw + 14;
        }
        y += 104;
    }

    // Pictures, in columns, each placed under the shortest.
    const auto pics = refs_->pictures(refBoard_);
    if (pics.empty())
        y += gfx_.text(L"No pictures on this board yet. On any page, right-click a picture and choose “Save picture to a reference board”.",
                       {x, y, w, 60}, Font::Body, pal_.inkMuted) + 16;
    const int cols = std::max(1, (int)((w + 16) / 236));
    const float gap = 16, colW = (w - gap * (cols - 1)) / cols;
    std::vector<float> colY((size_t)cols, y);
    for (const RefPicture& p : pics) {
        const size_t c = (size_t)(std::min_element(colY.begin(), colY.end()) - colY.begin());
        const float ph = std::clamp(p.width > 0 ? colW * p.height / p.width : colW, 60.0f, colW * 2.2f);
        const Rect pr{x + c * (colW + gap), colY[c], colW, ph};
        colY[c] += ph + gap;
        if (pr.bottom() < clip.y || pr.y > clip.bottom()) continue;   // only what's on screen
        const std::string id = "ref:pic:" + std::to_string(p.id);
        addHit(id, pr, [this, page = p.page, src = p.source] {
            const std::string open = !page.empty() ? page : src;
            if (!open.empty()) newTab(widen(open), true, true);
        });
        const float hv = hoverAmount(id);
        gfx_.fillRounded(pr, 8, pal_.sunken);
        gfx_.pushClip(pr);
        gfx_.drawImage(referencesFolder() + L"\\" + widen(p.file), pr, true);
        gfx_.popClip();
        if (hv > 0.01f) {
            // The page it came from, and what can be done with it.
            const Rect bar{pr.x, pr.bottom() - 58, pr.w, 58};
            gfx_.fill(bar, Color{0, 0, 0, 0.62f * hv});
            const std::wstring from = widen(p.title.empty() ? hostOf(p.page) : p.title);
            gfx_.text(from, {bar.x + 10, bar.y + 6, bar.w - 20, 18}, Font::Small, Color{1, 1, 1, hv}, {Align::Left, false, true});
            float ax = bar.x + 10;
            auto action = [&](const std::string& aid, const std::wstring& label, std::function<void()> fn) {
                linkText(aid, ax + 7, bar.y + 29, label, std::move(fn));
                ax += gfx_.measureWidth(label, Font::Small) + 24;
            };
            const std::string k = std::to_string(p.id);
            action("ref:colours:" + k, L"Colours", [this, pid = p.id] { addMainColours(pid); });
            if (refs_->boards().size() > 1)
                action("ref:move:" + k, L"Move", [this, pid = p.id] {
                    std::vector<MenuItem> items;
                    const auto bs = refs_->boards();
                    for (size_t i = 0; i < bs.size(); ++i) items.push_back(MenuItem::item(widen(bs[i].name), 1 + (UINT)i, {}, bs[i].id != refBoard_));
                    POINT pt;
                    GetCursorPos(&pt);
                    const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
                    if (cmd && refs_->movePicture(pid, bs[cmd - 1].id)) invalidate();
                });
            action("ref:remove:" + k, L"Remove", [this, pid = p.id] {
                const std::string f = refs_->removePicture(pid);
                if (!f.empty()) DeleteFileW((referencesFolder() + L"\\" + widen(f)).c_str());
                invalidate();
            });
        }
        if (focused(id)) focusRing(pr);
    }
    y = std::max(y, *std::max_element(colY.begin(), colY.end())) + 30;
    endScroll(y - (clip.y - 0) + 0);
}

// ------------------------------------------------------------------ colour picker

void BrowserWindow::startColourPick() {
    TabView* v = activeView();
    if (!v || !v->page || v->showStart || !v->page->ready()) { flash(L"Open a page first, then pick a colour from it.", 5); return; }
    const std::wstring page = v->page->url();
    v->page->capture([this, page](int w, int h, std::vector<unsigned char> rgba) {
        if (rgba.empty()) { flash(L"The page couldn't be captured to pick from.", 6); return; }
        gfx_.forgetImage(L"pick:" + std::to_wstring(pick_.gen));
        pick_.on = true;
        pick_.w = w;
        pick_.h = h;
        pick_.rgba = std::move(rgba);
        pick_.page = page;
        ++pick_.gen;
        pick_.mx = pick_.my = -1;
        invalidate();
        flash(L"Click a colour to copy it. Esc to stop.", 5);
    });
}

void BrowserWindow::drawColourPick(const Rect& r) {
    gfx_.fill(r, pal_.paper);
    pick_.shown = r;
    gfx_.drawPixels(L"pick:" + std::to_wstring(pick_.gen), pick_.w, pick_.h, pick_.rgba.data(), r, false);
    if (!r.contains(pick_.mx, pick_.my)) return;
    const int px = std::clamp((int)((pick_.mx - r.x) / r.w * pick_.w), 0, pick_.w - 1);
    const int py = std::clamp((int)((pick_.my - r.y) / r.h * pick_.h), 0, pick_.h - 1);
    // The loupe: 11 x 11 pixels around the pointer, enlarged, with the one under it outlined.
    const int n = 11;
    const float cell = 11, size = n * cell;
    float lx = pick_.mx + 24, ly = pick_.my + 24;
    if (lx + size > r.right()) lx = pick_.mx - 24 - size;
    if (ly + size + 30 > r.bottom()) ly = pick_.my - 24 - size - 30;
    const Rect loupe{lx, ly, size, size};
    gfx_.fillRounded(loupe.inset(-4), 8, pal_.paper);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const int sx = std::clamp(px + i - n / 2, 0, pick_.w - 1), sy = std::clamp(py + j - n / 2, 0, pick_.h - 1);
            const unsigned char* c = &pick_.rgba[((size_t)sy * pick_.w + sx) * 4];
            gfx_.fill({lx + i * cell, ly + j * cell, cell, cell}, Color{c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, 1});
        }
    gfx_.stroke({lx + (n / 2) * cell, ly + (n / 2) * cell, cell, cell}, pal_.paper, 2);
    gfx_.stroke({lx + (n / 2) * cell - 1, ly + (n / 2) * cell - 1, cell + 2, cell + 2}, pal_.ink, 1);
    gfx_.strokeRounded(loupe.inset(-4), 8, pal_.rule, 1);
    const unsigned char* c = &pick_.rgba[((size_t)py * pick_.w + px) * 4];
    const std::uint32_t rgb = ((std::uint32_t)c[0] << 16) | ((std::uint32_t)c[1] << 8) | c[2];
    const Rect lab{lx - 4, ly + size + 8, size + 8, 26};
    gfx_.fillRounded(lab, 6, pal_.paper);
    gfx_.strokeRounded(lab, 6, pal_.rule, 1);
    gfx_.fillRounded({lab.x + 6, lab.y + 6, 14, 14}, 3, rgbColor(rgb));
    gfx_.text(widen(hexColour(rgb)), {lab.x + 26, lab.y, lab.w - 30, lab.h}, Font::SmallStrong, pal_.ink, {Align::Left, false, false, true});
}

void BrowserWindow::finishColourPick(bool take) {
    if (!pick_.on) return;
    if (take && pick_.shown.contains(pick_.mx, pick_.my) && !pick_.rgba.empty()) {
        const Rect& r = pick_.shown;
        const int px = std::clamp((int)((pick_.mx - r.x) / r.w * pick_.w), 0, pick_.w - 1);
        const int py = std::clamp((int)((pick_.my - r.y) / r.h * pick_.h), 0, pick_.h - 1);
        const unsigned char* c = &pick_.rgba[((size_t)py * pick_.w + px) * 4];
        const std::uint32_t rgb = ((std::uint32_t)c[0] << 16) | ((std::uint32_t)c[1] << 8) | c[2];
        const std::wstring hex = widen(hexColour(rgb));
        copyText(hwnd(), hex);
        if (refs_) refs_->addColour({0, refBoard_ ? refBoard_ : refs_->firstBoard(), rgb, narrow(pick_.page), 0});
        flash(L"Copied " + hex + L" and kept it on your reference board.", 5);
    }
    gfx_.forgetImage(L"pick:" + std::to_wstring(pick_.gen));
    pick_ = ColourPick{false, 0, 0, pick_.gen, {}, {}, -1, -1, {}};
    invalidate();
}

}  // namespace nrb
