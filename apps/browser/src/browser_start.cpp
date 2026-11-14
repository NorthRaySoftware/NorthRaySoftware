// NorthRay Browser's start page (a new tab, and Home): a greeting on the night
// sky with a search field, your bookmarks bar as tiles, recently closed tabs,
// and a few keyboard shortcuts. Everything on it is drawn locally; the only
// thing it fetches is the weather, and only once a place has been chosen for it.
#include "browser_window.hpp"

#include "nrb/url.hpp"

#include "cjui/dialog.hpp"
#include "cjui/northray.hpp"

#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winhttp.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <sstream>
#include <thread>

namespace nrb {

using cjui::Align;
using cjui::Color;
using cjui::Font;
using cjui::MenuItem;
using cjui::Rect;
namespace nr = cjui::northray;

namespace {

std::wstring greeting() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    if (t.wHour >= 5 && t.wHour < 12) return L"Good morning";
    if (t.wHour >= 12 && t.wHour < 18) return L"Good afternoon";
    return L"Good evening";
}

std::wstring today() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t buf[96] = L"";
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &t, nullptr, buf, (int)std::size(buf), nullptr);
    return buf;
}

}  // namespace

void BrowserWindow::drawStartPage(const Rect& r) {
    gfx_.fill(r, pal_.paper);
    tileRects_.clear();
    rowRects_.clear();
    const float pad = r.w > 900 ? 44 : 24;
    const float w = std::min(r.w - pad * 2, 900.0f);
    const float x = r.x + (r.w - w) / 2;
    float y = r.y + std::clamp(r.h * 0.06f, 20.0f, 48.0f);

    // Asked once: being the default browser.
    if (!settings_.defaultOffered && !isDefaultBrowser()) {
        const Rect card{x, y, w, 52};
        gfx_.fillRounded(card, 10, pal_.surface);
        gfx_.strokeRounded(card, 10, pal_.rule, 1);
        gfx_.text(L"Open links from other apps in NorthRay Browser?", {card.x + 18, card.y, card.w - 330, card.h}, Font::Body, pal_.ink,
                  {Align::Left, false, true, true});
        const std::wstring yes = L"Make it the default\u2026", no = L"Not now";
        const float nw = gfx_.measureWidth(no, Font::Small) + 26, yw = gfx_.measureWidth(yes, Font::Small) + 26;
        button("start:nodefault", {card.right() - 12 - nw, card.y + 11, nw, 30}, no, [this] {
            settings_.defaultOffered = true;
            settings_.save(settingsPath_);
            flash(L"It won't ask again. The menu has \u201CMake it the default browser\u201D if you change your mind.", 6);
            invalidate();
        });
        button("start:default", {card.right() - 20 - nw - yw, card.y + 11, yw, 30}, yes, [this] { makeDefaultBrowser(); }, true);
        y = card.bottom() + 16;
    }

    // The night (or your own picture): greeting, date, clock, weather and the search field.
    const float heroH = 214;
    const Rect hero{x, y, w, heroH};
    bool picture = false;
    if (!settings_.startBackground.empty()) {
        const std::wstring bg = userFolder() + L"\\" + widen(settings_.startBackground);
        gfx_.pushClip(hero);
        picture = gfx_.drawImage(bg, hero, true);
        if (picture) {
            // Darkened towards the words so they read on any picture.
            gfx_.fillGradient(hero, 0, Color{0.03f, 0.05f, 0.10f, 0.62f}, Color{0.03f, 0.05f, 0.10f, 0.30f}, true);
        }
        gfx_.popClip();
    }
    if (!picture) nr::nightSky(gfx_, hero, 14, 1.0f, 7, pal_.paper);
    gfx_.text(greeting(), {hero.x + 36, hero.y + 34, hero.w - 72, 40}, Font::Title, nr::kNightInk, {Align::Left, false});
    gfx_.text(today(), {hero.x + 36, hero.y + 74, hero.w - 72, 22}, Font::Body, nr::kNightMuted, {Align::Left, false});
    if (settings_.showClock) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t clock[8];
        swprintf(clock, 8, L"%02d:%02d", t.wHour, t.wMinute);
        gfx_.text(clock, {hero.right() - 236, hero.y + 30, 200, 44}, Font::Title, nr::kNightInk, {Align::Right, false});
    }
    // The weather, if a place is chosen.
    if (!settings_.weatherPlace.empty()) {
        refreshWeather(false);
        std::wstring line = weather_.ok ? widen(temperatureText(weather_.temperature, settings_.fahrenheit)) + L"  " + widen(weather_.words())
                                        : weatherBusy_ ? std::wstring(L"Getting the weather\u2026") : std::wstring(L"Weather not available");
        gfx_.text(line, {hero.right() - 336, hero.y + 76, 300, 22}, Font::BodyStrong, nr::kNightInk, {Align::Right, false, true});
        std::wstring place = widen(settings_.weatherPlace.substr(0, settings_.weatherPlace.find(',')));
        if (weather_.ok)
            place += L"  \u00B7  H " + widen(temperatureText(weather_.high, settings_.fahrenheit)) + L"  L " +
                     widen(temperatureText(weather_.low, settings_.fahrenheit));
        gfx_.text(place, {hero.right() - 336, hero.y + 98, 300, 18}, Font::Small, nr::kNightMuted, {Align::Right, false, true});
    }
    {
        // Small links along the foot: who the weather is from, and changing the page.
        const std::wstring cust = L"Customise\u2026";
        const float cw = gfx_.measureWidth(cust, Font::Small);
        linkText("start:customise", hero.right() - 43 - cw, hero.y + 184, cust, [this] {
            POINT pt;
            GetCursorPos(&pt);
            customiseMenu(pt);
        });
        if (!settings_.weatherPlace.empty())
            linkText("start:openmeteo", hero.x + 43, hero.y + 184, L"Weather data by Open-Meteo.com", [this] { newTab(L"https://open-meteo.com/", true, true); });
    }
    if (weatherSetup_) {
        // Typing the place's name, in place of the search field.
        const Rect f{hero.x + 36, hero.y + 120, hero.w - 72, 50};
        gfx_.fillRounded(f, 12, pal_.sunken);
        gfx_.strokeRounded(f, 12, nr::kAuroraGreen, 2);
        renameRect_ = {f.x + 18, f.y + (f.h - 22) / 2, f.w - 36, 22};
        renameVisible_ = true;
        y = hero.bottom() + 34;
    } else {
    const Rect field{hero.x + 36, hero.y + 120, hero.w - 72, 50};
    const bool focusedField = GetFocus() == startSearch_;
    gfx_.fillRounded(field, 12, pal_.sunken);
    if (focusedField) gfx_.strokeRounded(field, 12, nr::kAuroraGreen, 2);
    gfx_.circle(field.x + 24, field.y + 23, 7, pal_.inkMuted, false, 1.8f);
    gfx_.line(field.x + 29, field.y + 28, field.x + 34, field.y + 33, pal_.inkMuted, 1.8f);
    startSearchRect_ = {field.x + 48, field.y + (field.h - 22) / 2, field.w - 64, 22};
    startSearchVisible_ = true;
    y = hero.bottom() + 34;
    }

    drawStartTiles({x, y, w, r.bottom() - y}, y);

    // Notes, kept on this computer.
    if (settings_.showNotes && y + 150 < r.bottom()) {
        y += sectionLabel(x, y, w, L"NOTES") + 4;
        const Rect card{x, y, w, 120};
        gfx_.fillRounded(card, 10, pal_.surface);
        gfx_.strokeRounded(card, 10, GetFocus() == notesEdit_ ? pal_.focus : pal_.rule, GetFocus() == notesEdit_ ? 1.5f : 1);
        notesRect_ = {card.x + 14, card.y + 12, card.w - 28, card.h - 24};
        const bool empty = GetFocus() != notesEdit_ && GetWindowTextLengthW(notesEdit_) == 0 && !notesActive_;
        notesVisible_ = !empty;
        if (empty) {
            addHit("start:notes", card, [this] {
                notesActive_ = true;
                invalidate();
                UpdateWindow(hwnd());
                SetFocus(notesEdit_);
            });
            gfx_.text(L"Notes to yourself: a to-do, a word to look up, an idea. Kept on this computer.", {card.x + 18, card.y + 12, card.w - 36, 20},
                      Font::Body, pal_.inkFaint, {Align::Left, false, true});
        }
        y = card.bottom() + 30;
    }

    // Recently closed tabs, if any.
    const auto& closed = model_.closed();
    if (!closed.empty() && y + 90 < r.bottom()) {
        y += sectionLabel(x, y, w, L"RECENTLY CLOSED") + 4;
        int shown = 0;
        for (const ClosedTab& c : closed) {
            if (shown >= 5 || y + 34 > r.bottom() - 40) break;
            const Rect row{x, y, w, 34};
            const std::string id = "start:closed:" + std::to_string(shown);
            const std::wstring url = widen(c.url);
            addHit(id, row, [this, url] {
                if (TabView* v = activeView()) { navigateTab(*v, url); focusPage(); }
            });
            const float hv = hoverAmount(id);
            if (hv > 0.01f) gfx_.fillRounded(row, 6, pal_.ink.withAlpha(0.05f * hv));
            const std::wstring title = c.title.empty() ? widen(hostOf(c.url)) : widen(c.title);
            gfx_.text(title, {row.x + 10, row.y, row.w * 0.55f, row.h}, Font::Body, pal_.ink, {Align::Left, false, true, true});
            gfx_.text(widen(displayUrl(c.url)), {row.x + row.w * 0.55f + 16, row.y, row.w * 0.45f - 26, row.h}, Font::Small,
                      pal_.inkMuted, {Align::Right, false, true, true});
            if (focused(id)) focusRing(row);
            y += 36;
            ++shown;
        }
        y += 18;
    }

    // A few keys worth knowing, quietly at the foot.
    if (y + 30 < r.bottom()) {
        const std::wstring tips = L"Ctrl+L  address bar     Ctrl+T  new tab     Ctrl+D  bookmark this page     "
                                  L"Ctrl+Shift+B  bookmarks bar     Ctrl+Shift+T  reopen a closed tab";
        gfx_.text(tips, {x, std::max(y, r.bottom() - 44), w, 20}, Font::Small, pal_.inkFaint, {Align::Center, false, true});
    }
}

