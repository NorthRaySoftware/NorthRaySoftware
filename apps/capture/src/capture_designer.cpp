// NorthRay Capture's window: the overlay designer (Milestone 10). A design is a source; this page
// edits it: templates to start from, a canvas where elements are picked, dragged (snapping to the page
// and to each other, with guides) and resized, a list of layers, and every element's properties.
// The canvas is drawn by the same renderer as the live source and PNG export (video/design_render).
#include "capture_window.hpp"

#include "log.hpp"
#include "video/design_render.hpp"

#include "ncap/design.hpp"
#include "ncap/gif.hpp"
#include "ncap/sourceutil.hpp"

#include "cjui/dialog.hpp"
#include "cjui/menu.hpp"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ncap {

using cjui::Align;
using cjui::Color;
using cjui::Font;
using cjui::MenuItem;
using cjui::narrow;
using cjui::Rect;
using cjui::widen;
namespace dz = design;
namespace su = sourceutil;

namespace {

struct Named {
    const wchar_t* name;
    const char* hex;
};
const Named kColours[] = {{L"White", "#FFFFFF"},       {L"Night ink", "#F4F7FB"},   {L"Black", "#000000"},        {L"Deep night", "#0B1A2E"},
                          {L"Aurora green", "#3FCF8E"}, {L"Aurora teal", "#2BB3B1"}, {L"Aurora violet", "#7A5CFF"}, {L"Sky blue", "#4DA3FF"},
                          {L"Pink", "#FF3DD6"},         {L"Gold", "#D9A441"},        {L"Red", "#E0566A"},          {L"Mist", "#9FB3C8"}};

struct Gradient {
    const wchar_t* name;
    std::vector<dz::Stop> stops;
    float angle;
};
std::vector<Gradient> gradients() {
    auto c = [](const char* h) {
        su::Rgba r;
        su::parseColour(h, r);
        return r;
    };
    const dz::Brand b = video::designBrand();
    auto bc = [&](const char* k) {
        const dz::BrandColour* x = dz::brandColour(b, k);
        return x ? x->colour : su::Rgba{};
    };
    return {
        {L"Your brand (main to second)", {{0, bc("primary"), "primary"}, {1, bc("secondary"), "secondary"}}, 0},
        {L"Your brand (panels to background)", {{0, bc("panel"), "panel"}, {1, bc("night"), "night"}}, 90},
        {L"Your brand (main fading out)", {{0, bc("primary"), "primary"}, {1, {bc("primary").r, bc("primary").g, bc("primary").b, 0}, "primary"}}, 0},
        {L"Aurora (green to violet)", {{0, c("#3FCF8E")}, {1, c("#7A5CFF")}}, 0},
        {L"Northern night", {{0, c("#060B17")}, {0.55f, c("#0B1A2E")}, {1, c("#0E2A33")}}, 90},
        {L"Neon (cyan to pink)", {{0, c("#00E5FF")}, {1, c("#FF3DD6")}}, 0},
        {L"Gold", {{0, c("#FFD678")}, {0.5f, c("#D9A441")}, {1, c("#B07820")}}, 90},
        {L"Sunset", {{0, c("#FF8A3D")}, {0.5f, c("#E0566A")}, {1, c("#7A5CFF")}}, 0},
        {L"Fade to clear (dark)", {{0, c("#0B1A2EF0")}, {1, c("#0B1A2E00")}}, 0},
    };
}

su::Rgba fromHex(const std::string& h) {
    su::Rgba r;
    su::parseColour(h, r);
    return r;
}

// "#3FCF8E 0, #7A5CFF 100" → stops (a percentage after each colour, optional).
bool parseStops(const std::string& text, std::vector<dz::Stop>& out) {
    out.clear();
    std::stringstream ss(text);
    std::string part;
    while (std::getline(ss, part, ',')) {
        std::stringstream ps(part);
        std::string col, at;
        ps >> col >> at;
        su::Rgba c;
        if (!su::parseColour(col, c)) return false;
        dz::Stop s;
        s.colour = c;
        s.at = at.empty() ? -1 : std::clamp((float)std::atof(at.c_str()) / 100.0f, 0.0f, 1.0f);
        out.push_back(s);
        if (out.size() > 16) return false;
    }
    if (out.size() < 2) return false;
    for (std::size_t i = 0; i < out.size(); ++i)
        if (out[i].at < 0) out[i].at = (float)i / (float)(out.size() - 1);
    std::stable_sort(out.begin(), out.end(), [](const dz::Stop& a, const dz::Stop& b) { return a.at < b.at; });
    return true;
}

std::string stopsText(const dz::Fill& f) {
    std::string s;
    for (auto& st : f.stops) s += (s.empty() ? "" : ", ") + su::colourText(st.colour) + " " + std::to_string((int)std::lround(st.at * 100));
    return s;
}

// A solid fill linked to a brand colour (new elements start on the brand).
dz::Fill brandFill(const char* key) {
    const dz::BrandColour* x = dz::brandColour(video::designBrand(), key);
    dz::Fill f = dz::Fill::solid(x ? x->colour : su::Rgba{});
    f.brand = x ? key : "";
    return f;
}

// A colour's name for the properties: a brand colour by its name ("Brand: Main colour"), else #RRGGBB.
std::wstring colourName(const su::Rgba& c, const std::string& brand) {
    if (!brand.empty())
        if (const dz::BrandColour* x = dz::brandColour(video::designBrand(), brand)) return L"Brand: " + widen(x->name);
    return widen(su::colourText(c));
}

std::wstring fillText(const dz::Fill& f) {
    if (f.type == dz::Fill::None) return L"None";
    if (f.type == dz::Fill::Solid) return colourName(f.colour, f.brand);
    bool linked = false;
    for (auto& st : f.stops) linked = linked || !st.brand.empty();
    return L"Gradient · " + std::to_wstring(f.stops.size()) + L" colours" + (linked ? L" (brand)" : L"");
}

std::wstring fontName(const std::string& f) {
    const dz::Brand b = video::designBrand();
    if (f == dz::kBrandHeading) return L"Brand heading (" + widen(b.headingFont) + L")";
    if (f == dz::kBrandBody) return L"Brand body (" + widen(b.bodyFont) + L")";
    return widen(f);
}

const wchar_t* kAnimNames[] = {L"None", L"Fade in", L"Slide in from the left", L"Slide in from the right", L"Slide up from the bottom",
                               L"Slide down from the top", L"Pop", L"Wipe"};

std::wstring profilePath() {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) out = std::wstring(p) + L"\\NorthRay\\profile.json";
    CoTaskMemFree(p);
    return out;
}

// The details templates fill in, shared by the NorthRay apps (%LOCALAPPDATA%\NorthRay\profile.json).
const char* kProfileKeys[] = {"name", "twitch", "youtube", "instagram", "tiktok", "website", "schedule", "book", "series", "show", "episode"};
const wchar_t* kProfileLabels[] = {L"Your name", L"Twitch", L"YouTube", L"Instagram", L"TikTok", L"Website", L"Schedule (e.g. Saturdays 8pm)",
                                   L"Book (its title)", L"Series (e.g. Book One of The Alvora Chronicles)", L"Radio show", L"Episode"};
constexpr int kProfileCount = (int)(sizeof kProfileKeys / sizeof kProfileKeys[0]);

}  // namespace

// ---------------------------------------------------------------- profile (placeholders)

void CaptureWindow::loadProfile() {
    std::ifstream f(std::filesystem::path(profilePath()), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    const nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, false);
    video::Placeholders v;
    for (const char* k : kProfileKeys) v.push_back({k, j.is_object() && j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string()});
    // Writing Studio's book being written, when it has one, comes before Your details' book and series.
    for (auto& [k, val] : writingValues_) {
        if (val.empty()) continue;
        auto it = std::find_if(v.begin(), v.end(), [&](const auto& p) { return p.first == k; });
        if (it != v.end()) it->second = val;
        else v.push_back({k, val});
    }
    // Writing Studio's own values are there even when it isn't, empty, so a design never shows "{chapter}".
    for (const char* k : {"chapter", "chapterwords", "chapterline", "words", "goal", "today", "bookwords", "bookline", "streak", "streakline"})
        if (std::none_of(v.begin(), v.end(), [&](const auto& p) { return p.first == k; })) v.push_back({k, ""});
    v.push_back({"cover", ""});   // stays "" without Writing Studio's cover
    for (auto& [k, val] : writingValues_)
        if (k == "cover" && !val.empty()) v.back().second = val;
    profile_ = v;
    video::setDesignValues(v);
}

void CaptureWindow::editProfile() {
    std::vector<MenuItem> items;
    for (int i = 0; i < kProfileCount; ++i) {
        const std::string now = i < (int)profile_.size() ? profile_[(size_t)i].second : std::string();
        std::wstring hint = now.empty() ? L"not set" : widen(now);
        for (auto& [k, val] : writingValues_)
            if (k == kProfileKeys[i] && !val.empty()) hint = L"from Writing Studio: " + widen(val);
        items.push_back(MenuItem::item(kProfileLabels[i], 100 + (UINT)i, hint));
    }
    POINT pt;
    GetCursorPos(&pt);
    const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
    if (cmd < 100 || cmd >= 100 + (UINT)kProfileCount) return;
    const int i = (int)cmd - 100;
    askText(std::wstring(kProfileLabels[i]) + L" ({" + widen(kProfileKeys[i]) + L"} in designs)",
            i < (int)profile_.size() ? widen(profile_[(size_t)i].second) : L"", [this, i](const std::wstring& v) {
                nlohmann::json j = nlohmann::json::object();
                for (auto& [k, val] : profile_) j[k] = val;
                j[kProfileKeys[i]] = narrow(v).substr(0, 120);
                const std::wstring path = profilePath();
                std::filesystem::create_directories(std::filesystem::path(path).parent_path());
                std::ofstream(std::filesystem::path(path), std::ios::binary | std::ios::trunc) << j.dump(2);
                loadProfile();
                designRendered_.clear();
                invalidate();
            });
}

