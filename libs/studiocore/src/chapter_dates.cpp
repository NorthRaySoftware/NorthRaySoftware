#include "studio/chapter_dates.hpp"
#include "studio/typeset.hpp"

#include <algorithm>
#include <cctype>
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
    auto part = [](const PartLook& p) { return json{{"bold", p.bold}, {"italic", p.italic}, {"caps", p.caps}}; };
    return json{{"style", style}, {"pattern", pattern}, {"era", era}, {"eraPlace", eraPlace}, {"eraSeparator", eraSeparator}, {"time", time},
                {"look", {{"date", part(look.date)}, {"era", part(look.era)}, {"time", part(look.time)}, {"align", look.align},
                          {"position", look.position}, {"sceneDates", look.sceneDates}}}}
        .dump();
}

std::string applyCaps(const std::string& text, const std::string& caps) {
    if (caps != "upper") return text;
    std::u32string u = typeset::utf32(text);
    for (char32_t& c : u) {
        if (c >= 'a' && c <= 'z') c -= 32;
        else if ((c >= 0xE0 && c <= 0xFE && c != 0xF7) || (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) || (c >= 0x430 && c <= 0x44F)) c -= 32;
        else if (c >= 0x450 && c <= 0x45F) c -= 80;
        else if (c >= 0x100 && c <= 0x17F && (c & 1)) c -= 1;   // Latin Extended-A pairs
    }
    return typeset::utf8(u);
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
    if (j.contains("look") && j["look"].is_object()) {
        const json& l = j["look"];
        auto part = [&](const char* k, PartLook& p) {
            if (!l.contains(k) || !l[k].is_object()) return;
            p.bold = l[k].value("bold", p.bold);
            p.italic = l[k].value("italic", p.italic);
            const std::string c = l[k].value("caps", p.caps);
            if (oneOf(c, {"as", "small", "upper"})) p.caps = c;
        };
        part("date", f.look.date);
        part("era", f.look.era);
        part("time", f.look.time);
        const std::string a = l.value("align", f.look.align), pos = l.value("position", f.look.position);
        if (oneOf(a, {"left", "center", "right"})) f.look.align = a;
        if (oneOf(pos, {"under", "header", "above"})) f.look.position = pos;
        f.look.sceneDates = l.value("sceneDates", false);
    }
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
    std::vector<Seg> segs;   // the date line in parts
    auto add = [&](const std::string& t, int part) {
        if (t.empty()) return;
        if (!segs.empty() && segs.back().part == part) segs.back().text += t;
        else segs.push_back({t, part});
    };
    bool eraNamed = false;
    if (f.style == "numeric-dmy") add(pad(d.day, 2) + sep + pad(mnum, 2) + sep + pad(ey.year, 2), 0);
    else if (f.style == "numeric-mdy") add(pad(mnum, 2) + sep + pad(d.day, 2) + sep + pad(ey.year, 2), 0);
    else if (f.style == "numeric-ymd") add(pad(ey.year, 4) + "-" + pad(mnum, 2) + "-" + pad(d.day, 2), 0);
    else if (f.style == "long") add((weekday.empty() ? "" : weekday + ", ") + std::to_string(d.day) + " " + month + " " + std::to_string(ey.year), 0);
    else if (f.style == "month") add(month + " " + std::to_string(ey.year), 0);
    else if (f.style == "custom") {
        const std::string& p = f.pattern;
        for (size_t i = 0; i < p.size();) {
            if (p[i] == '{') {
                const size_t e = p.find('}', i);
                if (e != std::string::npos) {
                    const std::string tok = p.substr(i + 1, e - i - 1);
                    std::string v;
                    int part = 0;
                    bool known = true;
                    if (tok == "D") v = std::to_string(d.day);
                    else if (tok == "DD") v = pad(d.day, 2);
                    else if (tok == "M") v = std::to_string(mnum);
                    else if (tok == "MM") v = pad(mnum, 2);
                    else if (tok == "Month") v = month;
                    else if (tok == "Mon") v = month.substr(0, std::min<size_t>(3, month.size()));
                    else if (tok == "Year" || tok == "YYYY") v = std::to_string(ey.year);
                    else if (tok == "Weekday") v = weekday;
                    else if (tok == "Era") { v = ey.abbr; eraNamed = true; part = 1; }
                    else if (tok == "EraName") { v = ey.name; eraNamed = true; part = 1; }
                    else known = false;
                    if (known) { add(v, part); i = e + 1; continue; }
                }
            }
            add(std::string(1, p[i++]), 0);
        }
        // A weekday-less calendar leaves "{Weekday}, " as ", ": tidy what's left at the start.
        while (!segs.empty() && !segs[0].text.empty() && (segs[0].text[0] == ',' || segs[0].text[0] == ' ')) {
            segs[0].text.erase(0, 1);
            if (segs[0].text.empty()) segs.erase(segs.begin());
        }
    } else add(std::to_string(d.day) + " " + month + " " + std::to_string(ey.year), 0);   // day-month
    // The era, unless the pattern placed it; its separator goes with the date.
    if (!eraNamed && f.era != "none") {
        const std::string tag = f.era == "full" ? (ey.name.empty() ? ey.abbr : ey.name) : (ey.abbr.empty() ? ey.name : ey.abbr);
        if (!tag.empty()) {
            if (f.eraPlace == "before") {
                std::vector<Seg> before{{tag, 1}, {f.eraSeparator, 0}};
                for (const Seg& sg : segs) {
                    if (before.back().part == sg.part) before.back().text += sg.text;
                    else before.push_back(sg);
                }
                segs = std::move(before);
            } else {
                add(f.eraSeparator, 0);
                add(tag, 1);
            }
        }
    }
    if (!t.empty() && f.time != "hidden") {
        const std::string tt = cal::timeOfDayText(c, t);
        if (!tt.empty()) {
            if (f.time == "line") out.timeSegs.push_back({tt, 2});
            else { add(", ", 0); add(tt, 2); }
        }
    }
    out.dateSegs = std::move(segs);
    for (const Seg& sg : out.dateSegs) out.date += sg.text;
    for (const Seg& sg : out.timeSegs) out.time += sg.text;
    out.look = f.look;
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

// ------------------------------------------- typed date lines (5c, 0.107)

namespace {
std::string paragraphText(const json& p) {
    std::string t;
    for (const json& n : p.value("content", json::array())) {
        if (n.value("type", "") == "text") t += n.value("text", "");
        else if (n.value("type", "") == "hard_break") t += " ";
    }
    return t;
}
std::string trimmed(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    return s.substr(i);
}
std::string lowerAscii(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
}  // namespace

std::vector<TypedDate> findTypedDates(const Library& lib, const Id& book, const std::vector<Id>& only) {
    std::vector<TypedDate> out;
    const cal::Calendar c = cal::calendarOfBook(lib, book);
    const cal::StorySpan span = cal::storySpan(lib, book);
    for (const ManuscriptNode* n : lib.manuscriptOf(book)) {
        if (n->kind != NodeKind::Chapter || n->deleted) continue;
        if (!only.empty() && std::find(only.begin(), only.end(), n->id) == only.end()) continue;
        const json doc = json::parse(lib.nodeContent(n->id), nullptr, false);
        if (!doc.is_object() || !doc.contains("content") || !doc["content"].is_array()) continue;
        const json& content = doc["content"];
        // The first block with words in it: a typed date line, or nothing to do.
        for (size_t i = 0; i < content.size(); ++i) {
            const json& b = content[i];
            if (b.value("type", "") != "paragraph") break;
            const std::string text = trimmed(paragraphText(b));
            if (text.empty()) continue;
            if (!typeset::looksLikeDateLine(text)) break;
            TypedDate t;
            t.chapter = n->id;
            t.title = n->title;
            t.index = (int)i;
            t.text = text;
            if (const auto now = lineFor(lib, *n)) t.current = now->date;
            if (c.empty()) t.why = "The book's world has no calendar.";
            else if (!span.on) t.why = "Planning's timeline doesn't count in calendar dates.";
            else {
                // A weekday at the start ("Tuesday, ...") says nothing the date doesn't.
                std::string s = text;
                for (const std::string& wd : c.weekdays)
                    if (!wd.empty() && lowerAscii(s).rfind(lowerAscii(wd), 0) == 0) { s = trimmed(s.substr(wd.size())); if (!s.empty() && s[0] == ',') s = trimmed(s.substr(1)); break; }
                if (const auto dt = cal::parseDateTime(c, s)) {
                    t.readable = true;
                    t.date = dt->date;
                    t.time = dt->time;
                    t.becomes = format(c, t.date, t.time, formatFor(lib, book, n->id)).date;
                } else
                    t.why = "It doesn't read as a date in the calendar.";
            }
            out.push_back(std::move(t));
            break;
        }
    }
    return out;
}

Converted convertTypedDates(Library& lib, const Id& book, const std::vector<TypedDate>& found) {
    Converted done;
    const cal::Calendar c = cal::calendarOfBook(lib, book);
    const cal::StorySpan span = cal::storySpan(lib, book);
    if (c.empty() || !span.on) return done;
    done.timelineBefore = lib.setting("timeline:" + book);
    json tl = json::parse(done.timelineBefore, nullptr, false);
    if (!tl.is_object()) return done;
    if (!tl.contains("nodes") || !tl["nodes"].is_object()) tl["nodes"] = json::object();
    for (const TypedDate& t : found) {
        if (!t.readable) continue;
        const std::string before = lib.nodeContent(t.chapter);
        json doc = json::parse(before, nullptr, false);
        if (!doc.is_object() || !doc.contains("content") || !doc["content"].is_array() || t.index < 0 || t.index >= (int)doc["content"].size()) continue;
        if (trimmed(paragraphText(doc["content"][(size_t)t.index])) != t.text) continue;   // the text changed since it was found
        doc["content"].erase((size_t)t.index);
        if (doc["content"].empty()) doc["content"].push_back({{"type", "paragraph"}});
        // Its day on the timeline (and its time), the chapter's own.
        json node = tl["nodes"].contains(t.chapter) && tl["nodes"][t.chapter].is_object() ? tl["nodes"][t.chapter] : json::object();
        node["day"] = (double)(cal::toDay(c, t.date) - span.start);
        node.erase("time");
        node.erase("part");
        if (t.time.minutes >= 0) node["time"] = t.time.minutes;
        else if (!t.time.part.empty()) node["part"] = t.time.part;
        tl["nodes"][t.chapter] = node;
        done.contentBefore.push_back({t.chapter, before});
        lib.saveNodeContent(t.chapter, doc.dump());
        ++done.count;
    }
    if (done.count) lib.setSetting("timeline:" + book, tl.dump());
    return done;
}

void undoConverted(Library& lib, const Id& book, const Converted& c) {
    for (const auto& [id, text] : c.contentBefore) lib.saveNodeContent(id, text);
    if (c.count) lib.setSetting("timeline:" + book, c.timelineBefore);
}

}  // namespace studio::dates