// The bookmarks bar as tiles: the quickest way back to the sites you use.
void BrowserWindow::drawStartTiles(const Rect& area, float& y) {
    const float x = area.x, w = area.w;
    const float labelH = sectionLabel(x, y, w, L"BOOKMARKS");
    if (bookmarks_) {
        const std::wstring all = L"All bookmarks";
        linkText("start:allbm", x + w - gfx_.measureWidth(all, Font::Small), y - 2, all, [this] { showBookmarksPage(); });
    }
    y += labelH + 8;
    std::vector<Bookmark> items;
    if (bookmarks_)
        for (const Bookmark& b : bookmarks_->children(BookmarkStore::kBar))
            if (!b.folder) items.push_back(b);
    if (items.empty()) {
        const Rect card{x, y, w, 84};
        gfx_.fillRounded(card, 10, pal_.surface);
        gfx_.strokeRounded(card, 10, pal_.rule, 1);
        drawStar(card.x + 38, card.y + 42, 13, pal_.ribbon, false);
        gfx_.text(L"Your bookmarks appear here", {card.x + 70, card.y + 18, card.w - 90, 22}, Font::BodyStrong, pal_.ink,
                  {Align::Left, false});
        gfx_.text(L"Open a page and click the star in the address bar, or press Ctrl+D.", {card.x + 70, card.y + 42, card.w - 300, 22},
                  Font::Small, pal_.inkMuted, {Align::Left, false, true});
        if (bookmarks_) {
            const std::wstring imp = L"Import from another browser";
            linkText("start:import", card.right() - 20 - gfx_.measureWidth(imp, Font::Small), card.y + 44, imp,
                     [this] { importBookmarks(); });
        }
        y = card.bottom() + 30;
        return;
    }
    const float tileW = 112, tileH = 100, gap = 12;
    const int cols = std::max(1, (int)((w + gap) / (tileW + gap)));
    const int rows = (int)items.size() > cols ? 2 : 1;
    const size_t maxTiles = (size_t)cols * (size_t)rows;
    const float rowW = std::min((float)std::min(items.size(), (size_t)cols), (float)cols) * (tileW + gap) - gap;
    const float x0 = x + (w - rowW) / 2;
    for (size_t i = 0; i < items.size() && i < maxTiles; ++i) {
        const Bookmark& b = items[i];
        const Rect t{x0 + (float)(i % (size_t)cols) * (tileW + gap), y + (float)(i / (size_t)cols) * (tileH + gap), tileW, tileH};
        const std::string id = "start:bm:" + std::to_string(b.id);
        addHit(id, t, [this, b] { openBookmark(b, (GetKeyState(VK_CONTROL) & 0x8000) != 0); });
        tileRects_.push_back({b.id, t});
        const float hv = hoverAmount(id);
        softShadow(t, 10, hv);
        gfx_.fillRounded({t.x, t.y - 2 * hv, t.w, t.h}, 10, pal_.surface);
        gfx_.strokeRounded({t.x, t.y - 2 * hv, t.w, t.h}, 10, pal_.rule.mix(pal_.inkFaint, 0.3f * hv), 1);
        const float iy = t.y + 18 - 2 * hv;
        drawBookmarkIcon(b, {t.x + (t.w - 32) / 2, iy, 32, 32});
        gfx_.text(widen(b.title), {t.x + 8, iy + 42, t.w - 16, 36}, Font::Small, pal_.ink, {Align::Center, true, true});
        if (focused(id)) focusRing(t);
    }
    y += (float)rows * (tileH + gap) + 22;
}