// Writing Studio's summary (%LOCALAPPDATA%\NorthRay\Writing Studio\summary.json), kept in step automatically:
// the book and chapter being written, its series line and cover, today's words (typing not yet saved included),
// the daily goal, the streak and the book's words, as {book} {series} {cover} {chapter} {chapterwords} {chapterline} {words} {goal} {today}
// {streak} {bookwords}, and {bookline} and {streakline} as phrases. Writing Studio rewrites it within two seconds of a change; Capture looks every two seconds
// and reads it when it has changed. Today's words count only if it's today's summary.
void CaptureWindow::writingTick() {
    const ULONGLONG now = GetTickCount64();
    if (now - writingChecked_ < 1900) return;
    writingChecked_ = now;
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::path(profilePath()).parent_path() / L"Writing Studio" / L"summary.json";
    const auto t = std::filesystem::last_write_time(p, ec);
    const long long stamp = ec ? 0 : (long long)t.time_since_epoch().count();
    if (stamp == writingStamp_) return;
    writingStamp_ = stamp;
    std::vector<std::pair<std::string, std::string>> vals;
    if (stamp) {
        std::ifstream f(p, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        const nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, false);
        if (j.is_object() && j.value("format", "") == "northray-writing-summary") {
            auto str = [&](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>().substr(0, 1024) : std::string(); };
            vals.push_back({"book", str("book")});
            vals.push_back({"series", str("series")});
            vals.push_back({"cover", str("cover")});
            SYSTEMTIME st;
            GetLocalTime(&st);
            char today[16];
            std::snprintf(today, sizeof today, "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
            const bool isToday = str("day") == today;
            const long long words = isToday && j.contains("wordsToday") && j["wordsToday"].is_number_integer() ? j["wordsToday"].get<long long>() : 0;
            auto grouped = [](long long n) {   // 12,480
                std::string d = std::to_string(n < 0 ? -n : n), out;
                for (std::size_t i = 0; i < d.size(); ++i) {
                    if (i && (d.size() - i) % 3 == 0) out += ',';
                    out += d[i];
                }
                return (n < 0 ? "-" : "") + out;
            };
            auto integer = [&](const char* k) { return j.contains(k) && j[k].is_number_integer() ? j[k].get<long long>() : 0LL; };
            const long long goal = std::max(0LL, integer("dailyGoal"));
            vals.push_back({"chapter", str("chapter")});
            vals.push_back({"words", grouped(words)});
            vals.push_back({"goal", goal ? grouped(goal) : std::string()});
            vals.push_back({"today", grouped(words) + (goal ? " of " + grouped(goal) : std::string()) + (words == 1 && !goal ? " word today" : " words today")});
            const long long book = integer("bookWords"), streak = integer("streak"), chapterWords = integer("chapterWords");
            vals.push_back({"chapterwords", j.contains("chapterWords") ? grouped(chapterWords) : std::string()});
            vals.push_back({"chapterline", j.contains("chapterWords") ? grouped(chapterWords) + (chapterWords == 1 ? " word" : " words") : std::string()});
            vals.push_back({"bookwords", j.contains("bookWords") ? grouped(book) : std::string()});
            vals.push_back({"bookline", j.contains("bookWords") ? grouped(book) + (book == 1 ? " word in the book" : " words in the book") : std::string()});
            vals.push_back({"streak", j.contains("streak") ? std::to_string(streak) : std::string()});
            vals.push_back({"streakline", streak > 0 ? std::to_string(streak) + "-day streak" : std::string()});
        }
    }
    if (vals == writingValues_) return;
    writingValues_ = vals;
    loadProfile();
    designRendered_.clear();
    invalidate();
}

// ---------------------------------------------------------------- opening and saving

void CaptureWindow::addDesign() {
    // Start from a template or an empty page.
    std::vector<MenuItem> items = {MenuItem::item(L"An empty page (1920 × 1080)", 1), MenuItem::sep()};
    const auto& ts = dz::templates();
    std::string group;
    for (std::size_t i = 0; i < ts.size(); ++i) {
        if (ts[i].group != group) {
            group = ts[i].group;
            items.push_back(MenuItem::item(widen(group), 2, {}, false));
        }
        items.push_back(MenuItem::item(L"   " + widen(ts[i].name), 100 + (UINT)i,
                                       std::to_wstring(ts[i].doc.width) + L" × " + std::to_wstring(ts[i].doc.height)));
    }
    POINT pt;
    GetCursorPos(&pt);
    const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
    dz::Document d;
    if (cmd == 1) d.name = "Design";
    else if (cmd >= 100 && cmd < 100 + ts.size()) d = ts[cmd - 100].doc;
    else return;
    const Id before = selected_;
    addCapture(SourceKind::Design, d.name, dz::toJson(d));
    // The new item is selected; open the designer on its source.
    if (const SceneItem* it = scenes_.item(scenes_.current(), selected_); it && selected_ != before) openDesigner(it->source);
}

void CaptureWindow::openDesigner(const Id& source) {
    const Source* s = scenes_.source(source);
    if (!s || s->kind != SourceKind::Design) return;
    designFor_ = source;
    designDoc_ = dz::fromJson(s->settings);
    designSel_.clear();
    designUndo_.clear();
    designRendered_.clear();
    filtersFor_.clear();
    settingsOpen_ = true;
    resetScroll();
    invalidate();
}

void CaptureWindow::designChanged(bool keepUndo) {
    const Source* s = scenes_.source(designFor_);
    if (!s) return;
    if (keepUndo && s->settings != dz::toJson(designDoc_)) {
        designUndo_.push_back(s->settings);
        if (designUndo_.size() > 60) designUndo_.erase(designUndo_.begin());
    }
    scenes_.setSourceSettings(designFor_, dz::toJson(designDoc_));
    scenes_.renameSource(designFor_, designDoc_.name);
    syncSources();
    scenesChanged();
}

void CaptureWindow::designUndo() {
    if (designUndo_.empty()) return;
    designDoc_ = dz::fromJson(designUndo_.back());
    designUndo_.pop_back();
    if (!designDoc_.find(designSel_)) designSel_.clear();
    designChanged(false);
}

void CaptureWindow::exportDesign() {
    PWSTR pics = nullptr;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pics))) folder = std::wstring(pics) + L"\\NorthRay Capture";
    CoTaskMemFree(pics);
    if (folder.empty()) return;
    SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    video::DesignPicture p;
    std::wstring err;
    if (!video::renderDesign(designDoc_, 1e9f, 1.0f, profile_, p, err)) {
        flash(L"The design couldn't be drawn: " + err, 6);
        return;
    }
    std::wstring name = widen(designDoc_.name);
    for (auto& ch : name)
        if (wcschr(L"\\/:*?\"<>|", ch)) ch = L'-';
    wchar_t stamp[32];
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    wcsftime(stamp, 32, L"%Y-%m-%d %H.%M.%S", &tmv);
    const std::wstring path = folder + L"\\" + name + L" " + std::to_wstring(p.w) + L"x" + std::to_wstring(p.h) + L" - " + stamp + L".png";
    if (!video::savePng(path, p)) {
        flash(L"The PNG couldn't be written to your Pictures folder.", 6);
        return;
    }
    log::info("Design saved as PNG: " + narrow(path));
    flash(L"Saved " + std::to_wstring(p.w) + L"×" + std::to_wstring(p.h) + L" PNG in Pictures › NorthRay Capture.", 8);
}

// The design's entrance as a looping animated GIF (to post or share): 25 frames a second from the start
// of the entrances until they've finished, then the finished look held for a moment. At most 800 wide,
// as GIFs get large quickly.
void CaptureWindow::exportDesignGif() {
    const int entrance = dz::entranceMs(designDoc_);
    if (entrance <= 0) {
        flash(L"Nothing in this design moves yet. Give an element an Entrance (its menu), or use Save as PNG for a still picture.", 8);
        return;
    }
    PWSTR pics = nullptr;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pics))) folder = std::wstring(pics) + L"\\NorthRay Capture";
    CoTaskMemFree(pics);
    if (folder.empty()) return;
    SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    const float scale = std::min(1.0f, 800.0f / (float)std::max(1, designDoc_.width));
    constexpr int kStepMs = 40, kHoldMs = 1500;
    std::vector<gif::Frame> frames;
    int w = 0, h = 0;
    for (int t = 0;; t += kStepMs) {
        const bool last = t >= entrance;
        video::DesignPicture p;
        std::wstring err;
        if (!video::renderDesign(designDoc_, last ? 1e9f : (float)t, scale, profile_, p, err)) {
            flash(L"The design couldn't be drawn: " + err, 6);
            return;
        }
        if (frames.empty()) w = p.w, h = p.h;
        if (p.w != w || p.h != h) return;
        frames.push_back({std::move(p.bgra), last ? kHoldMs : kStepMs});
        if (last) break;
    }
    const std::vector<std::uint8_t> bytes = gif::encode(w, h, frames);
    if (bytes.empty()) {
        flash(L"The GIF couldn't be made.", 6);
        return;
    }
    std::wstring name = widen(designDoc_.name);
    for (auto& ch : name)
        if (wcschr(L"\\/:*?\"<>|", ch)) ch = L'-';
    wchar_t stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &now);
    wcsftime(stamp, 32, L"%Y-%m-%d %H.%M.%S", &tmv);
    const std::wstring path = folder + L"\\" + name + L" " + std::to_wstring(w) + L"x" + std::to_wstring(h) + L" - " + stamp + L".gif";
    FILE* f = _wfopen(path.c_str(), L"wb");
    const bool ok = f && std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    if (f) std::fclose(f);
    if (!ok) {
        DeleteFileW(path.c_str());
        flash(L"The GIF couldn't be written to your Pictures folder.", 6);
        return;
    }
    log::info("Design saved as an animated GIF: " + narrow(path) + " (" + std::to_string(frames.size()) + " frames)");
    flash(L"Saved a " + std::to_wstring(w) + L"×" + std::to_wstring(h) + L" animated GIF (" + std::to_wstring((bytes.size() + 1023) / 1024) +
              L" KB) in Pictures › NorthRay Capture.", 8);
}

// ---------------------------------------------------------------- the canvas: mouse and keys

bool CaptureWindow::designerMouseDown(float x, float y) {
    if (designFor_.empty() || !designCanvas_.contains(x, y)) return false;
    const float px = (x - designCanvas_.x) / designScale_, py = (y - designCanvas_.y) / designScale_;
    // The selected element's resize handle (bottom right) first.
    if (const dz::Element* e = designDoc_.find(designSel_); e && !e->locked) {
        const float hx = designCanvas_.x + (e->x + e->w) * designScale_, hy = designCanvas_.y + (e->y + e->h) * designScale_;
        if (std::fabs(x - hx) < 9 && std::fabs(y - hy) < 9) {
            ddrag_ = 2;
            dStartX_ = px, dStartY_ = py;
            dStartBox_ = {e->x, e->y, e->w, e->h};
            dBefore_ = dz::toJson(designDoc_);
            return true;
        }
    }
    designSel_ = dz::hitTest(designDoc_, px, py);
    if (const dz::Element* e = designDoc_.find(designSel_)) {
        ddrag_ = 1;
        dStartX_ = px, dStartY_ = py;
        dStartBox_ = {e->x, e->y, e->w, e->h};
        dBefore_ = dz::toJson(designDoc_);
    }
    invalidate();
    return true;
}

