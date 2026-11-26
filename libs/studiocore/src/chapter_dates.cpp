#include "studio/chapter_dates.hpp"
#include "studio/typeset.hpp"

#include <cmath>
#include <nlohmann/json.hpp>

namespace studio::dates {

using nlohmann::json;

namespace {
bool oneOf(const std::string& v, std::initializer_list<const char*> ok) {
    for (const char* o : ok) if (v == o) return true;
    return false;
}
std::string pad(std::int64_t v, int width) {
    std::string s = std::to_string(v < 0 ? -v : v);
    while ((int)s.size() < width) s = "0" + s;
    return (v < 0 ? "-" : "") + s;
}
json settingOf(const Library& lib, const Id& book) {
    json j = json::parse(lib.setting("dates:" + book), nullptr, false);
    return j.is_object() ? j : json::object();
}
}  // namespace

const std::vector<std::pair<std::string, std::string>>& styles() {
    static const std::vector<std::pair<std::string, std::string>> s = {
        {"numeric-dmy", "Numeric, day first"}, {"numeric-mdy", "Numeric, month first"}, {"numeric-ymd", "Numeric, year first"},
        {"day-month", "Day and month name"},   {"long", "Long, with weekday"},          {"month", "Month name only"},
        {"custom", "Your own pattern"}};
    return s;
}

std::string Format::toJson() const {
    return json{{"style", style}, {"pattern", pattern}, {"era", era}, {"eraPlace", eraPlace}, {"eraSeparator", eraSeparator}, {"time", time}}.dump();
}

Format Format::fromJson(const std::string& text) {
    Format f;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return f;
    auto str = [&](const char* k, std::string& v, std::initializer_list<const char*> ok) {
        if (!j.contains(k) || !j[k].is_string()) return;
        const std::string got = j[k].get<std::string>();
        if (ok.size() == 0 || oneOf(got, ok)) v = got;
    };
    str("style", f.style, {"numeric-dmy", "numeric-mdy", "numeric-ymd", "day-month", "long", "month", "custom"});
    str("era", f.era, {"none", "short", "full"});
    str("eraPlace", f.eraPlace, {"after", "before"});
    str("time", f.time, {"hidden", "appended", "line"});
    if (j.contains("pattern") && j["pattern"].is_string() && j["pattern"].get<std::string>().size() <= 200) f.pattern = j["pattern"].get<std::string>();
    if (j.contains("eraSeparator") && j["eraSeparator"].is_string() && j["eraSeparator"].get<std::string>().size() <= 12)
        f.eraSeparator = j["eraSeparator"].get<std::string>();
    return f;
}

Lines format(const cal::Calendar& c, const cal::Date& d0, const cal::TimeOfDay& t, const Format& f) {
    Lines out;
    if (c.empty()) return out;
    const cal::Date d = cal::clampDate(c, d0);
    const cal::EraYear ey = cal::eraYear(c, d.year);
    const std::string month = cal::monthLabel(c, d.month);
    const std::int64_t mnum = cal::monthNumber(c, d.month);
    std::string weekday;
    if (!c.weekdays.empty()) {
        const int w = cal::weekday(c, cal::toDay(c, d));
        if (w >= 0 && w < (int)c.weekdays.size()) weekday = c.weekdays[(size_t)w];
    }
    const std::string sep(1, c.separator ? c.separator : '/');
    std::string date;
    bool eraNamed = false;
    if (f.style == "numeric-dmy") date = pad(d.day, 2) + sep + pad(mnum, 2) + sep + pad(ey.year, 2);
    else if (f.style == "numeric-mdy") date = pad(mnum, 2) + sep + pad(d.day, 2) + sep + pad(ey.year, 2);
    else if (f.style == "numeric-ymd") date = pad(ey.year, 4) + "-" + pad(mnum, 2) + "-" + pad(d.day, 2);
    else if (f.style == "long") date = (weekday.empty() ? "" : weekday + ", ") + std::to_string(d.day) + " " + month + " " + std::to_string(ey.year);
    else if (f.style == "month") date = month + " " + std::to_string(ey.year);
    else if (f.style == "custom") {
        const std::string& p = f.pattern;
        for (size_t i = 0; i < p.size();) {
            if (p[i] == '{') {
                const size_t e = p.find('}', i);
                if (e != std::string::npos) {
                    const std::string tok = p.substr(i + 1, e - i - 1);
                    std::string v;
                    bool known = true;
                    if (tok == "D") v = std::to_string(d.day);
                    else if (tok == "DD") v = pad(d.day, 2);
                    else if (tok == "M") v = std::to_string(mnum);
                    else if (tok == "MM") v = pad(mnum, 2);
                    else if (tok == "Month") v = month;
                    else if (tok == "Mon") v = month.substr(0, std::min<size_t>(3, month.size()));
                    else if (tok == "Year" || tok == "YYYY") v = std::to_string(ey.year);
                    else if (tok == "Weekday") v = weekday;
                    else if (tok == "Era") { v = ey.abbr; eraNamed = true; }
                    else if (tok == "EraName") { v = ey.name; eraNamed = true; }
                    else known = false;
                    if (known) { date += v; i = e + 1; continue; }
                }
            }
            date += p[i++];
        }
        // A weekday-less calendar leaves "{Weekday}, " as ", ": tidy what's left at the start.
        while (!date.empty() && (date[0] == ',' || date[0] == ' ')) date.erase(0, 1);
    } else date = std::to_string(d.day) + " " + month + " " + std::to_string(ey.year);   // day-month
    // The era, unless the pattern placed it.
    if (!eraNamed && f.era != "none") {
        const std::string tag = f.era == "full" ? (ey.name.empty() ? ey.abbr : ey.name) : (ey.abbr.empty() ? ey.name : ey.abbr);
        if (!tag.empty()) date = f.eraPlace == "before" ? tag + f.eraSeparator + date : date + f.eraSeparator + tag;
    }
    out.date = date;
    if (!t.empty() && f.time != "hidden") {
        const std::string tt = cal::timeOfDayText(c, t);
        if (!tt.empty()) {
            if (f.time == "line") out.time = tt;
            else out.date += ", " + tt;
        }
    }
    return out;
}

Format bookFormat(const Library& lib, const Id& book) {
    const json j = settingOf(lib, book);
    if (j.contains("format") && j["format"].is_object()) return Format::fromJson(j["format"].dump());
    // From the Typesetting design's two choices (0.85).
    const typeset::Design d = typeset::designFromJson(lib.setting("typeset:" + book));
    Format f;
    if (d.dateForm == "numbers") {
        const cal::Calendar c = cal::calendarOfBook(lib, book);
        f.style = c.order == "mdy" ? "numeric-mdy" : c.order == "ymd" ? "numeric-ymd" : "numeric-dmy";
    }
    f.era = d.dateEra == "name" ? "full" : d.dateEra == "none" ? "none" : "short";
    return f;
}

void setBookFormat(Library& lib, const Id& book, const Format& f) {
    json j = settingOf(lib, book);
    j["format"] = json::parse(f.toJson());
    lib.setSetting("dates:" + book, j.dump());
}

std::optional<Format> chapterFormat(const Library& lib, const Id& book, const Id& chapter) {
    const json j = settingOf(lib, book);
    if (j.contains("chapters") && j["chapters"].is_object() && j["chapters"].contains(chapter) && j["chapters"][chapter].is_object())
        return Format::fromJson(j["chapters"][chapter].dump());
    return std::nullopt;
}

void setChapterFormat(Library& lib, const Id& book, const Id& chapter, const std::optional<Format>& f) {
    json j = settingOf(lib, book);
    if (!j.contains("chapters") || !j["chapters"].is_object()) j["chapters"] = json::object();
    if (f) j["chapters"][chapter] = json::parse(f->toJson());
    else j["chapters"].erase(chapter);
    lib.setSetting("dates:" + book, j.dump());
}

Format formatFor(const Library& lib, const Id& book, const Id& chapter) {
    if (auto own = chapterFormat(lib, book, chapter)) return *own;
    return bookFormat(lib, book);
}

std::optional<Lines> lineFor(const Library& lib, const ManuscriptNode& n) {
    if (n.kind == NodeKind::Part) return std::nullopt;
    const cal::StorySpan span = cal::storySpan(lib, n.bookId);
    if (!span.on) return std::nullopt;
    const cal::Calendar c = cal::calendarOfBook(lib, n.bookId);
    if (c.empty()) return std::nullopt;
    const json tl = json::parse(lib.setting("timeline:" + n.bookId), nullptr, false);
    if (!tl.is_object() || !tl.contains("nodes") || !tl["nodes"].is_object()) return std::nullopt;
    const json& nodes = tl["nodes"];
    auto placed = [&](const Id& id) -> const json* {
        if (!nodes.contains(id) || !nodes[id].is_object() || !nodes[id].contains("day") || !nodes[id]["day"].is_number()) return nullptr;
        return &nodes[id];
    };
    const json* v = placed(n.id);
    if (!v && n.kind == NodeKind::Chapter)
        for (const ManuscriptNode* m : lib.manuscriptOf(n.bookId))
            if (m->parentId == n.id && m->kind == NodeKind::Scene)
                if ((v = placed(m->id))) break;
    if (!v) return std::nullopt;
    const std::int64_t day = span.start + (std::int64_t)std::floor((*v)["day"].get<double>());
    cal::TimeOfDay t;
    if (v->contains("time") && (*v)["time"].is_number_integer()) t.minutes = (*v)["time"].get<int>();
    else if (v->contains("part") && (*v)["part"].is_string()) t.part = (*v)["part"].get<std::string>();
    const Id chapter = n.kind == NodeKind::Scene ? n.parentId : n.id;
    return format(c, cal::fromDay(c, day), t, formatFor(lib, n.bookId, chapter));
}

}  // namespace studio::dates