// ------------------------------------------------------------------ the extras

std::wstring BrowserWindow::userFolder() const {
    return settingsPath_.substr(0, settingsPath_.find_last_of(L"\\/")) + L"\\User";
}

void BrowserWindow::loadNotes() {
    std::ifstream in((userFolder() + L"\\start-notes.txt").c_str(), std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    std::wstring t = widen(ss.str());
    // The edit control wants Windows line ends.
    std::wstring crlf;
    for (wchar_t c : t) {
        if (c == L'\r') continue;
        if (c == L'\n') crlf += L"\r\n";
        else crlf += c;
    }
    SetWindowTextW(notesEdit_, crlf.c_str());
}

void BrowserWindow::saveNotes() {
    KillTimer(hwnd(), 5);
    const int n = GetWindowTextLengthW(notesEdit_);
    std::wstring t((size_t)n, L'\0');
    if (n) GetWindowTextW(notesEdit_, t.data(), n + 1);
    CreateDirectoryW(userFolder().c_str(), nullptr);
    const std::wstring path = userFolder() + L"\\start-notes.txt", tmp = path + L".new";
    {
        std::ofstream out(tmp.c_str(), std::ios::binary | std::ios::trunc);
        out << narrow(t);
        if (!out) return;
    }
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// ---- fetching (the weather): WinHTTP on its own thread, the answer posted back

static std::wstring fieldText(HWND e) {
    const int n = GetWindowTextLengthW(e);
    std::wstring t((size_t)n, L'\0');
    if (n) GetWindowTextW(e, t.data(), n + 1);
    return t;
}

struct BrowserWindow::Fetch {
    std::wstring url;
    std::function<void(bool, const std::string&)> done;
    bool ok = false;
    std::string body;
};

void BrowserWindow::fetchText(const std::wstring& url, std::function<void(bool ok, const std::string& body)> done) {
    auto* f = new Fetch{url, std::move(done), false, {}};
    const HWND target = hwnd();
    std::thread([f, target] {
        URL_COMPONENTS u{sizeof u};
        wchar_t host[256] = L"", path[2048] = L"";
        u.lpszHostName = host;
        u.dwHostNameLength = 256;
        u.lpszUrlPath = path;
        u.dwUrlPathLength = 2048;
        wchar_t extra[2048] = L"";
        u.lpszExtraInfo = extra;
        u.dwExtraInfoLength = 2048;
        if (WinHttpCrackUrl(f->url.c_str(), 0, 0, &u) && u.nScheme == INTERNET_SCHEME_HTTPS) {
            if (HINTERNET s = WinHttpOpen(L"NorthRayBrowser", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)) {
                WinHttpSetTimeouts(s, 8000, 8000, 8000, 10000);
                if (HINTERNET c = WinHttpConnect(s, host, u.nPort, 0)) {
                    const std::wstring obj = std::wstring(path) + extra;
                    if (HINTERNET r = WinHttpOpenRequest(c, L"GET", obj.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)) {
                        if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
                            DWORD status = 0, len = sizeof status;
                            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &len, nullptr);
                            for (DWORD avail = 0; WinHttpQueryDataAvailable(r, &avail) && avail && f->body.size() < 2 * 1024 * 1024;) {
                                std::string chunk(avail, '\0');
                                DWORD got = 0;
                                if (!WinHttpReadData(r, chunk.data(), avail, &got) || !got) break;
                                f->body.append(chunk.data(), got);
                            }
                            f->ok = status == 200;
                        }
                        WinHttpCloseHandle(r);
                    }
                    WinHttpCloseHandle(c);
                }
                WinHttpCloseHandle(s);
            }
        }
        if (!PostMessageW(target, WM_APP + 22, 0, reinterpret_cast<LPARAM>(f))) delete f;
    }).detach();
}