bool CaptureWindow::designerMouseMove(float x, float y, bool leftDown) {
    if (designFor_.empty() || ddrag_ == 0) return false;
    if (!leftDown) return designerMouseUp();
    dz::Element* e = designDoc_.find(designSel_);
    if (!e) return true;
    const float px = (x - designCanvas_.x) / designScale_, py = (y - designCanvas_.y) / designScale_;
    const bool shift = GetKeyState(VK_SHIFT) < 0;   // Shift: no snapping
    dz::Box b = dStartBox_;
    if (ddrag_ == 1) {
        b.x += px - dStartX_;
        b.y += py - dStartY_;
    } else {
        b.w = std::max(4.0f, b.w + px - dStartX_);
        b.h = std::max(4.0f, b.h + py - dStartY_);
        if (GetKeyState(VK_CONTROL) < 0 && dStartBox_.w > 0) b.h = b.w * dStartBox_.h / dStartBox_.w;   // Ctrl: keep its shape
    }
    dz::Snapped s{b, {}, {}};
    if (!shift) s = dz::snap(designDoc_, designSel_, b, 6 / designScale_, ddrag_ == 2);
    e->x = std::round(s.box.x);
    e->y = std::round(s.box.y);
    e->w = std::round(s.box.w);
    e->h = std::round(s.box.h);
    dGuidesX_ = s.guidesX;
    dGuidesY_ = s.guidesY;
    invalidate();
    return true;
}

bool CaptureWindow::designerMouseUp() {
    if (ddrag_ == 0) return false;
    ddrag_ = 0;
    dGuidesX_.clear();
    dGuidesY_.clear();
    if (dz::toJson(designDoc_) != dBefore_) {
        designUndo_.push_back(dBefore_);
        designChanged(false);
    }
    invalidate();
    return true;
}

bool CaptureWindow::designerKey(WPARAM vk, bool ctrl, bool shift) {
    if (designFor_.empty()) return false;
    if (ctrl && vk == 'Z') { designUndo(); return true; }
    dz::Element* e = designDoc_.find(designSel_);
    if (ctrl && vk == 'D' && e) {
        designSel_ = dz::duplicate(designDoc_, designSel_);
        designChanged();
        return true;
    }
    if (!e) return false;
    if (vk == VK_DELETE) {
        dz::remove(designDoc_, designSel_);
        designSel_.clear();
        designChanged();
        return true;
    }
    const float step = shift ? 10.0f : 1.0f;
    float dx = 0, dy = 0;
    if (vk == VK_LEFT) dx = -step;
    else if (vk == VK_RIGHT) dx = step;
    else if (vk == VK_UP) dy = -step;
    else if (vk == VK_DOWN) dy = step;
    else return false;
    e->x += dx;
    e->y += dy;
    designChanged();
    return true;
}

// ---------------------------------------------------------------- drawing

