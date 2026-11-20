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
    for (const char* k : {"chapter", "chapterwords", "chapterline", "words", "goal", "today", "bookwords", "bookline", "streak", "streakline",
                          "status", "target", "targetline", "viewpoint", "viewpointpic", "place", "placepic", "storyday", "sprintwords", "sprintline",
                          "streamwords", "streamline", "cast1", "cast2", "cast3", "cast4", "cast5", "cast6", "cast1pic", "cast2pic", "cast3pic",
                          "cast4pic", "cast5pic", "cast6pic"})
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
    // Going live or starting a recording: "words this stream" starts again, so the summary is read afresh.
    const bool liveNow = streamer_.active() || recorder_.active();
    if (liveNow != writingLive_) {
        writingLive_ = liveNow;
        streamWordsBase_ = -1;
        writingStamp_ = -1;
    }
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::path(profilePath()).parent_path() / L"Writing Studio" / L"summary.json";
    const auto t = std::filesystem::last_write_time(p, ec);
    const long long stamp = ec ? 0 : (long long)t.time_since_epoch().count();
    if (stamp == writingStamp_) return;
    const bool firstRead = writingStamp_ == -1 && !writingSeen_;
    writingStamp_ = stamp;
    std::vector<std::pair<std::string, std::string>> vals;
    std::int64_t sprintEnds = -1;
    std::string worldColour;
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
            auto integer = [&](const nlohmann::json& o, const char* k) { return o.contains(k) && o[k].is_number_integer() ? o[k].get<long long>() : 0LL; };
            auto wordsText = [&](long long n, const char* after) { return grouped(n) + (n == 1 ? " word" : " words") + after; };
            const long long goal = std::max(0LL, integer(j, "dailyGoal"));
            const std::string chapter = str("chapter");
            vals.push_back({"chapter", chapter});
            vals.push_back({"words", grouped(words)});
            vals.push_back({"goal", goal ? grouped(goal) : std::string()});
            vals.push_back({"today", grouped(words) + (goal ? " of " + grouped(goal) : std::string()) + (words == 1 && !goal ? " word today" : " words today")});
            const long long book = integer(j, "bookWords"), streak = integer(j, "streak"), chapterWords = integer(j, "chapterWords");
            vals.push_back({"chapterwords", j.contains("chapterWords") ? grouped(chapterWords) : std::string()});
            vals.push_back({"chapterline", j.contains("chapterWords") ? wordsText(chapterWords, "") : std::string()});
            vals.push_back({"bookwords", j.contains("bookWords") ? grouped(book) : std::string()});
            vals.push_back({"bookline", j.contains("bookWords") ? wordsText(book, " in the book") : std::string()});
            vals.push_back({"streak", j.contains("streak") ? std::to_string(streak) : std::string()});
            vals.push_back({"streakline", streak > 0 ? std::to_string(streak) + "-day streak" : std::string()});
            // The chapter's planning (0.13): status and target always; the story's details unless spoiler-safe.
            const long long target = integer(j, "target");
            vals.push_back({"status", str("status")});
            vals.push_back({"target", target > 0 ? grouped(target) : std::string()});
            vals.push_back({"targetline", target > 0 && j.contains("chapterWords") ? grouped(chapterWords) + " of " + grouped(target) + " words" : std::string()});
            const bool safe = settings_.writing.spoilerSafe;
            auto named = [&](const char* k, const char* field) {
                return !safe && j.contains(k) && j[k].is_object() && j[k].contains(field) && j[k][field].is_string() ? j[k][field].get<std::string>().substr(0, 1024)
                                                                                                                      : std::string();
            };
            vals.push_back({"viewpoint", named("viewpoint", "name")});
            vals.push_back({"viewpointpic", named("viewpoint", "picture")});
            vals.push_back({"place", named("place", "name")});
            vals.push_back({"placepic", named("place", "picture")});
            vals.push_back({"storyday", safe ? std::string() : str("storyDay")});
            for (int i = 0; i < 6; ++i) {
                std::string n, pic;
                if (!safe && j.contains("cast") && j["cast"].is_array() && i < (int)j["cast"].size() && j["cast"][(size_t)i].is_object()) {
                    const auto& c = j["cast"][(size_t)i];
                    n = c.value("name", "").substr(0, 200);
                    pic = c.value("picture", "").substr(0, 1024);
                }
                vals.push_back({"cast" + std::to_string(i + 1), n});
                vals.push_back({"cast" + std::to_string(i + 1) + "pic", pic});
            }
            // The sprint running now.
            long long sprintWords = 0;
            if (j.contains("sprint") && j["sprint"].is_object()) {
                sprintEnds = integer(j["sprint"], "endsAt");
                sprintWords = integer(j["sprint"], "words");
            }
            vals.push_back({"sprintwords", sprintEnds > 0 ? grouped(sprintWords) : std::string()});
            vals.push_back({"sprintline", sprintEnds > 0 ? wordsText(sprintWords, " this sprint") : std::string()});
            // Words this stream (or recording): from what today's count was when it began.
            if (writingLive_) {
                if (streamWordsBase_ < 0) streamWordsBase_ = words;
                else if (words < streamWordsLast_) streamWordsBase_ = -(streamWordsLast_ - streamWordsBase_);   // past midnight: carry on from 0
                streamWordsLast_ = words;
            }
            const long long streamWords = writingLive_ ? std::max(0LL, words - streamWordsBase_) : 0;
            vals.push_back({"streamwords", writingLive_ ? grouped(streamWords) : std::string()});
            vals.push_back({"streamline", writingLive_ ? wordsText(streamWords, " this stream") : std::string()});
            worldColour = str("worldColour");
            // The stream report's writing figures.
            if (report_.running()) report_.writing(words, chapter);
            // Celebrations: the daily goal reached, and a sprint finished (not what had already happened when Capture opened).
            const std::string day = str("day");
            if (!firstRead && settings_.writing.celebrate && goal > 0 && day == writingDay_ && writingWords_ < goal && words >= goal)
                celebrateWriting("Daily goal reached: " + wordsText(words, " today!"));
            long long sprintAt = 0;
            if (j.contains("lastSprint") && j["lastSprint"].is_object()) {
                const auto& ls = j["lastSprint"];
                sprintAt = integer(ls, "at");
                if (!firstRead && sprintAt && sprintAt != writingSprintAt_) {
                    const long long w = integer(ls, "words");
                    if (report_.running()) report_.sprint(w);
                    if (settings_.writing.celebrate && ls.value("finished", false)) {
                        const long long m = integer(ls, "minutes");
                        celebrateWriting("Sprint done: " + wordsText(w, "") + " in " + std::to_string(m) + (m == 1 ? " minute!" : " minutes!"));
                    }
                }
            }
            writingSprintAt_ = sprintAt;
            writingDay_ = day;
            writingWords_ = words;
            // Writing Studio's workspace: its scene, when set (only on a change, so a scene picked by hand stays).
            const std::string ws = str("workspace");
            if (!firstRead && !ws.empty() && !writingWorkspace_.empty() && ws != writingWorkspace_ && settings_.writing.scenes) {
                const std::string key = ws == "writing" || ws == "planning" || ws == "worldbuilding" ? ws : "other";
                auto it = settings_.writing.sceneFor.find(key);
                if (it != settings_.writing.sceneFor.end() && !it->second.empty()) switchToSceneNamed(it->second, L"Writing Studio");
            }
            writingWorkspace_ = ws;
            // A recording marker for each chapter moved to.
            const std::string chapterId = str("chapterId");
            if (!firstRead && settings_.writing.markers && recorder_.active() && !chapterId.empty() && !writingChapterId_.empty() &&
                chapterId != writingChapterId_ && markers_.size() < 2000) {
                const double secs = recorder_.status().seconds;
                markers_.push_back({(std::int64_t)(secs * 1e6), chapter.empty() ? std::string("Chapter") : chapter});
            }
            writingChapterId_ = chapterId;
        }
    }
    writingSeen_ = true;
    video::setWritingSprint(sprintEnds);
    video::setWorldColour(settings_.writing.worldColour ? worldColour : std::string());
    if (vals == writingValues_) return;
    writingValues_ = vals;
    loadProfile();
    designRendered_.clear();
    invalidate();
}