void BrowserWindow::fetched(Fetch* raw) {
    std::unique_ptr<Fetch> f(raw);
    if (f->done) f->done(f->ok, f->body);
}

void BrowserWindow::refreshWeather(bool force) {
    if (settings_.weatherPlace.empty() || weatherBusy_) return;
    const std::int64_t now = (std::int64_t)std::time(nullptr);
    if (!force && weatherAt_ && now - weatherAt_ < 30 * 60) return;
    weatherBusy_ = true;
    weatherAt_ = now;
    Place p;
    p.latitude = settings_.weatherLat;
    p.longitude = settings_.weatherLon;
    fetchText(widen(forecastUrl(p)), [this](bool ok, const std::string& body) {
        weatherBusy_ = false;
        const Weather w = ok ? parseForecast(body) : Weather{};
        if (w.ok) weather_ = w;
        else weatherAt_ = (std::int64_t)std::time(nullptr) - 25 * 60;   // try again in five minutes
        invalidate();
    });
}

void BrowserWindow::startWeatherSetup() {
    weatherSetup_ = true;
    SetWindowTextW(renameEdit_, L"");
    SendMessageW(renameEdit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Type your town or city, then press Enter"));
    invalidate();
    UpdateWindow(hwnd());
    ShowWindow(renameEdit_, SW_SHOW);
    SetFocus(renameEdit_);
}

void BrowserWindow::finishWeatherSetup(bool commit) {
    weatherSetup_ = false;
    const std::wstring name = fieldText(renameEdit_);
    SendMessageW(renameEdit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L""));
    ShowWindow(renameEdit_, SW_HIDE);
    if (GetFocus() == renameEdit_) SetFocus(hwnd());
    invalidate();
    if (!commit || name.find_first_not_of(L' ') == std::wstring::npos) return;
    flash(L"Looking for " + name + L"…", 3);
    fetchText(widen(placeSearchUrl(narrow(name))), [this, name](bool ok, const std::string& body) {
        const auto places = ok ? parsePlaces(body) : std::vector<Place>{};
        if (places.empty()) {
            flash(ok ? L"No place called " + name + L" was found." : L"The weather service couldn't be reached.", 6);
            return;
        }
        // Which one (there are many Newports).
        std::vector<MenuItem> items{MenuItem::item(L"Which " + name + L"?", 0, {}, false), MenuItem::sep()};
        for (size_t i = 0; i < places.size(); ++i) items.push_back(MenuItem::item(widen(places[i].label()), 1 + (UINT)i));
        POINT pt;
        GetCursorPos(&pt);
        const UINT cmd = places.size() == 1 ? 1 : cjui::showMenu(hwnd(), items, pt, pal_);
        if (!cmd) return;
        const Place& p = places[cmd - 1];
        settings_.weatherPlace = p.label();
        settings_.weatherLat = p.latitude;
        settings_.weatherLon = p.longitude;
        settings_.save(settingsPath_);
        weather_ = {};
        refreshWeather(true);
        invalidate();
    });
}