void CaptureWindow::drawDesigner(const Rect& area) {
    const Source* src = scenes_.source(designFor_);
    if (!src) {
        designFor_.clear();
        return;
    }
    gfx_.fill(area, pal_.paper);
    const float pad = 16, leftW = 230, rightW = 330;
    // Top bar.
    const float top = area.y + 12;
    addHit("dz:name", {area.x + pad, top, 520, 34}, [this] {
        askText(L"The design's name", widen(designDoc_.name), [this](const std::wstring& v) {
            if (v.empty()) return;
            designDoc_.name = narrow(v).substr(0, 80);
            designChanged();
        });
    });
    gfx_.text(L"Design: " + widen(designDoc_.name), {area.x + pad, top, 520, 34}, Font::Title, pal_.ink, {Align::Left, false, true, true});
    {
        float bx = area.right() - pad;
        auto topButton = [&](const std::string& id, const std::wstring& label, float w, std::function<void()> act, bool primary = false) {
            bx -= w;
            button(id, {bx, top + 2, w, 32}, label, std::move(act), primary);
            bx -= 8;
        };
        topButton("dz:done", L"Done", 100, [this] { openSettings(false); }, true);
        topButton("dz:png", L"Save as PNG", 130, [this] { exportDesign(); });
        topButton("dz:gif", L"Save as GIF", 120, [this] { exportDesignGif(); });
        topButton("dz:lib", L"To the library", 140, [this] { librarySaveDesign(); });
        topButton("dz:profile", L"Your details…", 140, [this] { editProfile(); });
        topButton("dz:brand", L"Brand kit…", 120, [this] { editBrand(); });
        topButton("dz:page", L"Page…", 90, [this] {
            std::vector<MenuItem> items = {
                MenuItem::item(L"Size…", 1, std::to_wstring(designDoc_.width) + L" × " + std::to_wstring(designDoc_.height)),
                MenuItem::item(L"1920 × 1080 (the canvas)", 2), MenuItem::item(L"1280 × 720", 3), MenuItem::item(L"1080 × 1920 (vertical)", 4),
                MenuItem::item(L"320 × 100 (Twitch panel)", 5), MenuItem::sep(),
                MenuItem::item(L"Background: none (see-through)", 10, {}, true, designDoc_.background.type == dz::Fill::None),
                MenuItem::item(L"Background colour…", 11, designDoc_.background.type == dz::Fill::Solid ? widen(su::colourText(designDoc_.background.colour)) : L""),
            };
            const auto gs = gradients();
            std::vector<MenuItem> gm;
            for (std::size_t i = 0; i < gs.size(); ++i) gm.push_back(MenuItem::item(gs[i].name, 20 + (UINT)i));
            items.push_back(MenuItem::sub(L"Background gradient", gm));
            const dz::Brand brand = video::designBrand();
            std::vector<MenuItem> bm;
            for (std::size_t i = 0; i < brand.colours.size(); ++i)
                bm.push_back(MenuItem::item(widen(brand.colours[i].name), 40 + (UINT)i, widen(su::colourText(brand.colours[i].colour)), true,
                                            designDoc_.background.type == dz::Fill::Solid && designDoc_.background.brand == brand.colours[i].key));
            items.push_back(MenuItem::sub(L"Background: a brand colour", bm));
            items.push_back(MenuItem::sep());
            items.push_back(MenuItem::item(L"Follow the brand kit", 30, L"links matching colours and fonts"));
            items.push_back(MenuItem::item(L"Stop following the brand kit", 31, L"keeps how it looks now", dz::usesBrand(designDoc_)));
            items.push_back(MenuItem::sep());
            items.push_back(MenuItem::item(L"Show only while music plays", 32, L"{song} and {artist}", true, designDoc_.onlyWhilePlaying));
            POINT pt;
            GetCursorPos(&pt);
            const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
            const int sizes[][2] = {{0, 0}, {0, 0}, {1920, 1080}, {1280, 720}, {1080, 1920}, {320, 100}};
            if (cmd >= 2 && cmd <= 5) {
                designDoc_.width = sizes[cmd][0];
                designDoc_.height = sizes[cmd][1];
                designChanged();
            } else if (cmd == 1) {
                askText(L"The page's size, width × height in pixels", std::to_wstring(designDoc_.width) + L", " + std::to_wstring(designDoc_.height),
                        [this](const std::wstring& v) {
                            su::Placement p;
                            if (!su::parsePlacement(narrow(v), p) || !p.hasX || !p.hasY || p.x < 16 || p.y < 16 || p.x > 8192 || p.y > 8192) {
                                flash(L"Type the width and height, e.g. 1920, 1080.", 5);
                                return;
                            }
                            designDoc_.width = (int)p.x;
                            designDoc_.height = (int)p.y;
                            designChanged();
                        });
            } else if (cmd == 10) {
                designDoc_.background = dz::Fill::none();
                designChanged();
            } else if (cmd == 11) {
                askText(L"The background colour (#RRGGBB)", L"#0B1A2E", [this](const std::wstring& v) {
                    su::Rgba c;
                    if (!su::parseColour(narrow(v), c)) return;
                    designDoc_.background = dz::Fill::solid(c);
                    designChanged();
                });
            } else if (cmd >= 40 && cmd < 40 + brand.colours.size()) {
                designDoc_.background = dz::Fill::solid(brand.colours[cmd - 40].colour);
                designDoc_.background.brand = brand.colours[cmd - 40].key;
                designChanged();
            } else if (cmd == 30) {
                dz::linkToBrand(designDoc_, brand);
                designChanged();
                flash(dz::usesBrand(designDoc_) ? L"This design now follows your brand kit where its colours and fonts match."
                                                : L"Nothing here matches the brand kit's colours or Segoe UI text.", 6);
            } else if (cmd == 32) {
                designDoc_.onlyWhilePlaying = !designDoc_.onlyWhilePlaying;
                designChanged();
            } else if (cmd == 31) {
                designDoc_ = dz::unlinkBrand(designDoc_, brand);
                designChanged();
            } else if (cmd >= 20 && cmd < 20 + gs.size()) {
                designDoc_.background = dz::Fill::linear(gs[cmd - 20].stops, gs[cmd - 20].angle);
                designChanged();
            }
        });
    }

    const float bodyY = top + 50, bodyH = area.bottom() - bodyY - pad;
    const Rect left{area.x + pad, bodyY, leftW, bodyH};
    const Rect right{area.right() - pad - rightW, bodyY, rightW, bodyH};
    const Rect centre{left.right() + pad, bodyY, right.x - left.right() - pad * 2, bodyH};

    // ---- Left: add, and the layers (top first).
    {
        float y = left.y;
        gfx_.text(L"ADD", {left.x, y, left.w, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
        y += 22;
        auto addEl = [this](dz::Kind k) {
            dz::Element e;
            e.kind = k;
            e.w = k == dz::Kind::Text ? 600.0f : 400.0f;
            e.h = k == dz::Kind::Text ? 120.0f : 240.0f;
            e.x = std::round((designDoc_.width - std::min(e.w, (float)designDoc_.width)) / 2);
            e.y = std::round((designDoc_.height - std::min(e.h, (float)designDoc_.height)) / 2);
            e.w = std::min(e.w, (float)designDoc_.width);
            e.h = std::min(e.h, (float)designDoc_.height);
            if (k == dz::Kind::Text) {
                e.text = "Your text";
                e.size = std::max(12.0f, std::min(72.0f, designDoc_.height / 8.0f));
                e.align = "centre";
                e.fill = brandFill("text");
                e.font = dz::kBrandBody;
            } else if (k == dz::Kind::Rect) {
                e.fill = dz::Fill::linear(gradients()[0].stops, 0);
                e.radius = 20;
            } else if (k == dz::Kind::Ellipse) {
                e.fill = brandFill("secondary");
            }
            designSel_ = dz::add(designDoc_, e);
            designChanged();
        };
        const float bw = (left.w - 8) / 2;
        button("dz:add:text", {left.x, y, bw, 30}, L"Text", [addEl] { addEl(dz::Kind::Text); });
        button("dz:add:rect", {left.x + bw + 8, y, bw, 30}, L"Rectangle", [addEl] { addEl(dz::Kind::Rect); });
        y += 38;
        button("dz:add:ell", {left.x, y, bw, 30}, L"Ellipse", [addEl] { addEl(dz::Kind::Ellipse); });
        button("dz:add:img", {left.x + bw + 8, y, bw, 30}, L"Picture…", [this] {
            std::wstring path;
            if (!pickImageFile(path)) return;
            dz::Element e;
            e.kind = dz::Kind::Image;
            e.file = narrow(path);
            e.fill = dz::Fill::none();
            e.w = 480, e.h = 270;
            e.x = std::round((designDoc_.width - e.w) / 2);
            e.y = std::round((designDoc_.height - e.h) / 2);
            const std::size_t cut = path.find_last_of(L"\\/");
            e.name = narrow(cut == std::wstring::npos ? path : path.substr(cut + 1)).substr(0, 60);
            designSel_ = dz::add(designDoc_, e);
            designChanged();
        });
        y += 38;
        button("dz:add:timer", {left.x, y, bw, 30}, L"Timer", [this] {
            dz::Element e;
            e.kind = dz::Kind::Timer;
            e.w = 600, e.h = 160;
            e.x = std::round((designDoc_.width - e.w) / 2);
            e.y = std::round((designDoc_.height - e.h) / 2);
            e.size = 96;
            e.bold = true;
            e.align = "centre";
            e.fill = brandFill("text");
            e.font = dz::kBrandHeading;
            designSel_ = dz::add(designDoc_, e);
            designChanged();
        });
        button("dz:add:qr", {left.x + bw + 8, y, bw, 30}, L"QR code", [this] {
            dz::Element e;
            e.kind = dz::Kind::Qr;
            e.qr = "{website}";
            e.w = e.h = 240;
            e.x = std::round((designDoc_.width - e.w) / 2);
            e.y = std::round((designDoc_.height - e.h) / 2);
            e.fill = dz::Fill::solid({255, 255, 255, 255});
            e.radius = 0;
            designSel_ = dz::add(designDoc_, e);
            designChanged();
        });
        y += 38;
        button("dz:add:chat", {left.x + bw + 8, y, bw, 30}, L"Chat", [this] {
            dz::Element e;
            e.kind = dz::Kind::Chat;
            e.name = "Chat";
            e.w = std::min(600.f, (float)designDoc_.width), e.h = std::min(700.f, (float)designDoc_.height);
            e.x = std::round((designDoc_.width - e.w) / 2);
            e.y = std::round((designDoc_.height - e.h) / 2);
            e.size = 28;
            e.fill = brandFill("text");
            e.font = dz::kBrandBody;
            designSel_ = dz::add(designDoc_, e);
            designChanged();
        });
        button("dz:add:icon", {left.x, y, bw, 30}, L"Icon\u2026", [this] {
            std::vector<MenuItem> items;
            const auto& ics = dz::icons();
            for (std::size_t i = 0; i < ics.size(); ++i) items.push_back(MenuItem::item(widen(ics[i].name), 100 + (UINT)i));
            POINT pt;
            GetCursorPos(&pt);
            const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
            if (cmd < 100 || cmd >= 100 + ics.size()) return;
            dz::Element e;
            e.kind = dz::Kind::Icon;
            e.icon = ics[cmd - 100].id;
            e.name = ics[cmd - 100].name;
            e.w = e.h = 120;
            e.x = std::round((designDoc_.width - e.w) / 2);
            e.y = std::round((designDoc_.height - e.h) / 2);
            e.fill = dz::Fill::linear(gradients()[0].stops, 0);
            designSel_ = dz::add(designDoc_, e);
            designChanged();
        });
        y += 48;
        gfx_.text(L"LAYERS", {left.x, y, left.w, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
        y += 22;
        for (int i = (int)designDoc_.elements.size() - 1; i >= 0 && y < left.bottom() - 30; --i) {
            const dz::Element& e = designDoc_.elements[(size_t)i];
            const Rect row{left.x, y, left.w, 30};
            const std::string id = "dz:layer:" + e.id;
            const std::string eid = e.id;
            addHit(id, {row.x, row.y, row.w - 56, row.h}, [this, eid] {
                designSel_ = eid;
                invalidate();
            });
            const bool sel = e.id == designSel_;
            if (sel) gfx_.fillRounded(row, 6, pal_.ribbon.withAlpha(0.18f));
            else if (hoverAmount(id) > 0) gfx_.fillRounded(row, 6, pal_.ink.withAlpha(0.05f * hoverAmount(id)));
            {   // a small picture of its kind
                const Rect ic{row.x + 8, row.y + 8, 16, 14};
                if (e.kind == dz::Kind::Text) gfx_.text(L"T", {row.x + 6, row.y, 20, 30}, Font::BodyStrong, pal_.inkMuted, {Align::Center, false, false, true});
                else if (e.kind == dz::Kind::Timer) {
                    gfx_.circle(ic.x + 8, ic.y + 7, 7, pal_.inkMuted, false, 1.5f);
                    gfx_.line(ic.x + 8, ic.y + 7, ic.x + 8, ic.y + 2, pal_.inkMuted, 1.5f);
                    gfx_.line(ic.x + 8, ic.y + 7, ic.x + 12, ic.y + 7, pal_.inkMuted, 1.5f);
                } else if (e.kind == dz::Kind::Qr) {
                    for (int q = 0; q < 4; ++q) gfx_.fill({ic.x + (q % 2) * 9.0f, ic.y + (q / 2) * 8.0f, 6, 6}, pal_.inkMuted);
                } else if (e.kind == dz::Kind::Chat) {
                    gfx_.strokeRounded({ic.x, ic.y, 16, 11}, 3, pal_.inkMuted, 1.5f);
                    gfx_.line(ic.x + 4, ic.y + 11, ic.x + 3, ic.y + 15, pal_.inkMuted, 1.5f);
                } else if (e.kind == dz::Kind::Icon)
                    gfx_.text(L"*", {row.x + 6, row.y, 20, 30}, Font::BodyStrong, pal_.inkMuted, {Align::Center, false, false, true});
                else if (e.kind == dz::Kind::Ellipse) gfx_.circle(ic.x + 8, ic.y + 7, 7, pal_.inkMuted, false, 1.5f);
                else if (e.kind == dz::Kind::Rect) gfx_.strokeRounded(ic, 3, pal_.inkMuted, 1.5f);
                else {
                    gfx_.strokeRounded(ic, 2, pal_.inkMuted, 1.5f);
                    gfx_.line(ic.x + 2, ic.bottom() - 3, ic.x + 7, ic.y + 6, pal_.inkMuted, 1.5f);
                    gfx_.line(ic.x + 7, ic.y + 6, ic.right() - 2, ic.bottom() - 3, pal_.inkMuted, 1.5f);
                }
            }
            gfx_.text(widen(e.name), {row.x + 30, row.y, row.w - 90, 30}, sel ? Font::BodyStrong : Font::Body, e.visible ? pal_.ink : pal_.inkFaint,
                      {Align::Left, false, true, true});
            const Rect eye{row.right() - 52, row.y + 3, 24, 24}, lock{row.right() - 26, row.y + 3, 24, 24};
            addHit(id + ":eye", eye, [this, eid] {
                if (dz::Element* x = designDoc_.find(eid)) x->visible = !x->visible;
                designChanged();
            });
            addHit(id + ":lock", lock, [this, eid] {
                if (dz::Element* x = designDoc_.find(eid)) x->locked = !x->locked;
                designChanged();
            });
            // Shown or hidden (an eye: filled when shown), locked or not (a padlock).
            gfx_.circle(eye.x + 12, eye.y + 12, 6, e.visible ? pal_.inkMuted : pal_.inkFaint, false, 1.5f);
            if (e.visible) gfx_.circle(eye.x + 12, eye.y + 12, 2.5f, pal_.inkMuted, true);
            else gfx_.line(eye.x + 5, eye.y + 19, eye.x + 19, eye.y + 5, pal_.inkFaint, 1.5f);
            const Color lc = e.locked ? pal_.ink : pal_.inkFaint.withAlpha(0.6f);
            gfx_.strokeRounded({lock.x + 7, lock.y + 11, 10, 8}, 1.5f, lc, 1.5f);
            gfx_.circle(lock.x + 12, lock.y + 10, 3.5f, lc, false, 1.5f);
            if (e.locked) gfx_.fill({lock.x + 7, lock.y + 11, 10, 8}, lc);
            y += 32;
        }
        if (designDoc_.elements.empty())
            gfx_.text(L"Nothing yet: add text, shapes or pictures above.", {left.x, y, left.w, 40}, Font::Small, pal_.inkMuted);
    }

    // ---- Centre: the page, fitted, on a checkerboard (see-through parts show as squares).
    {
        const float sc = std::min(centre.w / designDoc_.width, centre.h / designDoc_.height);
        designScale_ = sc;
        const float pw = designDoc_.width * sc, ph = designDoc_.height * sc;
        designCanvas_ = {std::round(centre.x + (centre.w - pw) / 2), std::round(centre.y + (centre.h - ph) / 2), std::round(pw), std::round(ph)};
        const Rect& c = designCanvas_;
        gfx_.pushClip(c);
        const float cell = 12;
        for (float yy = c.y; yy < c.bottom(); yy += cell)
            for (float xx = c.x; xx < c.right(); xx += cell)
                gfx_.fill({xx, yy, cell, cell}, ((int)((xx - c.x) / cell) + (int)((yy - c.y) / cell)) % 2 ? cjui::hexColor("#D8DEE6") : cjui::hexColor("#F1F4F8"));
        gfx_.popClip();
        // Drawn again only when the design (or its size on screen) changes.
        // Timers show the time now (a countdown its full length): the canvas follows them each second.
        const dz::Clock clock = video::clockNow(0);
        std::string timers;
        for (auto& el : designDoc_.elements)
            if (el.kind == dz::Kind::Timer) timers += dz::timerText(el, clock).text + "|";
        std::string prof;
        for (auto& [k, v] : profile_) prof += v + "|";
        // Chat and the brand kit: their version; fading chat: each second too.
        unsigned live = 0;
        video::designValues(&live);
        prof += std::to_string(live) + "|";
        for (auto& el : designDoc_.elements)
            if (el.kind == dz::Kind::Chat && el.chatFade > 0) {
                prof += std::to_string(GetTickCount64() / 1000);
                break;
            }
        const std::string key = dz::toJson(designDoc_) + "@" + std::to_string((int)c.w) + "x" + std::to_string((int)c.h) + "#" + prof + timers;
        if (key != designRendered_) {
            video::DesignPicture p;
            std::wstring err;
            gfx_.forgetImage(L"designer:canvas");
            designPixels_.clear();
            if (video::renderDesign(designDoc_, 1e9f, sc * gfx_.dpiScale(), profile_, p, err, true, clock)) {
                designPixels_.resize(p.bgra.size());
                for (std::size_t i = 0; i < p.bgra.size(); i += 4) {   // BGRA → RGBA
                    designPixels_[i] = p.bgra[i + 2];
                    designPixels_[i + 1] = p.bgra[i + 1];
                    designPixels_[i + 2] = p.bgra[i];
                    designPixels_[i + 3] = p.bgra[i + 3];
                }
                designPixW_ = p.w;
                designPixH_ = p.h;
            }
            designRendered_ = key;
        }
        if (!designPixels_.empty()) gfx_.drawPixels(L"designer:canvas", designPixW_, designPixH_, designPixels_.data(), c, true);
        gfx_.strokeRounded(c, 0, pal_.rule, 1);
        // Guides while dragging, and the selection.
        for (float gx : dGuidesX_) gfx_.line(c.x + gx * sc, c.y, c.x + gx * sc, c.bottom(), cjui::hexColor("#FF3DD6"), 1);
        for (float gy : dGuidesY_) gfx_.line(c.x, c.y + gy * sc, c.right(), c.y + gy * sc, cjui::hexColor("#FF3DD6"), 1);
        gfx_.pushClip({c.x - 6, c.y - 6, c.w + 12, c.h + 12});   // a selection reaching off the page stops at its edge
        if (const dz::Element* e = designDoc_.find(designSel_)) {
            const Rect r{c.x + e->x * sc, c.y + e->y * sc, e->w * sc, e->h * sc};
            gfx_.strokeRounded(r, 0, cjui::hexColor("#3FCF8E"), 1.5f);
            if (!e->locked) {
                const Rect hnd{r.right() - 5, r.bottom() - 5, 10, 10};
                gfx_.fill(hnd, cjui::hexColor("#FFFFFF"));
                gfx_.strokeRounded(hnd, 0, cjui::hexColor("#3FCF8E"), 1.5f);
            }
        }
        gfx_.popClip();
        gfx_.text(L"Drag to move (Shift: no snapping), the corner to resize (Ctrl: keep its shape). Arrow keys nudge; Delete removes; "
                  L"Ctrl+D copies; Ctrl+Z undoes.",
                  {centre.x, centre.bottom() - 2, centre.w, 18}, Font::Small, pal_.inkFaint, {Align::Center, false, true, true});
    }

    // ---- Right: the selected element's properties.
    drawDesignerProperties(right);
}

void CaptureWindow::drawDesignerProperties(const Rect& r) {
    float y = r.y;
    dz::Element* e = designDoc_.find(designSel_);
    if (!e) {
        gfx_.text(L"PAGE", {r.x, y, r.w, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
        y += 24;
        y += gfx_.text(std::to_wstring(designDoc_.width) + L" × " + std::to_wstring(designDoc_.height) + L" pixels. Background: " +
                           fillText(designDoc_.background) + L". Change both under Page…",
                       {r.x, y, r.w, 60}, Font::Small, pal_.inkMuted) + 12;
        y += gfx_.text(L"Click an element on the page or in Layers to change it. Text can show your details: {name}, {twitch}, {youtube}, "
                       L"{instagram}, {tiktok}, {website} and {schedule} (set them under Your details…), and the music playing in a "
                       L"video or sound source: {song} and {artist}. Colours and fonts marked Brand "
                       L"follow your Brand kit…: change it once and every design using it changes.",
                       {r.x, y, r.w, 120}, Font::Small, pal_.inkMuted) + 12;
        return;
    }
    const std::string eid = e->id;
    // Each change applies to the element (found again by id: the list may have changed).
    auto edit = [this, eid](std::function<void(dz::Element&)> f) {
        if (dz::Element* x = designDoc_.find(eid)) {
            f(*x);
            designChanged();
        }
    };
    auto label = [&](const std::wstring& t) {
        gfx_.text(t, {r.x, y, 110, 26}, Font::Body, pal_.ink, {Align::Left, false, true, true});
    };
    auto sliderRow = [&](const std::string& key, const std::wstring& name, float v, float lo, float hi, const wchar_t* unit,
                         std::function<void(dz::Element&, float)> apply) {
        label(name);
        slider("dz:p:" + key, {r.x + 112, y + 6, r.w - 112 - 70, 16}, (v - lo) / (hi - lo), [this, eid, lo, hi, apply](float t) {
            dz::Element* x = designDoc_.find(eid);
            if (!x) return;
            const float nv = std::round(lo + std::clamp(t, 0.0f, 1.0f) * (hi - lo));
            apply(*x, nv);
            designChanged(false);   // sliders: no undo step per pixel
        });
        wchar_t b[32];
        swprintf(b, 32, L"%.0f%ls", v, unit);
        gfx_.text(b, {r.right() - 66, y, 66, 26}, Font::Body, pal_.ink, {Align::Right, false, true});
        y += 32;
    };
    // Colours: the brand kit's first (linked: they follow the kit), then a few named ones, or any.
    // `apply` gets the colour and the brand key it's linked to ("" if none).
    auto colourMenu = [this](const su::Rgba& now, const std::string& nowBrand, std::function<void(su::Rgba, std::string)> apply, bool allowNone) {
        std::vector<MenuItem> items;
        if (allowNone) items.push_back(MenuItem::item(L"None", 99, {}, true, now.a == 0));
        const dz::Brand brand = video::designBrand();
        for (std::size_t i = 0; i < brand.colours.size(); ++i)
            items.push_back(MenuItem::item(L"Brand: " + widen(brand.colours[i].name), 200 + (UINT)i, widen(su::colourText(brand.colours[i].colour)),
                                           true, nowBrand == brand.colours[i].key));
        items.push_back(MenuItem::sep());
        for (int i = 0; i < (int)(sizeof kColours / sizeof kColours[0]); ++i)
            items.push_back(MenuItem::item(kColours[i].name, 100 + (UINT)i, {}, true, su::colourText(now) == kColours[i].hex));
        items.push_back(MenuItem::sep());
        items.push_back(MenuItem::item(L"Another colour…", 98, widen(su::colourText(now))));
        POINT pt;
        GetCursorPos(&pt);
        const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
        if (cmd == 99) apply({0, 0, 0, 0}, "");
        else if (cmd >= 200 && cmd < 200 + brand.colours.size()) {
            su::Rgba c = brand.colours[cmd - 200].colour;
            c.a = now.a ? now.a : 255;   // keep how see-through it was
            apply(c, brand.colours[cmd - 200].key);
        } else if (cmd >= 100 && cmd < 100 + sizeof kColours / sizeof kColours[0]) apply(fromHex(kColours[cmd - 100].hex), "");
        else if (cmd == 98)
            askText(L"A colour (#RRGGBB, or #RRGGBBAA to see through it)", widen(su::colourText(now)), [this, apply](const std::wstring& v) {
                su::Rgba c;
                if (!su::parseColour(narrow(v), c)) {
                    flash(L"Type a colour like #3FCF8E.", 5);
                    return;
                }
                apply(c, "");
            });
    };

    // Name and kind.
    static const wchar_t* kKindNames[] = {L"TEXT", L"RECTANGLE", L"ELLIPSE", L"PICTURE", L"TIMER", L"QR CODE", L"ICON", L"CHAT"};
    static_assert(sizeof kKindNames / sizeof kKindNames[0] == (std::size_t)dz::Kind::Chat + 1, "a name for every kind of element");
    gfx_.text((std::size_t)e->kind < std::size(kKindNames) ? kKindNames[(int)e->kind] : L"ELEMENT",
              {r.x, y, r.w, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 22;
    {
        const Rect nb{r.x, y, r.w, 30};
        button("dz:p:name", nb, widen(e->name), [this, eid] {
            const dz::Element* x = designDoc_.find(eid);
            if (!x) return;
            askText(L"The element's name (for Layers)", widen(x->name), [this, eid](const std::wstring& v) {
                if (dz::Element* z = designDoc_.find(eid)) z->name = narrow(v).substr(0, 80);
                designChanged();
            });
        });
        y += 38;
    }
    // Place.
    {
        label(L"Place");
        wchar_t b[96];
        swprintf(b, 96, L"%.0f, %.0f  ·  %.0f × %.0f", e->x, e->y, e->w, e->h);
        button("dz:p:place", {r.x + 112, y - 2, r.w - 112, 30}, b, [this, eid] {
            const dz::Element* x = designDoc_.find(eid);
            if (!x) return;
            askText(L"x, y, width, height on the page (pixels)",
                    widen(std::to_string((int)x->x) + ", " + std::to_string((int)x->y) + ", " + std::to_string((int)x->w) + ", " + std::to_string((int)x->h)),
                    [this, eid](const std::wstring& v) {
                        su::Placement p;
                        dz::Element* z = designDoc_.find(eid);
                        if (!z || !su::parsePlacement(narrow(v), p)) return;
                        if (p.hasX) z->x = p.x;
                        if (p.hasY) z->y = p.y;
                        if (p.hasSize && p.w >= 1 && p.h >= 1) z->w = p.w, z->h = p.h;
                        designChanged();
                    });
        });
        y += 36;
    }
    sliderRow("rot", L"Rotation", e->rotation, -180, 180, L"°", [](dz::Element& x, float v) { x.rotation = v; });
    sliderRow("op", L"Opacity", e->opacity * 100, 0, 100, L"%", [](dz::Element& x, float v) { x.opacity = v / 100; });

    // Fill (text: the letters).
    {
        label(e->kind == dz::Kind::Text ? L"Colour" : L"Fill");
        button("dz:p:fill", {r.x + 112, y - 2, r.w - 112, 30}, fillText(e->fill), [this, eid, edit, colourMenu] {
            const dz::Element* x = designDoc_.find(eid);
            if (!x) return;
            std::vector<MenuItem> items = {MenuItem::item(L"None", 1, {}, x->kind != dz::Kind::Text, x->fill.type == dz::Fill::None),
                                           MenuItem::item(L"A colour…", 2), MenuItem::sep()};
            const auto gs = gradients();
            for (std::size_t i = 0; i < gs.size(); ++i) items.push_back(MenuItem::item(gs[i].name, 10 + (UINT)i));
            items.push_back(MenuItem::sep());
            const bool grad = x->fill.type == dz::Fill::Linear || x->fill.type == dz::Fill::Radial;
            items.push_back(MenuItem::item(L"Gradient colours…", 3, grad ? widen(stopsText(x->fill)) : L"type them"));
            items.push_back(MenuItem::item(L"Round: from the middle out", 5, {}, grad, x->fill.type == dz::Fill::Radial));
            items.push_back(MenuItem::item(L"Gradient direction…", 4, x->fill.type == dz::Fill::Linear ? std::to_wstring((int)x->fill.angle) + L"°" : L"",
                                           x->fill.type == dz::Fill::Linear));
            POINT pt;
            GetCursorPos(&pt);
            const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
            if (cmd == 1) edit([](dz::Element& z) { z.fill = dz::Fill::none(); });
            else if (cmd == 2)
                colourMenu(x->fill.type == dz::Fill::Solid ? x->fill.colour : su::Rgba{255, 255, 255, 255}, x->fill.type == dz::Fill::Solid ? x->fill.brand : "",
                           [edit](su::Rgba c, std::string b) {
                               edit([c, b](dz::Element& z) {
                                   z.fill = dz::Fill::solid(c);
                                   z.fill.brand = b;
                               });
                           }, false);
            else if (cmd >= 10 && cmd < 10 + gs.size()) {
                const Gradient g = gs[cmd - 10];
                edit([g](dz::Element& z) { z.fill = dz::Fill::linear(g.stops, g.angle); });
            } else if (cmd == 5) {
                edit([](dz::Element& z) { z.fill.type = z.fill.type == dz::Fill::Radial ? dz::Fill::Linear : dz::Fill::Radial; });
            } else if (cmd == 3) {
                askText(L"Gradient colours: #RRGGBB and where (0-100), separated by commas", x->fill.type == dz::Fill::Linear ? widen(stopsText(x->fill)) : L"#3FCF8E 0, #7A5CFF 100",
                        [this, edit](const std::wstring& v) {
                            std::vector<dz::Stop> stops;
                            if (!parseStops(narrow(v), stops)) {
                                flash(L"Type two to sixteen colours, e.g. #3FCF8E 0, #2BB3B1 50, #7A5CFF 100.", 6);
                                return;
                            }
                            edit([stops](dz::Element& z) {
                                const bool round = z.fill.type == dz::Fill::Radial;
                                z.fill = dz::Fill::linear(stops, z.fill.type == dz::Fill::Linear ? z.fill.angle : 0);
                                if (round) z.fill.type = dz::Fill::Radial;
                            });
                        });
            } else if (cmd == 4) {
                askText(L"Direction in degrees (0 left to right, 90 top to bottom)", std::to_wstring((int)x->fill.angle), [edit](const std::wstring& v) {
                    float d = 0;
                    if (!su::parseDegrees(narrow(v), d)) return;
                    edit([d](dz::Element& z) { z.fill.angle = d; });
                });
            }
        });
        y += 36;
    }
    // Outline.
    {
        label(L"Outline");
        button("dz:p:stroke", {r.x + 112, y - 2, r.w - 112, 30}, e->stroke.a ? colourName(e->stroke, e->strokeBrand) : L"None", [this, eid, edit, colourMenu] {
            const dz::Element* x = designDoc_.find(eid);
            if (!x) return;
            colourMenu(x->stroke, x->strokeBrand, [edit](su::Rgba c, std::string b) {
                edit([c, b](dz::Element& z) {
                    z.stroke = c;
                    z.strokeBrand = c.a ? b : "";
                    if (c.a && z.strokeWidth <= 0) z.strokeWidth = 4;
                });
            }, true);
        });
        y += 36;
        if (e->stroke.a) sliderRow("sw", L"Thickness", e->strokeWidth, 0, 40, L" px", [](dz::Element& x, float v) { x.strokeWidth = v; });
    }
    if (e->kind == dz::Kind::Rect || e->kind == dz::Kind::Image)
        sliderRow("rad", L"Corners", e->radius, 0, std::max(1.0f, std::min(e->w, e->h) / 2), L" px", [](dz::Element& x, float v) { x.radius = v; });
    // Shadow.
    {
        label(L"Shadow");
        toggle("dz:p:shadow", r.x + 112, y + 2, e->shadow.on, [edit] { edit([](dz::Element& z) { z.shadow.on = !z.shadow.on; }); });
        if (e->shadow.on)
            button("dz:p:shcol", {r.x + 170, y - 2, r.w - 170, 30}, colourName(e->shadow.colour, e->shadow.brand), [this, eid, edit, colourMenu] {
                const dz::Element* x = designDoc_.find(eid);
                if (!x) return;
                colourMenu(x->shadow.colour, x->shadow.brand, [edit](su::Rgba c, std::string b) {
                    edit([c, b](dz::Element& z) {
                        z.shadow.colour = c;
                        z.shadow.brand = b;
                    });
                }, false);
            });
        y += 36;
        if (e->shadow.on) {
            sliderRow("shb", L"Softness", e->shadow.blur, 0, 60, L" px", [](dz::Element& x, float v) { x.shadow.blur = v; });
            sliderRow("shy", L"Down", e->shadow.dy, -40, 40, L" px", [](dz::Element& x, float v) { x.shadow.dy = v; });
        }
    }
    // Timer.
    if (e->kind == dz::Kind::Timer) {
        static const char* modes[] = {"countdown", "until", "stopwatch", "live", "clock", "golive", "remaining", "next"};
        static const wchar_t* modeNames[] = {L"Countdown for a length", L"Countdown to a time", L"Stopwatch", L"Time live", L"Clock",
                                             L"Until going live", L"Time left in the stream", L"Next stream"};
        constexpr int kModes = 8;
        int mi = 0;
        for (int i = 0; i < kModes; ++i)
            if (e->timerMode == modes[i]) mi = i;
        label(L"Shows");
        button("dz:p:tmode", {r.x + 112, y - 2, r.w - 112, 30}, modeNames[mi], [this, eid, edit] {
            std::vector<MenuItem> items;
            const dz::Element* x = designDoc_.find(eid);
            for (int i = 0; i < kModes; ++i) {
                if (i == 5) items.push_back(MenuItem::sep());
                items.push_back(MenuItem::item(modeNames[i], 100 + (UINT)i, {}, true, x && x->timerMode == modes[i]));
            }
            POINT pt;
            GetCursorPos(&pt);
            const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
            if (cmd >= 100 && cmd < 100 + kModes) edit([cmd](dz::Element& z) { z.timerMode = modes[cmd - 100]; });
        });
        y += 36;
        if (e->timerMode == "countdown" || e->timerMode == "until") {
            label(e->timerMode == "countdown" ? L"Length" : L"Until");
            wchar_t b[32];
            if (e->timerMode == "countdown") swprintf(b, 32, L"%ls", widen(dz::durationText((std::int64_t)e->timerSeconds * 1000)).c_str());
            else swprintf(b, 32, L"%02d:%02d", e->timerUntil / 60, e->timerUntil % 60);
            button("dz:p:tval", {r.x + 112, y - 2, r.w - 112, 30}, b, [this, eid] {
                const dz::Element* x = designDoc_.find(eid);
                if (!x) return;
                const bool cd = x->timerMode == "countdown";
                askText(cd ? L"How long: minutes, or h:mm:ss / m:ss" : L"The time to count to (24-hour, e.g. 20:00)",
                        cd ? widen(dz::durationText((std::int64_t)x->timerSeconds * 1000)) : L"20:00", [this, eid, cd](const std::wstring& v) {
                            // "5" = 5 minutes; "4:30" = m:ss (or hh:mm for a time); "1:00:00" = h:mm:ss.
                            int parts[3] = {0, 0, 0}, n = 0;
                            std::string t = narrow(v);
                            std::stringstream ss(t);
                            std::string p;
                            while (std::getline(ss, p, ':') && n < 3) {
                                if (p.empty() || p.find_first_not_of("0123456789 ") != std::string::npos) return flash(L"Type it like 5, 4:30 or 20:00.", 5);
                                parts[n++] = std::atoi(p.c_str());
                            }
                            dz::Element* z = designDoc_.find(eid);
                            if (!z || n == 0) return;
                            if (cd) {
                                const int secs = n == 1 ? parts[0] * 60 : n == 2 ? parts[0] * 60 + parts[1] : parts[0] * 3600 + parts[1] * 60 + parts[2];
                                if (secs < 1 || secs > 359999) return flash(L"Between one second and 99 hours.", 5);
                                z->timerSeconds = secs;
                            } else {
                                const int mins = n == 1 ? parts[0] * 60 : parts[0] * 60 + parts[1];
                                if (n > 2 || mins < 0 || mins >= 24 * 60 || (n == 2 && parts[1] > 59)) return flash(L"A time like 20:00.", 5);
                                z->timerUntil = mins;
                            }
                            designChanged();
                        });
            });
            y += 36;
            label(L"At zero");
            button("dz:p:tend", {r.x + 112, y - 2, r.w - 112, 30}, L"Shows: " + widen(e->timerEnd), [this, eid] {
                const dz::Element* x = designDoc_.find(eid);
                if (!x) return;
                askText(L"What it shows at zero", widen(x->timerEnd), [this, eid](const std::wstring& v) {
                    if (dz::Element* z = designDoc_.find(eid)) z->timerEnd = narrow(v).substr(0, 200);
                    designChanged();
                });
            });
            y += 36;
            const Scene* target = scenes_.scene(e->timerScene);
            label(L"Then");
            button("dz:p:tscene", {r.x + 112, y - 2, r.w - 112, 30}, target ? L"Switch to " + widen(target->name) : L"Stay on this scene", [this, eid, edit] {
                std::vector<MenuItem> items = {MenuItem::item(L"Stay on this scene", 1)};
                std::vector<Id> ids;
                for (auto& sc : scenes_.scenes()) {
                    ids.push_back(sc.id);
                    items.push_back(MenuItem::item(L"Switch to " + widen(sc.name), 100 + (UINT)(ids.size() - 1)));
                }
                POINT pt;
                GetCursorPos(&pt);
                const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
                if (cmd == 1) edit([](dz::Element& z) { z.timerScene.clear(); });
                else if (cmd >= 100 && cmd < 100 + ids.size()) {
                    const Id to = ids[cmd - 100];
                    edit([to](dz::Element& z) { z.timerScene = to; });
                }
            });
            y += 36;
            label(L"Last 10 s");
            toggle("dz:p:tpulse", r.x + 112, y + 2, e->timerPulse, [edit] { edit([](dz::Element& z) { z.timerPulse = !z.timerPulse; }); });
            gfx_.text(L"pulse", {r.x + 170, y, 100, 26}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
            y += 36;
        }
        if (e->timerMode == "golive" || e->timerMode == "remaining" || e->timerMode == "next") {
            const std::wstring note = e->timerMode == "golive"    ? L"Counts down to the time set to go live, in Settings \u203A Streaming \u203A Your stream."
                                      : e->timerMode == "remaining" ? L"Counts down from the stream's length (Settings \u203A Streaming \u203A Your stream) while you're live."
                                                                    : L"Shows the next stream's date and time from Settings \u203A Streaming \u203A Your stream.";
            y += gfx_.text(note, {r.x, y, r.w, 60}, Font::Small, pal_.inkMuted) + 8;
            if (e->timerMode != "next") {
                label(L"Last 10 s");
                toggle("dz:p:tpulse2", r.x + 112, y + 2, e->timerPulse, [edit] { edit([](dz::Element& z) { z.timerPulse = !z.timerPulse; }); });
                gfx_.text(L"pulse", {r.x + 170, y, 100, 26}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
                y += 36;
            }
        }
        if (e->timerMode == "clock") {
            label(L"Date");
            toggle("dz:p:tdate", r.x + 112, y + 2, e->timerDate, [edit] { edit([](dz::Element& z) { z.timerDate = !z.timerDate; }); });
            y += 36;
        }
        button("dz:p:twords", {r.x, y, r.w, 30}, e->text.empty() ? L"Words around it\u2026" : L"Words: " + widen(e->text), [this, eid] {
            const dz::Element* x = designDoc_.find(eid);
            if (!x) return;
            askText(L"Words around the time ({time} is where it goes; empty for the time alone)", widen(x->text.empty() ? "Starting in {time}" : x->text),
                    [this, eid](const std::wstring& v) {
                        if (dz::Element* z = designDoc_.find(eid)) z->text = narrow(v).substr(0, 200);
                        designChanged();
                    });
        });
        y += 38;
        sliderRow("size", L"Size", e->size, 8, 300, L" px", [](dz::Element& x, float v) { x.size = v; });
        label(L"Style");
        {
            const float bw = (r.w - 112 - 8) / 2;
            button("dz:p:bold", {r.x + 112, y - 2, bw, 30}, L"Bold", [edit] { edit([](dz::Element& z) { z.bold = !z.bold; }); }, e->bold);
            button("dz:p:font", {r.x + 112 + bw + 8, y - 2, bw, 30}, L"Font", [this, eid] {
                const dz::Element* x = designDoc_.find(eid);
                if (!x) return;
                pickFont(x->font, true, [this, eid](const std::string& f) {
                    if (dz::Element* z = designDoc_.find(eid)) z->font = f.substr(0, 64);
                    designChanged();
                });
            });
        }
        y += 36;
    }
    if (e->kind == dz::Kind::Qr) {
        button("dz:p:qr", {r.x, y, r.w, 30}, L"Link: " + widen(e->qr), [this, eid] {
            const dz::Element* x = designDoc_.find(eid);
            if (!x) return;
            askText(L"The link or text it holds ({website}, {twitch}\u2026 work too)", widen(x->qr), [this, eid](const std::wstring& v) {
                if (dz::Element* z = designDoc_.find(eid)) z->qr = narrow(v).substr(0, 1000);
                designChanged();
            });
        });
        y += 38;
        y += gfx_.text(L"Fill is its light colour, Outline its dark one. Keep good contrast so phones can read it.", {r.x, y - 4, r.w, 36}, Font::Small,
                       pal_.inkFaint) + 6;
    }
    if (e->kind == dz::Kind::Icon) {
        const dz::Icon* ic = dz::iconById(e->icon);
        button("dz:p:icon", {r.x, y, r.w, 30}, L"Icon: " + (ic ? widen(ic->name) : std::wstring(L"?")), [this, edit] {
            std::vector<MenuItem> items;
            const auto& ics = dz::icons();
            for (std::size_t i = 0; i < ics.size(); ++i) items.push_back(MenuItem::item(widen(ics[i].name), 100 + (UINT)i));
            POINT pt;
            GetCursorPos(&pt);
            const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
            if (cmd < 100 || cmd >= 100 + ics.size()) return;
            const std::string id = ics[cmd - 100].id;
            edit([id](dz::Element& z) { z.icon = id; });
        });
        y += 38;
    }
    // Chat.
    if (e->kind == dz::Kind::Chat) {
        y += gfx_.text(chatStatusText(), {r.x, y, r.w, 80}, Font::Small, pal_.inkMuted, {Align::Left, true}) + 10;
        sliderRow("chn", L"Messages", (float)e->chatCount, 1, 30, L"", [](dz::Element& x, float v) { x.chatCount = (int)std::lround(v); });
        sliderRow("chf", L"Fade after", (float)e->chatFade, 0, 300, L" s", [](dz::Element& x, float v) { x.chatFade = (int)std::lround(v); });
        if (e->chatFade == 0) y += gfx_.text(L"Fade after 0 s: messages stay until newer ones push them out.", {r.x, y - 4, r.w, 18}, Font::Small, pal_.inkFaint) + 6;
        label(L"Cards");
        toggle("dz:p:chb", r.x + 112, y + 2, e->chatBubbles, [edit] { edit([](dz::Element& z) { z.chatBubbles = !z.chatBubbles; }); });
        if (e->chatBubbles)
            button("dz:p:chbc", {r.x + 170, y - 2, r.w - 170, 30}, colourName(e->chatBubble, e->chatBubbleBrand), [this, eid, edit, colourMenu] {
                const dz::Element* x = designDoc_.find(eid);
                if (!x) return;
                colourMenu(x->chatBubble, x->chatBubbleBrand, [edit](su::Rgba c, std::string b) {
                    edit([c, b](dz::Element& z) {
                        z.chatBubble = c;
                        z.chatBubbleBrand = b;
                    });
                }, false);
            });
        y += 36;
        label(L"Bubbles");
        {
            // Unset (designs from before 0.10.27): shown as what Align does, Left or Right.
            const int now = e->chatSide == "mine" ? 2 : e->chatSide == "right" ? 1 : e->chatSide == "left" ? 0 : e->align == "right" ? 1 : e->align == "centre" ? -1 : 0;
            segmented("dz:p:chside", {r.x + 112, y - 2, r.w - 112, 30}, {L"Left", L"Right", L"Split"}, now,
                      [edit](int i) { edit([i](dz::Element& z) { z.chatSide = i == 2 ? "mine" : i == 1 ? "right" : "left"; }); });
        }
        y += 36;
        if (e->chatSide == "mine")
            y += gfx_.text(L"Split: your own messages (from your channel) sit on the right, everyone else's on the left, like messages on a phone.",
                           {r.x, y - 4, r.w, 36}, Font::Small, pal_.inkFaint, {Align::Left, true}) + 6;
        label(L"Names");
        segmented("dz:p:chnc", {r.x + 112, y - 2, r.w - 112, 30}, {L"Viewers' colours", L"Brand colour"}, e->chatNameColours ? 0 : 1,
                  [edit](int i) { edit([i](dz::Element& z) { z.chatNameColours = i == 0; }); });
        y += 36;
        y += gfx_.text(L"Read-only: Capture reads your chat without signing in and never posts. Deleted messages, timeouts and bans "
                       L"disappear here too. Emotes show as their names.",
                       {r.x, y, r.w, 60}, Font::Small, pal_.inkFaint, {Align::Left, true}) + 8;
    }
    // Text.
    if (e->kind == dz::Kind::Text || e->kind == dz::Kind::Chat) {
        if (e->kind == dz::Kind::Text) {
        button("dz:p:text", {r.x, y, r.w, 30}, L"Edit the text…", [this, eid] {
            const dz::Element* x = designDoc_.find(eid);
            if (!x) return;
            std::string t = x->text;
            for (std::size_t at = 0; (at = t.find('\n', at)) != std::string::npos;) t.replace(at, 1, "\\n");
            askText(L"The text (\\n starts a new line; {name}, {twitch}… fill in your details)", widen(t), [this, eid](const std::wstring& v) {
                std::string t2 = narrow(v);
                for (std::size_t at = 0; (at = t2.find("\\n", at)) != std::string::npos;) t2.replace(at, 2, "\n");
                if (dz::Element* z = designDoc_.find(eid)) z->text = t2;
                designChanged();
            });
        });
        y += 38;
        }
        sliderRow("size", L"Size", e->size, 8, 300, L" px", [](dz::Element& x, float v) { x.size = v; });
        label(L"Style");
        {
            const float bw = (r.w - 112 - 16) / 3;
            button("dz:p:bold", {r.x + 112, y - 2, bw, 30}, L"Bold", [edit] { edit([](dz::Element& z) { z.bold = !z.bold; }); }, e->bold);
            button("dz:p:italic", {r.x + 112 + bw + 8, y - 2, bw, 30}, L"Italic",
                   [edit] { edit([](dz::Element& z) { z.italic = !z.italic; }); }, e->italic);
            button("dz:p:font", {r.x + 112 + (bw + 8) * 2, y - 2, bw, 30}, L"Font", [this, eid] {
                const dz::Element* x = designDoc_.find(eid);
                if (!x) return;
                pickFont(x->font, true, [this, eid](const std::string& f) {
                    if (dz::Element* z = designDoc_.find(eid)) z->font = f.substr(0, 64);
                    designChanged();
                });
            });
        }
        y += 36;
        if (e->kind == dz::Kind::Text || e->chatSide.empty()) {   // chat placed by Bubbles (above) has no Align
            segmented("dz:p:align", {r.x + 112, y - 2, r.w - 112, 30}, {L"Left", L"Centre", L"Right"},
                      e->align == "centre" ? 1 : e->align == "right" ? 2 : 0, [edit](int i) {
                          edit([i](dz::Element& z) { z.align = i == 1 ? "centre" : i == 2 ? "right" : "left"; });
                      });
            label(L"Align");
            y += 36;
        }
    }
    if (e->kind == dz::Kind::Image) {
        button("dz:p:file", {r.x, y, r.w - 120, 30}, e->file == dz::kBrandLogo ? L"Brand logo" : L"Another picture…", [this, edit, eid] {
            const dz::Element* x = designDoc_.find(eid);
            if (!x) return;
            const bool hasLogo = !video::designBrand().logo.empty();
            std::vector<MenuItem> items = {MenuItem::item(L"A picture file…", 1),
                                           MenuItem::item(L"Your brand logo", 2, hasLogo ? L"" : L"set it under Brand kit…", true, x->file == dz::kBrandLogo)};
            POINT pt;
            GetCursorPos(&pt);
            const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
            if (cmd == 2) edit([](dz::Element& z) { z.file = dz::kBrandLogo; });
            if (cmd != 1) return;
            std::wstring path;
            if (!pickImageFile(path)) return;
            const std::string f = narrow(path);
            edit([f](dz::Element& z) { z.file = f; });
        });
        button("dz:p:cover", {r.right() - 112, y, 112, 30}, e->cover ? L"Fill the box" : L"Fit inside", [edit] { edit([](dz::Element& z) { z.cover = !z.cover; }); });
        y += 38;
    }
    // Entrance.
    {
        label(L"Entrance");
        button("dz:p:anim", {r.x + 112, y - 2, r.w - 112, 30}, kAnimNames[(int)e->anim], [this, eid, edit] {
            const dz::Element* x = designDoc_.find(eid);
            if (!x) return;
            std::vector<MenuItem> items;
            for (int i = 0; i < 8; ++i) items.push_back(MenuItem::item(kAnimNames[i], 100 + (UINT)i, {}, true, (int)x->anim == i));
            POINT pt;
            GetCursorPos(&pt);
            const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
            if (cmd >= 100 && cmd < 108) edit([cmd](dz::Element& z) { z.anim = (dz::Anim)(cmd - 100); });
        });
        y += 36;
        if (e->anim != dz::Anim::None) {
            sliderRow("animms", L"Taking", (float)e->animMs, 100, 3000, L" ms", [](dz::Element& x, float v) { x.animMs = (int)v; });
            y += gfx_.text(L"Plays when this design's scene goes live.", {r.x, y - 4, r.w, 18}, Font::Small, pal_.inkFaint) + 6;
        }
    }
    // Layer.
    {
        const float bw = (r.w - 24) / 4;
        button("dz:p:up", {r.x, y, bw, 30}, L"Up", [this, eid] { if (dz::move(designDoc_, eid, 1)) designChanged(); });
        button("dz:p:down", {r.x + bw + 8, y, bw, 30}, L"Down", [this, eid] { if (dz::move(designDoc_, eid, -1)) designChanged(); });
        button("dz:p:dup", {r.x + (bw + 8) * 2, y, bw, 30}, L"Copy", [this, eid] {
            designSel_ = dz::duplicate(designDoc_, eid);
            designChanged();
        });
        button("dz:p:del", {r.x + (bw + 8) * 3, y, bw, 30}, L"Delete", [this, eid] {
            dz::remove(designDoc_, eid);
            designSel_.clear();
            designChanged();
        });
        y += 38;
    }
}

// ---------------------------------------------------------------- timers reaching zero

// Chat runs only while it's needed: a design in the collection has a chat element, and Your details
// has a Twitch name. Then Capture joins that channel's chat anonymously (read-only).
void CaptureWindow::chatTick() {
    bool needed = false;
    for (auto& s : scenes_.sources())
        if (s.kind == SourceKind::Design && s.settings.find("\"chat\"") != std::string::npos && dz::usesChat(dz::fromJson(s.settings))) {
            needed = true;
            break;
        }
    if (!designFor_.empty() && dz::usesChat(designDoc_)) needed = true;
    if (layout_.shows(Layout::Chat)) needed = true;
    std::string twitch;
    for (auto& [k, v] : profile_)
        if (k == "twitch") twitch = v;
    chat_.watch(needed ? chat::channelFrom(twitch) : std::string());
}

// The Chat panel: the newest messages at the bottom (as many as fit), each with its name in the
// viewer's colour (made readable on the panel) and what they are (streamer, mod, VIP, sub). Read-only;
// "Reply on Twitch" opens Twitch's own chat window in the browser.
void CaptureWindow::drawChatPanel(const Rect& b) {
    std::string twitch;
    for (auto& [k, v] : profile_)
        if (k == "twitch") twitch = v;
    const std::string channel = chat::channelFrom(twitch);
    const float pad = 12;
    float bottom = b.bottom() - 8;
    if (!channel.empty()) {
        const Rect rb{b.x + pad, bottom - 30, b.w - 2 * pad, 30};
        button("chat:reply", rb, L"Reply on Twitch\u2026", [this, channel] {
            const std::wstring url = L"https://www.twitch.tv/popout/" + widen(channel) + L"/chat";
            ShellExecuteW(hwnd(), L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        });
        bottom -= 38;
    }
    const auto st = chat_.status();
    std::wstring status;
    if (channel.empty())
        status = twitch.empty() ? L"Add your Twitch name under Your details\u2026 (in any design) to read your chat here. Read-only: no sign-in."
                                : L"\u201C" + widen(twitch) + L"\u201D isn't a Twitch name or link.";
    else if (st.state == output::TwitchChat::State::Connected) status = L"#" + widen(channel) + L" \u00B7 live \u00B7 read-only";
    else if (st.state == output::TwitchChat::State::Waiting) status = widen(st.error) + L". Trying again in " + std::to_wstring(st.retryInS) + L" s.";
    else status = L"Connecting to #" + widen(channel) + L"\u2026";
    const float sh = gfx_.measureHeight(status, b.w - 2 * pad, Font::Small);
    gfx_.text(status, {b.x + pad, b.y + 8, b.w - 2 * pad, sh}, Font::Small, st.state == output::TwitchChat::State::Waiting ? pal_.live : pal_.inkFaint);
    const float top = b.y + 8 + sh + 8;
    const auto msgs = video::designChat();
    if (msgs.empty() && !channel.empty() && st.state == output::TwitchChat::State::Connected) {
        gfx_.text(L"No messages yet. New ones appear here as they're sent.", {b.x + pad, top, b.w - 2 * pad, 40}, Font::Small, pal_.inkMuted);
        return;
    }
    // Readable name colours: lighter on the dark theme, darker on the light one.
    const Color surf = pal_.surface;
    const bool darkTheme = 0.2126f * surf.r + 0.7152f * surf.g + 0.0722f * surf.b < 0.5f;
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
        const std::wstring body = widen(it->text);
        const float th = gfx_.measureHeight(body.empty() ? L" " : body, b.w - 2 * pad, Font::Body);
        const float h = 19 + th + 8;
        if (bottom - h < top) break;
        const float y = bottom - h;
        const su::Rgba rc = it->colour.a ? it->colour : chat::defaultColour(it->login);
        Color nc{rc.r / 255.f, rc.g / 255.f, rc.b / 255.f, 1};
        const float lum = 0.2126f * nc.r + 0.7152f * nc.g + 0.0722f * nc.b;
        if (darkTheme && lum < 0.5f) nc = nc.mix(Color{1, 1, 1, 1}, 0.45f);
        if (!darkTheme && lum > 0.55f) nc = nc.mix(Color{0, 0, 0, 1}, 0.4f);
        std::wstring role;
        for (auto& bd : it->badges) {
            const wchar_t* r = bd == "broadcaster" ? L"streamer" : bd == "moderator" ? L"mod" : bd == "vip" ? L"VIP" : bd == "subscriber" ? L"sub" : nullptr;
            if (r) role += (role.empty() ? L"" : L", ") + std::wstring(r);
        }
        const std::wstring name = widen(it->name);
        const float nw = std::min(gfx_.measureWidth(name, Font::BodyStrong) + 2, b.w - 2 * pad);
        gfx_.text(name, {b.x + pad, y, nw, 19}, Font::BodyStrong, nc, {Align::Left, false, true});
        if (!role.empty()) gfx_.text(role, {b.x + pad + nw + 8, y + 1, b.w - 2 * pad - nw - 8, 18}, Font::Small, pal_.inkFaint, {Align::Left, false, true});
        gfx_.text(body, {b.x + pad, y + 19, b.w - 2 * pad, th + 2}, Font::Body, it->action ? nc : pal_.ink);
        bottom = y;
    }
}

// What the chat is doing, for the designer.
std::wstring CaptureWindow::chatStatusText() const {
    std::string twitch;
    for (auto& [k, v] : profile_)
        if (k == "twitch") twitch = v;
    if (chat::channelFrom(twitch).empty())
        return twitch.empty() ? L"Add your Twitch name under Your details\u2026 and this shows your chat, live (read-only, no sign-in). Sample messages till then."
                              : L"\u201C" + widen(twitch) + L"\u201D isn't a Twitch name or link: fix it under Your details\u2026";
    const auto st = chat_.status();
    switch (st.state) {
    case output::TwitchChat::State::Connected:
        return L"Showing #" + widen(st.channel) + L"'s chat, live (" + std::to_wstring(st.messages) + L" messages so far).";
    case output::TwitchChat::State::Connecting: return L"Connecting to #" + widen(st.channel) + L"'s chat\u2026";
    case output::TwitchChat::State::Waiting:
        return L"Chat: " + widen(st.error) + L". Trying again in " + std::to_wstring(st.retryInS) + L" s.";
    default: return L"Chat connects within a second or two.";
    }
}

void CaptureWindow::timerTick() {
    const Scene* sc = scenes_.scene(programScene());
    if (!sc) return;
    for (auto& it : sc->items) {
        const Source* src = scenes_.source(it.source);
        if (!src || src->kind != SourceKind::Design || !it.visible) continue;
        auto ent = designEntered_.find(src->id);
        if (ent == designEntered_.end()) continue;
        const dz::Document d = dz::fromJson(src->settings);
        if (!dz::usesTime(d)) continue;
        const dz::Clock clock = video::clockNow((std::int64_t)(GetTickCount64() - ent->second));
        for (auto& e : d.elements) {
            if (e.kind != dz::Kind::Timer || !e.visible || e.timerScene.empty() || !scenes_.scene(e.timerScene)) continue;
            if (!dz::timerText(e, clock).ended) continue;
            const std::string key = src->id + "/" + e.id + "/" + std::to_string(ent->second);
            if (!timersFired_.insert(key).second) continue;   // once per time its scene goes live
            log::info("Timer reached zero: switching to " + scenes_.scene(e.timerScene)->name);
            if (settings_.studioMode) programScene_ = e.timerScene;
            else scenes_.setCurrent(e.timerScene);
            scenesChanged();
            return;
        }
    }
}

}  // namespace ncap