// A celebration: its words for {celebration}, and every design shown only for celebrations comes in afresh.
void CaptureWindow::celebrateWriting(const std::string& text) {
    video::celebrate(text);
    for (auto& src : scenes_.sources())
        if (src.kind == SourceKind::Design && src.settings.find("\"onlyCelebrating\":true") != std::string::npos)
            if (auto l = live_.find(src.id); l != live_.end() && l->second.capture) l->second.capture->command("enter");
    log::info("Celebration: " + text);
}

// Switches the live scene to one named (in studio mode, the program), with a short note saying why.
void CaptureWindow::switchToSceneNamed(const std::string& name, const std::wstring& why) {
    const Scene* target = nullptr;
    for (auto& sc : scenes_.scenes())
        if (sc.name == name) target = &sc;
    if (!target) {
        flash(why + L": there's no scene called " + widen(name) + L" in this scene collection.", 6);
        return;
    }
    const Id current = settings_.studioMode ? programScene_ : scenes_.current();
    if (current == target->id) return;
    if (settings_.studioMode) programScene_ = target->id;
    else scenes_.setCurrent(target->id);
    scenesChanged();
    flash(why + L": switched to " + widen(name) + L".", 4);
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
    dPropScroll_ = 0;
    designDoc_ = dz::fromJson(s->settings);
    designSel_.clear();
    designMulti_.clear();
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

// What's selected: several (0.14), or the one.
std::vector<std::string> CaptureWindow::designSelection() const {
    std::vector<std::string> out;
    for (auto& id : designMulti_)
        if (designDoc_.find(id)) out.push_back(id);
    if (out.empty() && designDoc_.find(designSel_)) out.push_back(designSel_);
    return out;
}

void CaptureWindow::designSelect(std::vector<std::string> ids, const std::string& primary) {
    designMulti_ = std::move(ids);
    designSel_ = primary.empty() && !designMulti_.empty() ? designMulti_.back() : primary;
    if (designMulti_.size() <= 1) designMulti_.clear();
    invalidate();
}

void CaptureWindow::designGroupSelected(bool make) {
    const auto sel = designSelection();
    if (make) {
        if (sel.size() < 2) { flash(L"Select two or more elements to group (Shift-click, or drag a box round them).", 5); return; }
        const std::string g = dz::group(designDoc_, sel);
        if (g.empty()) return;
        designChanged();
        designSelect(dz::members(designDoc_, g), designSel_);
        flash(L"Grouped: they move, line up and hide as one. Alt-click picks one inside.", 5);
    } else {
        std::set<std::string> gs;
        for (auto& id : sel)
            if (const dz::Element* e = designDoc_.find(id); e && !e->group.empty()) gs.insert(e->group);
        if (gs.empty()) return;
        for (auto& g : gs) dz::ungroup(designDoc_, g);
        designChanged();
        invalidate();
    }
}

bool CaptureWindow::designerMouseDown(float x, float y) {
    if (designFor_.empty()) return false;
    const float sc = designScale_;
    const float px = (x - designCanvas_.x) / sc, py = (y - designCanvas_.y) / sc;
    // The gradient bar (0.15): a click on a stop picks it (drag to move it, pull it off the bar to remove it); a click
    // between stops adds one there, in the gradient's colour at that point.
    if (!dGradFor_.empty() && dGradBar_.contains(x, y)) {
        dz::Element* e = designDoc_.find(dGradFor_);
        if (!e) return false;
        const float t = std::clamp((x - dGradBar_.x) / dGradBar_.w, 0.0f, 1.0f);
        dBefore_ = dz::toJson(designDoc_);
        int s = dz::stopNear(e->fill, t, 9 / dGradBar_.w);
        dStopMoved_ = false;
        if (s < 0) {
            s = dz::addStop(e->fill, t);
            if (s < 0) {
                flash(L"A gradient can have up to 16 colours.", 4);
                return true;
            }
            dStopMoved_ = true;   // a new stop: there's nothing to pick a colour for yet
            designChanged(false);
        }
        dStopSel_ = s;
        dStopOff_ = false;
        dStopDownX_ = x;
        ddrag_ = 5;
        invalidate();
        return true;
    }
    // The rulers (0.14): a click makes a guide there, or picks one up; dragged back onto the ruler, it goes.
    const Rect topRuler{designCanvas_.x, designCanvas_.y - 20, designCanvas_.w, 18}, leftRuler{designCanvas_.x - 20, designCanvas_.y, 18, designCanvas_.h};
    if (topRuler.contains(x, y) || leftRuler.contains(x, y)) {
        const bool across = topRuler.contains(x, y);
        auto& gs = across ? designDoc_.guidesX : designDoc_.guidesY;
        const float at = std::round(across ? px : py);
        dBefore_ = dz::toJson(designDoc_);
        dGuideAxis_ = across ? 0 : 1;
        dGuideIndex_ = -1;
        for (std::size_t i = 0; i < gs.size(); ++i)
            if (std::fabs(gs[i] - at) * sc < 5) dGuideIndex_ = (int)i;
        if (dGuideIndex_ < 0 && gs.size() < 50) {
            gs.push_back(at);
            dGuideIndex_ = (int)gs.size() - 1;
        }
        if (dGuideIndex_ >= 0) ddrag_ = 4;
        invalidate();
        return true;
    }
    // Off the page but near it: a box round what to select (a page covered by a background panel needs it).
    const Rect& around = designCentre_;   // the middle column, between Layers and the properties
    if (!designCanvas_.contains(x, y)) {
        if (!around.contains(x, y)) return false;
        designSelect({}, {});
        ddrag_ = 3;
        dStartX_ = x, dStartY_ = y;
        dMarquee_ = {x, y, 0, 0};
        return true;
    }
    const bool shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    auto sel = designSelection();
    // The selected element's resize handle (bottom right) first: only with one selected.
    if (sel.size() == 1)
        if (const dz::Element* e = designDoc_.find(sel[0]); e && !e->locked) {
            const float hx = designCanvas_.x + (e->x + e->w) * sc, hy = designCanvas_.y + (e->y + e->h) * sc;
            if (std::fabs(x - hx) < 9 && std::fabs(y - hy) < 9) {
                ddrag_ = 2;
                dStartX_ = px, dStartY_ = py;
                dStartBox_ = {e->x, e->y, e->w, e->h};
                dBefore_ = dz::toJson(designDoc_);
                return true;
            }
        }
    const std::string hit = dz::hitTest(designDoc_, px, py);
    // Alt picks one element inside a group; otherwise a click picks its whole group.
    const std::vector<std::string> picked = hit.empty() ? std::vector<std::string>{} : alt ? std::vector<std::string>{hit} : dz::pick(designDoc_, hit);
    if (shift) {   // Shift-click: in or out of the selection
        if (!hit.empty()) {
            const bool in = std::find(sel.begin(), sel.end(), hit) != sel.end();
            for (auto& id : picked) {
                auto it = std::find(sel.begin(), sel.end(), id);
                if (in && it != sel.end()) sel.erase(it);
                else if (!in && it == sel.end()) sel.push_back(id);
            }
            designSelect(sel, in ? (sel.empty() ? std::string() : sel.back()) : hit);
        }
        return true;
    }
    if (hit.empty()) {   // a box round what to select
        designSelect({}, {});
        ddrag_ = 3;
        dStartX_ = x, dStartY_ = y;
        dMarquee_ = {x, y, 0, 0};
        return true;
    }
    if (std::find(sel.begin(), sel.end(), hit) == sel.end() || alt) designSelect(picked, hit);
    else designSel_ = hit;
    ddrag_ = 1;
    dStartX_ = px, dStartY_ = py;
    dStartBoxes_.clear();
    for (auto& id : designSelection())
        if (const dz::Element* e = designDoc_.find(id)) dStartBoxes_[id] = {e->x, e->y, e->w, e->h};
    dStartBox_ = dz::boundsOf(designDoc_, designSelection());
    dBefore_ = dz::toJson(designDoc_);
    invalidate();
    return true;
}

bool CaptureWindow::designerMouseMove(float x, float y, bool leftDown) {
    if (designFor_.empty() || ddrag_ == 0) return false;
    if (!leftDown) return designerMouseUp();
    const float px = (x - designCanvas_.x) / designScale_, py = (y - designCanvas_.y) / designScale_;
    const bool shift = GetKeyState(VK_SHIFT) < 0;   // Shift: no snapping
    if (ddrag_ == 5) {   // a gradient stop
        dz::Element* e = designDoc_.find(dGradFor_);
        if (!e || dStopSel_ < 0) return true;
        if (std::fabs(x - dStopDownX_) < 3 && !dStopMoved_) return true;   // still a click
        dStopMoved_ = true;
        // Pulled well above or below the bar: it'll go when let go (if there are more than two).
        dStopOff_ = e->fill.stops.size() > 2 && (y < dGradBar_.y - 26 || y > dGradBar_.bottom() + 26);
        float t = std::clamp((x - dGradBar_.x) / dGradBar_.w, 0.0f, 1.0f);
        if (!shift)   // snaps to whole tenths (Shift: anywhere)
            if (std::fabs(t * 10 - std::round(t * 10)) * dGradBar_.w / 10 < 4) t = std::round(t * 10) / 10;
        dStopSel_ = dz::moveStop(e->fill, dStopSel_, t);
        designChanged(false);
        invalidate();
        return true;
    }
    if (ddrag_ == 3) {
        dMarquee_ = {std::min(x, dStartX_), std::min(y, dStartY_), std::fabs(x - dStartX_), std::fabs(y - dStartY_)};
        invalidate();
        return true;
    }
    if (ddrag_ == 4) {
        auto& gs = dGuideAxis_ == 0 ? designDoc_.guidesX : designDoc_.guidesY;
        if (dGuideIndex_ >= 0 && dGuideIndex_ < (int)gs.size()) gs[(size_t)dGuideIndex_] = std::round(dGuideAxis_ == 0 ? px : py);
        invalidate();
        return true;
    }
    if (ddrag_ == 2) {
        dz::Element* e = designDoc_.find(designSel_);
        if (!e) return true;
        dz::Box b = dStartBox_;
        b.w = std::max(4.0f, b.w + px - dStartX_);
        b.h = std::max(4.0f, b.h + py - dStartY_);
        if (GetKeyState(VK_CONTROL) < 0 && dStartBox_.w > 0) b.h = b.w * dStartBox_.h / dStartBox_.w;   // Ctrl: keep its shape
        dz::Snapped s{b, {}, {}};
        if (!shift) s = dz::snap(designDoc_, std::vector<std::string>{designSel_}, b, 6 / designScale_, true);
        e->w = std::round(s.box.w);
        e->h = std::round(s.box.h);
        dGuidesX_ = s.guidesX;
        dGuidesY_ = s.guidesY;
        invalidate();
        return true;
    }
    // Moving everything selected, snapped as one box.
    dz::Box b = dStartBox_;
    b.x += px - dStartX_;
    b.y += py - dStartY_;
    std::vector<std::string> moving;
    for (auto& [id, box] : dStartBoxes_) moving.push_back(id);
    dz::Snapped s{b, {}, {}};
    if (!shift) s = dz::snap(designDoc_, moving, b, 6 / designScale_, false);
    const float dx = std::round(s.box.x - dStartBox_.x), dy = std::round(s.box.y - dStartBox_.y);
    for (auto& [id, box] : dStartBoxes_)
        if (dz::Element* e = designDoc_.find(id); e && !e->locked) {
            e->x = box.x + dx;
            e->y = box.y + dy;
        }
    dGuidesX_ = s.guidesX;
    dGuidesY_ = s.guidesY;
    invalidate();
    return true;
}

bool CaptureWindow::designerMouseUp() {
    if (ddrag_ == 0) return false;
    const int was = ddrag_;
    ddrag_ = 0;
    dGuidesX_.clear();
    dGuidesY_.clear();
    if (was == 5 && dStopOff_) {
        if (dz::Element* e = designDoc_.find(dGradFor_); e && dz::removeStop(e->fill, dStopSel_)) dStopSel_ = -1;
        dStopOff_ = false;
    }
    if (was == 3) {
        // Everything the box touches (whole groups), unless it was just a click.
        if (dMarquee_.w > 3 || dMarquee_.h > 3) {
            const float sc = designScale_;
            const float x0 = (dMarquee_.x - designCanvas_.x) / sc, y0 = (dMarquee_.y - designCanvas_.y) / sc;
            const float x1 = x0 + dMarquee_.w / sc, y1 = y0 + dMarquee_.h / sc;
            std::vector<std::string> ids;
            for (auto& e : designDoc_.elements)
                if (e.visible && !e.locked && e.x < x1 && e.x + e.w > x0 && e.y < y1 && e.y + e.h > y0)
                    for (auto& id : dz::pick(designDoc_, e.id))
                        if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
            designSelect(ids, ids.empty() ? std::string() : ids.back());
        }
        dMarquee_ = {};
        invalidate();
        return true;
    }
    if (was == 4) {
        // A guide dragged off the page (onto a ruler or beyond) goes.
        auto& gs = dGuideAxis_ == 0 ? designDoc_.guidesX : designDoc_.guidesY;
        const float lim = dGuideAxis_ == 0 ? (float)designDoc_.width : (float)designDoc_.height;
        if (dGuideIndex_ >= 0 && dGuideIndex_ < (int)gs.size() && (gs[(size_t)dGuideIndex_] < 0 || gs[(size_t)dGuideIndex_] > lim))
            gs.erase(gs.begin() + dGuideIndex_);
        dGuideIndex_ = -1;
    }
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
    if (ctrl && vk == 'A') {   // everything that can be picked
        std::vector<std::string> all;
        for (auto& e : designDoc_.elements)
            if (e.visible && !e.locked) all.push_back(e.id);
        designSelect(all, all.empty() ? std::string() : all.back());
        return true;
    }
    if (ctrl && vk == 'G') { designGroupSelected(!shift); return true; }
    const auto sel = designSelection();
    if (vk == VK_ESCAPE && !sel.empty()) { designSelect({}, {}); return true; }
    if (sel.empty()) return false;
    if (ctrl && vk == 'D') {
        // Copies of everything selected; a whole group's copies make a group of their own.
        std::vector<std::string> copies;
        std::map<std::string, std::vector<std::string>> byGroup;
        for (auto& id : sel) {
            const std::string g = designDoc_.find(id) ? designDoc_.find(id)->group : std::string();
            const std::string nid = dz::duplicate(designDoc_, id);
            if (nid.empty()) continue;
            if (dz::Element* c = designDoc_.find(nid)) c->group.clear();
            copies.push_back(nid);
            if (!g.empty()) byGroup[g].push_back(nid);
        }
        for (auto& [g, ids] : byGroup)
            if (ids.size() >= 2) dz::group(designDoc_, ids);
        designChanged();
        designSelect(copies, copies.empty() ? std::string() : copies.back());
        return true;
    }
    if (vk == VK_DELETE) {
        for (auto& id : sel) dz::remove(designDoc_, id);
        designSelect({}, {});
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
    for (auto& id : sel)
        if (dz::Element* e = designDoc_.find(id); e && !e->locked) {
            e->x += dx;
            e->y += dy;
        }
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
            items.push_back(MenuItem::item(L"Show only during a writing sprint", 33, L"Writing Studio", true, designDoc_.onlyWhileSprint));
            items.push_back(MenuItem::item(L"Show only for celebrations", 34, L"{celebration}, a few seconds", true, designDoc_.onlyCelebrating));
            items.push_back(MenuItem::sep());
            {
                std::vector<MenuItem> grid;
                for (int g : {0, 10, 20, 40, 50, 100})
                    grid.push_back(MenuItem::item(g ? std::to_wstring(g) + L" pixels" : std::wstring(L"Off"), 60 + (UINT)g, {}, true, designDoc_.grid == g));
                items.push_back(MenuItem::sub(L"Grid (snaps when nothing else is near)", grid));
            }
            items.push_back(MenuItem::item(L"Remove the guides", 35, std::to_wstring(designDoc_.guidesX.size() + designDoc_.guidesY.size()),
                                           !designDoc_.guidesX.empty() || !designDoc_.guidesY.empty()));
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
            } else if (cmd == 33) {
                designDoc_.onlyWhileSprint = !designDoc_.onlyWhileSprint;
                designChanged();
            } else if (cmd == 35) {
                designDoc_.guidesX.clear();
                designDoc_.guidesY.clear();
                designChanged();
            } else if (cmd >= 60 && cmd <= 160) {
                designDoc_.grid = (int)cmd - 60;
                designChanged();
            } else if (cmd == 34) {
                designDoc_.onlyCelebrating = !designDoc_.onlyCelebrating;
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
    designCentre_ = centre;

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
            designMulti_.clear();
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
            designMulti_.clear();
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
            designMulti_.clear();
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
            designMulti_.clear();
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
            designMulti_.clear();
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
            designMulti_.clear();
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
                // A row picks that one element (inside a group too); Shift or Ctrl adds it to the selection.
                if (GetKeyState(VK_SHIFT) < 0 || GetKeyState(VK_CONTROL) < 0) {
                    auto sel = designSelection();
                    auto it = std::find(sel.begin(), sel.end(), eid);
                    if (it != sel.end()) sel.erase(it);
                    else sel.push_back(eid);
                    designSelect(sel, eid);
                } else designSelect({eid}, eid);
            });
            const auto selNow = designSelection();
            const bool sel = std::find(selNow.begin(), selNow.end(), e.id) != selNow.end();
            if (!e.group.empty()) {   // a group's members: a bar in the group's own colour
                std::uint32_t h = 2166136261u;
                for (unsigned char ch : e.group) h = (h ^ ch) * 16777619u;
                static const char* cols[] = {"#3FCF8E", "#7A5CFF", "#E8C547", "#22C3E6", "#E0566A"};
                gfx_.fill({row.x - 6, row.y + 4, 3, row.h - 8}, cjui::hexColor(cols[h % 5]));
            }
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
        const float sc = std::min((centre.w - 24) / designDoc_.width, (centre.h - 46) / designDoc_.height);   // room for the rulers
        designScale_ = sc;
        const float pw = designDoc_.width * sc, ph = designDoc_.height * sc;
        designCanvas_ = {std::round(centre.x + 24 + (centre.w - 24 - pw) / 2), std::round(centre.y + 22 + (centre.h - 46 - ph) / 2), std::round(pw),
                         std::round(ph)};
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
        // The grid (0.14): faint lines at its spacing, every fifth a little stronger.
        if (designDoc_.grid > 0 && designDoc_.grid * sc >= 4) {
            gfx_.pushClip(c);
            const float g = (float)designDoc_.grid;
            int k = 0;
            for (float gx = 0; gx <= designDoc_.width; gx += g, ++k)
                gfx_.line(c.x + gx * sc, c.y, c.x + gx * sc, c.bottom(), cjui::hexColor("#7A5CFF").withAlpha(k % 5 ? 0.12f : 0.28f), 1);
            k = 0;
            for (float gy = 0; gy <= designDoc_.height; gy += g, ++k)
                gfx_.line(c.x, c.y + gy * sc, c.right(), c.y + gy * sc, cjui::hexColor("#7A5CFF").withAlpha(k % 5 ? 0.12f : 0.28f), 1);
            gfx_.popClip();
        }
        // The rulers along the top and left, in page pixels, and the guides placed from them.
        {
            const Rect tr{c.x, c.y - 20, c.w, 18}, lr{c.x - 20, c.y, 18, c.h};
            gfx_.fill(tr, pal_.ink.withAlpha(pal_.dark ? 0.08f : 0.05f));
            gfx_.fill(lr, pal_.ink.withAlpha(pal_.dark ? 0.08f : 0.05f));
            addHit("dz:ruler:top", tr, [] {});
            addHit("dz:ruler:left", lr, [] {});
            tooltip("dz:ruler:top", L"Click to place a guide; drag one back here to remove it");
            tooltip("dz:ruler:left", L"Click to place a guide; drag one back here to remove it");
            float stepPx = 10;
            for (float s2 : {10.f, 20.f, 50.f, 100.f, 200.f, 500.f, 1000.f}) { stepPx = s2; if (s2 * sc >= 50) break; }
            for (float v = 0; v <= designDoc_.width; v += stepPx / 5) {
                const bool major = std::fmod(v, stepPx) < 0.5f;
                const float xx = c.x + v * sc;
                gfx_.line(xx, tr.bottom(), xx, tr.bottom() - (major ? 8.0f : 3.0f), pal_.inkFaint, 1);
                if (major) gfx_.text(std::to_wstring((int)v), {xx + 2, tr.y, 50, 12}, Font::Small, pal_.inkFaint, {Align::Left, false, false, true});
            }
            for (float v = 0; v <= designDoc_.height; v += stepPx / 5) {
                const bool major = std::fmod(v, stepPx) < 0.5f;
                const float yy = c.y + v * sc;
                gfx_.line(lr.right(), yy, lr.right() - (major ? 8.0f : 3.0f), yy, pal_.inkFaint, 1);
            }
            const Color gc = cjui::hexColor("#22C3E6");
            for (float gx : designDoc_.guidesX) {
                gfx_.line(c.x + gx * sc, tr.y, c.x + gx * sc, c.bottom(), gc.withAlpha(0.8f), 1);
                gfx_.fill({c.x + gx * sc - 3, tr.y, 6, 6}, gc);
            }
            for (float gy : designDoc_.guidesY) {
                gfx_.line(lr.x, c.y + gy * sc, c.right(), c.y + gy * sc, gc.withAlpha(0.8f), 1);
                gfx_.fill({lr.x, c.y + gy * sc - 3, 6, 6}, gc);
            }
        }
        // Guides while dragging, and the selection.
        for (float gx : dGuidesX_) gfx_.line(c.x + gx * sc, c.y, c.x + gx * sc, c.bottom(), cjui::hexColor("#FF3DD6"), 1);
        for (float gy : dGuidesY_) gfx_.line(c.x, c.y + gy * sc, c.right(), c.y + gy * sc, cjui::hexColor("#FF3DD6"), 1);
        gfx_.pushClip({c.x - 6, c.y - 6, c.w + 12, c.h + 12});   // a selection reaching off the page stops at its edge
        {
            const auto sel = designSelection();
            const Color green = cjui::hexColor("#3FCF8E");
            for (auto& id : sel)
                if (const dz::Element* e = designDoc_.find(id))
                    gfx_.strokeRounded({c.x + e->x * sc, c.y + e->y * sc, e->w * sc, e->h * sc}, 0, green.withAlpha(sel.size() > 1 ? 0.8f : 1.0f),
                                       sel.size() > 1 ? 1.0f : 1.5f);
            if (sel.size() > 1) {   // their joint box, dashed
                const dz::Box jb = dz::boundsOf(designDoc_, sel);
                const Rect r{c.x + jb.x * sc - 3, c.y + jb.y * sc - 3, jb.w * sc + 6, jb.h * sc + 6};
                for (float t = 0; t < r.w; t += 10) {
                    gfx_.line(r.x + t, r.y, r.x + std::min(t + 5, r.w), r.y, green, 1.5f);
                    gfx_.line(r.x + t, r.bottom(), r.x + std::min(t + 5, r.w), r.bottom(), green, 1.5f);
                }
                for (float t = 0; t < r.h; t += 10) {
                    gfx_.line(r.x, r.y + t, r.x, r.y + std::min(t + 5, r.h), green, 1.5f);
                    gfx_.line(r.right(), r.y + t, r.right(), r.y + std::min(t + 5, r.h), green, 1.5f);
                }
            } else if (sel.size() == 1) {
                if (const dz::Element* e = designDoc_.find(sel[0]); e && !e->locked) {
                    const Rect r{c.x + e->x * sc, c.y + e->y * sc, e->w * sc, e->h * sc};
                    const Rect hnd{r.right() - 5, r.bottom() - 5, 10, 10};
                    gfx_.fill(hnd, cjui::hexColor("#FFFFFF"));
                    gfx_.strokeRounded(hnd, 0, green, 1.5f);
                }
            }
        }
        gfx_.popClip();
        if (ddrag_ == 3 && (dMarquee_.w > 0 || dMarquee_.h > 0)) {
            gfx_.fill(dMarquee_, cjui::hexColor("#3FCF8E").withAlpha(0.10f));
            gfx_.strokeRounded(dMarquee_, 0, cjui::hexColor("#3FCF8E"), 1);
        }
        gfx_.text(L"Drag to move (Shift: no snapping), the corner to resize (Ctrl: keep its shape). Shift-click or drag a box to select "
                  L"several; Ctrl+G groups. Arrows nudge; Delete; Ctrl+D copies; Ctrl+Z undoes.",
                  {centre.x, centre.bottom() - 2, centre.w, 18}, Font::Small, pal_.inkFaint, {Align::Center, false, true, true});
    }

    // ---- Right: the selected element's properties.
    drawDesignerProperties(right);
}