void BrowserWindow::chooseStartBackground() {
    wchar_t file[MAX_PATH * 2] = L"";
    OPENFILENAMEW ofn{sizeof ofn};
    ofn.hwndOwner = hwnd();
    ofn.lpstrFilter = L"Pictures (*.jpg, *.png, *.webp, *.gif, *.bmp)\0*.jpg;*.jpeg;*.png;*.webp;*.gif;*.bmp\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = (DWORD)std::size(file);
    ofn.lpstrTitle = L"Choose a picture for the start page";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;
    if (!gfx_.imageSize(file).first) { flash(L"That file couldn't be read as a picture.", 6); return; }
    std::wstring ext = file;
    ext = ext.substr(ext.find_last_of(L'.'));
    for (auto& c : ext) c = (wchar_t)towlower(c);
    // A copy is kept, so the page still works if the original moves. A new name each time (pictures are cached by name).
    const std::wstring name = L"start-background-" + std::to_wstring((long long)std::time(nullptr)) + ext;
    CreateDirectoryW(userFolder().c_str(), nullptr);
    if (!CopyFileW(file, (userFolder() + L"\\" + name).c_str(), FALSE)) { flash(L"The picture couldn't be copied.", 6); return; }
    if (!settings_.startBackground.empty()) DeleteFileW((userFolder() + L"\\" + widen(settings_.startBackground)).c_str());
    settings_.startBackground = narrow(name);
    settings_.save(settingsPath_);
    invalidate();
}

void BrowserWindow::customiseMenu(POINT screen) {
    enum : UINT { Picture = 1, Night, Clock, Notes, Place, Fahrenheit, NoWeather };
    std::vector<MenuItem> weather;
    weather.push_back(MenuItem::item(settings_.weatherPlace.empty() ? L"Show the weather for…" : L"Change the place…", Place));
    if (!settings_.weatherPlace.empty()) {
        weather.push_back(MenuItem::item(L"In °F", Fahrenheit, {}, true, settings_.fahrenheit));
        weather.push_back(MenuItem::sep());
        weather.push_back(MenuItem::item(L"Don't show the weather", NoWeather));
    }
    const std::vector<MenuItem> items = {
        MenuItem::item(L"Background picture…", Picture),
        MenuItem::item(L"Night sky", Night, {}, true, settings_.startBackground.empty()),
        MenuItem::sep(),
        MenuItem::item(L"Clock", Clock, {}, true, settings_.showClock),
        MenuItem::item(L"Notes", Notes, {}, true, settings_.showNotes),
        MenuItem::sub(L"Weather", weather),
    };
    const UINT cmd = cjui::showMenu(hwnd(), items, screen, pal_, cjui::MenuAnchor::TopRight);
    switch (cmd) {
        case Picture: chooseStartBackground(); return;
        case Night:
            if (!settings_.startBackground.empty()) DeleteFileW((userFolder() + L"\\" + widen(settings_.startBackground)).c_str());
            settings_.startBackground.clear();
            break;
        case Clock: settings_.showClock = !settings_.showClock; break;
        case Notes: settings_.showNotes = !settings_.showNotes; break;
        case Place: startWeatherSetup(); return;
        case Fahrenheit: settings_.fahrenheit = !settings_.fahrenheit; break;
        case NoWeather: settings_.weatherPlace.clear(); weather_ = {}; break;
        default: return;
    }
    settings_.save(settingsPath_);
    invalidate();
}

// ---- the default browser

static std::wstring exePath() {
    wchar_t buf[MAX_PATH * 2] = L"";
    GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    return buf;
}