void CaptureWindow::drawDesignerProperties(const Rect& r) {
    dPropRect_ = r;
    dGradFor_.clear();
    dGradBar_ = {};
    dPropScroll_ = std::clamp(dPropScroll_, 0.0f, std::max(0.0f, dPropContentH_ - r.h));
    gfx_.pushClip(r);
    setHitClip(r);
    const Rect inner{r.x, r.y - dPropScroll_, r.w, r.h + dPropScroll_};
    dPropEnd_ = inner.y;
    drawDesignerPropertiesBody(inner);
    clearHitClip();
    dPropContentH_ = dPropEnd_ - inner.y + 12;
    // More below (or above): the edge fades out; the mouse wheel scrolls.
    if (dPropScroll_ < dPropContentH_ - r.h - 1)
        for (int i = 0; i < 8; ++i) gfx_.fill({r.x, r.bottom() - 24 + i * 3.0f, r.w, 3}, pal_.paper.withAlpha(0.12f * (float)(i + 1)));
    if (dPropScroll_ > 1)
        for (int i = 0; i < 8; ++i) gfx_.fill({r.x, r.y + 21 - i * 3.0f, r.w, 3}, pal_.paper.withAlpha(0.12f * (float)(i + 1)));
    gfx_.popClip();
}

void CaptureWindow::drawDesignerPropertiesBody(const Rect& r) {
    float y = r.y;
    // Several selected (0.14): lining them up, spacing them, grouping.
    if (const auto sel = designSelection(); sel.size() > 1) {
        bool oneGroup = true, anyGroup = false;
        const std::string g0 = designDoc_.find(sel[0]) ? designDoc_.find(sel[0])->group : std::string();
        for (auto& id : sel)
            if (const dz::Element* x = designDoc_.find(id)) {
                anyGroup |= !x->group.empty();
                oneGroup &= !x->group.empty() && x->group == g0;
            }
        if (oneGroup && dz::members(designDoc_, g0).size() != sel.size()) oneGroup = false;
        gfx_.text(oneGroup ? L"GROUP" : L"SELECTION", {r.x, y, r.w, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
        y += 22;
        gfx_.text(std::to_wstring(sel.size()) + L" elements" + (oneGroup ? L", grouped" : L""), {r.x, y, r.w, 28}, Font::Heading, pal_.ink,
                  {Align::Left, false, true});
        y += 40;
        auto act = [this](std::function<bool(dz::Document&, const std::vector<std::string>&)> f) {
            const auto s2 = designSelection();
            if (f(designDoc_, s2)) designChanged();
        };
        gfx_.text(L"Line up", {r.x, y, r.w, 20}, Font::Small, pal_.inkMuted, {Align::Left, false});
        y += 22;
        static const char* hows[] = {"left", "centre", "right", "top", "middle", "bottom"};
        static const wchar_t* names[] = {L"Left", L"Centre", L"Right", L"Top", L"Middle", L"Bottom"};
        const float bw = (r.w - 16) / 3;
        for (int i = 0; i < 6; ++i)
            button(std::string("dz:al:") + hows[i], {r.x + (i % 3) * (bw + 8), y + (i / 3) * 38.0f, bw, 30}, names[i], [act, i] {
                act([i](dz::Document& d, const std::vector<std::string>& s3) { return dz::align(d, s3, hows[i]); });
            });
        y += 82;
        gfx_.text(L"Space evenly", {r.x, y, r.w, 20}, Font::Small, pal_.inkMuted, {Align::Left, false});
        y += 22;
        const float hw = (r.w - 8) / 2;
        button("dz:dist:x", {r.x, y, hw, 30}, L"Across", [this, act] {
            if (designSelection().size() < 3) { flash(L"Spacing evenly needs three or more.", 4); return; }
            act([](dz::Document& d, const std::vector<std::string>& s3) { return dz::distribute(d, s3, true); });
        });
        button("dz:dist:y", {r.x + hw + 8, y, hw, 30}, L"Down", [this, act] {
            if (designSelection().size() < 3) { flash(L"Spacing evenly needs three or more.", 4); return; }
            act([](dz::Document& d, const std::vector<std::string>& s3) { return dz::distribute(d, s3, false); });
        });
        y += 46;
        gfx_.text(L"On the page", {r.x, y, r.w, 20}, Font::Small, pal_.inkMuted, {Align::Left, false});
        y += 22;
        button("dz:pg:c", {r.x, y, hw, 30}, L"Centre across", [act] {
            act([](dz::Document& d, const std::vector<std::string>& s3) { return dz::align(d, s3, "centre", true); });
        });
        button("dz:pg:m", {r.x + hw + 8, y, hw, 30}, L"Centre down", [act] {
            act([](dz::Document& d, const std::vector<std::string>& s3) { return dz::align(d, s3, "middle", true); });
        });
        y += 46;
        button("dz:grp", {r.x, y, hw, 32}, oneGroup ? L"Ungroup" : L"Group", [this, oneGroup] { designGroupSelected(!oneGroup); }, true);
        if (anyGroup && !oneGroup) button("dz:ungrp", {r.x + hw + 8, y, hw, 32}, L"Ungroup", [this] { designGroupSelected(false); });
        y += 44;
        button("dz:msdup", {r.x, y, hw, 30}, L"Duplicate", [this] { designerKey('D', true, false); });
        button("dz:msdel", {r.x + hw + 8, y, hw, 30}, L"Delete", [this] { designerKey(VK_DELETE, false, false); });
        y += 44;
        y += gfx_.text(L"Shift-click adds or takes one away. Alt-click picks one element inside a group to change it on its own; so does "
                       L"its row in Layers. Ctrl+G groups, Ctrl+Shift+G ungroups, Ctrl+A selects everything, Esc nothing.",
                       {r.x, y, r.w, 80}, Font::Small, pal_.inkMuted) + 10;
        dPropEnd_ = y;
        return;
    }
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
                       L"follow your Brand kit…: change it once and every design using it changes.\n\n"
                       L"From Writing Studio: {book}, {series}, {chapter}, {chapterline}, {today}, {bookline}, {streakline}, {status}, "
                       L"{targetline}, {viewpoint}, {place}, {storyday}, {cast1} to {cast6}, {sprintline}, {streamline} and {celebration}; "
                       L"pictures can be {cover}, {viewpointpic}, {placepic} or {cast1pic} to {cast6pic}.",
                       {r.x, y, r.w, 260}, Font::Small, pal_.inkMuted) + 12;
        dPropEnd_ = y;
        return;
    }
    const std::string eid = e->id;
    if (!e->group.empty()) {   // one element of a group, picked on its own (Alt-click or its row in Layers)
        const std::string gid = e->group;
        gfx_.text(L"Part of a group of " + std::to_wstring(dz::members(designDoc_, gid).size()), {r.x, y, r.w - 190, 28}, Font::Small, pal_.inkMuted,
                  {Align::Left, false, true, true});
        button("dz:p:selgrp", {r.right() - 186, y, 104, 28}, L"The group", [this, gid] { designSelect(dz::members(designDoc_, gid), designSel_); });
        button("dz:p:ungrp", {r.right() - 76, y, 76, 28}, L"Ungroup", [this, gid] {
            dz::ungroup(designDoc_, gid);
            designChanged();
        });
        y += 40;
    }
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
    // The gradient bar (0.15): the gradient itself, with a handle under it for each colour. Click the bar to add a colour
    // there; drag a handle to move it (it snaps to tenths; Shift: anywhere), pull it off the bar to remove it, click it to
    // choose its colour below.
    if ((e->fill.type == dz::Fill::Linear || e->fill.type == dz::Fill::Radial) && e->fill.stops.size() >= 2) {
        if (dStopOwner_ != eid) dStopSel_ = -1, dStopOwner_ = eid;
        const dz::Fill& f = e->fill;
        const Rect bar{r.x + 10, y + 2, r.w - 20, 22};
        gfx_.fillRounded(bar, 4, cjui::hexColor("#FFFFFF"));   // a checkerboard shows see-through colours
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col * 8.0f < bar.w; ++col)
                if ((row + col) % 2)
                    gfx_.fill({bar.x + col * 8.0f, bar.y + row * 8.0f, std::min(8.0f, bar.w - col * 8.0f), std::min(8.0f, bar.h - row * 8.0f)},
                              cjui::hexColor("#C8CDD4"));
        std::vector<std::uint8_t> px(256 * 4);
        for (int i = 0; i < 256; ++i) {
            const su::Rgba c = dz::colourAt(f, i / 255.0f);
            px[(size_t)i * 4 + 0] = c.r, px[(size_t)i * 4 + 1] = c.g, px[(size_t)i * 4 + 2] = c.b, px[(size_t)i * 4 + 3] = c.a;
        }
        std::wstring key = L"dz:grad:" + widen(stopsText(f));
        for (auto& st : f.stops) key += L"|" + std::to_wstring(st.colour.a);
        if (key != dGradKey_) {
            if (!dGradKey_.empty()) gfx_.forgetImage(dGradKey_);
            dGradKey_ = key;
        }
        gfx_.drawPixels(key, 256, 1, px.data(), bar, true);
        gfx_.strokeRounded(bar, 4, pal_.rule, 1);
        for (std::size_t i = 0; i < f.stops.size(); ++i) {
            const dz::Stop& st = f.stops[i];
            const bool chosen = (int)i == dStopSel_;
            const float hx = bar.x + st.at * bar.w, hy = bar.bottom() + 10;
            const float fade = chosen && dStopOff_ ? 0.3f : 1.0f;
            gfx_.line(hx, bar.y - 2, hx, hy - 6, (chosen ? cjui::hexColor("#3FCF8E") : pal_.ink).withAlpha(0.8f * fade), chosen ? 2.0f : 1.0f);
            gfx_.circle(hx, hy, 7, cjui::Color{st.colour.r / 255.0f, st.colour.g / 255.0f, st.colour.b / 255.0f, fade}, true);
            gfx_.circle(hx, hy, 7, (chosen ? cjui::hexColor("#3FCF8E") : pal_.ink).withAlpha(fade), false, chosen ? 2.5f : 1.2f);
        }
        dGradFor_ = eid;
        dGradBar_ = {bar.x - 8, bar.y - 4, bar.w + 16, bar.h + 26};
        y += 54;
        if (dStopSel_ >= 0 && dStopSel_ < (int)f.stops.size()) {
            const int si = dStopSel_;
            const dz::Stop st = f.stops[(size_t)si];
            label(L"This colour");
            const float bw = f.stops.size() > 2 ? r.w - 112 - 84 : r.w - 112;
            button("dz:p:stopcol", {r.x + 112, y - 2, bw, 30}, colourName(st.colour, st.brand) + L"  at " + std::to_wstring((int)std::lround(st.at * 100)) + L"%",
                   [this, eid, si, edit, colourMenu] {
                       const dz::Element* x = designDoc_.find(eid);
                       if (!x || si >= (int)x->fill.stops.size()) return;
                       colourMenu(x->fill.stops[(size_t)si].colour, x->fill.stops[(size_t)si].brand, [edit, si](su::Rgba c, std::string b) {
                           edit([c, b, si](dz::Element& z) {
                               if (si >= (int)z.fill.stops.size()) return;
                               z.fill.stops[(size_t)si].colour = c;
                               z.fill.stops[(size_t)si].brand = b;
                           });
                       }, false);
                   });
            if (f.stops.size() > 2)
                button("dz:p:stoprm", {r.right() - 78, y - 2, 78, 30}, L"Remove", [this, eid, si] {
                    dz::Element* x = designDoc_.find(eid);
                    if (!x) return;
                    const std::string before = dz::toJson(designDoc_);
                    if (dz::removeStop(x->fill, si)) {
                        designUndo_.push_back(before);
                        dStopSel_ = -1;
                        designChanged(false);
                    }
                });
            y += 36;
        } else {
            y += gfx_.text(L"Click the bar to add a colour. Drag one to move it, or off the bar to remove it; click one to change it.",
                           {r.x, y - 4, r.w, 40}, Font::Small, pal_.inkMuted) + 6;
        }
        if (f.type == dz::Fill::Linear)
            sliderRow("gang", L"Direction", f.angle < 0 ? f.angle + 360 : f.angle, 0, 360, L"°", [](dz::Element& x, float v) { x.fill.angle = v; });
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
    // Glow (0.15): light round the shape, its outline or its letters.
    {
        label(L"Glow");
        toggle("dz:p:glow", r.x + 112, y + 2, e->glow.on, [edit] {
            edit([](dz::Element& z) {
                z.glow.on = !z.glow.on;
                if (z.glow.on && z.glow.brand.empty() && z.glow.colour == dz::Glow{}.colour) {   // the first time: the kit's main colour
                    const dz::Brand b = video::designBrand();
                    if (const dz::BrandColour* c = dz::brandColour(b, "primary")) {
                        z.glow.colour = {c->colour.r, c->colour.g, c->colour.b, z.glow.colour.a};
                        z.glow.brand = "primary";
                    }
                }
            });
        });
        if (e->glow.on)
            button("dz:p:glcol", {r.x + 170, y - 2, r.w - 170, 30}, colourName(e->glow.colour, e->glow.brand), [this, eid, edit, colourMenu] {
                const dz::Element* x = designDoc_.find(eid);
                if (!x) return;
                colourMenu(x->glow.colour, x->glow.brand, [edit](su::Rgba c, std::string b) {
                    edit([c, b](dz::Element& z) {
                        z.glow.colour = c;
                        z.glow.brand = b;
                    });
                }, false);
            });
        y += 36;
        if (e->glow.on) {
            sliderRow("gls", L"Size", e->glow.size, 1, 120, L" px", [](dz::Element& x, float v) { x.glow.size = v; });
            sliderRow("glt", L"Strength", e->glow.strength * 100, 25, 400, L"%", [](dz::Element& x, float v) { x.glow.strength = v / 100; });
        }
    }
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
        static const char* modes[] = {"countdown", "until", "stopwatch", "live", "clock", "golive", "remaining", "next", "sprint"};
        static const wchar_t* modeNames[] = {L"Countdown for a length", L"Countdown to a time", L"Stopwatch", L"Time live", L"Clock",
                                             L"Until going live", L"Time left in the stream", L"Next stream", L"Writing sprint (Writing Studio)"};
        constexpr int kModes = 9;
        int mi = 0;
        for (int i = 0; i < kModes; ++i)
            if (e->timerMode == modes[i]) mi = i;
        label(L"Shows");
        button("dz:p:tmode", {r.x + 112, y - 2, r.w - 112, 30}, modeNames[mi], [this, eid, edit] {
            std::vector<MenuItem> items;
            const dz::Element* x = designDoc_.find(eid);
            for (int i = 0; i < kModes; ++i) {
                if (i == 5 || i == 8) items.push_back(MenuItem::sep());
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
        if (e->timerMode == "sprint")
            y += gfx_.text(L"Counts down a writing sprint started from the word count in Writing Studio's writing bar. "
                           L"{sprintwords} is the words written in it.",
                           {r.x, y, r.w, 60}, Font::Small, pal_.inkMuted) + 8;
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
            designMulti_.clear();
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
    dPropEnd_ = y;
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