bool BrowserWindow::isDefaultBrowser() const {
    static int cached = -1;   // asked once a run (it doesn't change behind the page's back often)
    static std::int64_t at = 0;
    const std::int64_t now = (std::int64_t)GetTickCount64();
    if (cached >= 0 && now - at < 60000) return cached == 1;
    at = now;
    cached = 0;
    IApplicationAssociationRegistration* reg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ApplicationAssociationRegistration, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&reg))) && reg) {
        LPWSTR prog = nullptr;
        if (SUCCEEDED(reg->QueryCurrentDefault(L"https", AT_URLPROTOCOL, AL_EFFECTIVE, &prog)) && prog) {
            cached = std::wstring(prog) == L"NorthRayBrowserURL" ? 1 : 0;
            CoTaskMemFree(prog);
        }
        reg->Release();
    }
    return cached == 1;
}

static bool setKey(const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    const LONG r = RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

// Registers the browser with Windows (for this account only) as one that can open web links and
// pages, then opens Windows' own Default apps page for it: Windows lets only the person choose.
void BrowserWindow::makeDefaultBrowser() {
    const std::wstring exe = exePath(), open = L"\"" + exe + L"\" \"%1\"", icon = exe + L",0";
    const std::wstring app = L"Software\\Clients\\StartMenuInternet\\NorthRay Browser";
    bool ok = true;
    for (const wchar_t* prog : {L"NorthRayBrowserURL", L"NorthRayBrowserHTML"}) {
        const std::wstring base = std::wstring(L"Software\\Classes\\") + prog;
        ok &= setKey(base, nullptr, std::wstring(prog) == L"NorthRayBrowserURL" ? L"NorthRay Browser link" : L"NorthRay Browser page");
        ok &= setKey(base + L"\\DefaultIcon", nullptr, icon);
        ok &= setKey(base + L"\\shell\\open\\command", nullptr, open);
        ok &= setKey(base + L"\\Application", L"ApplicationName", L"NorthRay Browser");
    }
    ok &= setKey(app, nullptr, L"NorthRay Browser");
    ok &= setKey(app + L"\\DefaultIcon", nullptr, icon);
    ok &= setKey(app + L"\\shell\\open\\command", nullptr, L"\"" + exe + L"\"");
    ok &= setKey(app + L"\\Capabilities", L"ApplicationName", L"NorthRay Browser");
    ok &= setKey(app + L"\\Capabilities", L"ApplicationDescription", L"A web browser with creative tools, from NorthRay Software.");
    ok &= setKey(app + L"\\Capabilities", L"ApplicationIcon", icon);
    ok &= setKey(app + L"\\Capabilities\\URLAssociations", L"http", L"NorthRayBrowserURL");
    ok &= setKey(app + L"\\Capabilities\\URLAssociations", L"https", L"NorthRayBrowserURL");
    for (const wchar_t* ext : {L".htm", L".html", L".xhtml", L".svg", L".pdf"})
        ok &= setKey(app + L"\\Capabilities\\FileAssociations", ext, L"NorthRayBrowserHTML");
    ok &= setKey(app + L"\\Capabilities\\StartMenu", L"StartMenuInternet", L"NorthRay Browser");
    ok &= setKey(L"Software\\RegisteredApplications", L"NorthRay Browser", app + L"\\Capabilities");
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    settings_.defaultOffered = true;
    settings_.save(settingsPath_);
    if (!ok) { flash(L"Windows didn't let the browser register itself.", 8); return; }
    ShellExecuteW(hwnd(), L"open", L"ms-settings:defaultapps?registeredAppUser=NorthRay%20Browser", nullptr, nullptr, SW_SHOWNORMAL);
    cjui::messageBox(hwnd(),
                     L"Windows' Default apps settings are open at NorthRay Browser. Choose “Set default” there (or pick it for "
                     L"HTTP and HTTPS).\n\nWindows only lets you make this choice yourself, so no app can change it behind your back.",
                     L"Default browser", MB_OK | MB_ICONINFORMATION);
    invalidate();
}

}  // namespace nrb
