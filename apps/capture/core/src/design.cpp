#include "ncap/design.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <sstream>

#include "qrcodegen/qrcodegen.hpp"

namespace ncap::design {

using nlohmann::json;
using sourceutil::colourText;
using sourceutil::parseColour;

namespace {
constexpr float kPi = 3.14159265358979f;
const char* kKinds[] = {"text", "rect", "ellipse", "image", "timer", "qr", "icon", "chat"};
const char* kAnims[] = {"none", "fade", "slide-left", "slide-right", "slide-up", "slide-down", "pop", "wipe"};
float clampf(float v, float a, float b) { return std::isfinite(v) ? std::clamp(v, a, b) : a; }
std::uint8_t mix8(std::uint8_t a, std::uint8_t b, float t) { return (std::uint8_t)std::lround(a + (b - a) * t); }

json colourJson(const Rgba& c) { return colourText(c); }
Rgba colourFrom(const json& j, Rgba def) {
    Rgba c;
    return j.is_string() && parseColour(j.get<std::string>(), c) ? c : def;
}

json fillJson(const Fill& f) {
    json j = {{"type", f.type == Fill::None ? "none" : f.type == Fill::Solid ? "solid" : f.type == Fill::Linear ? "linear" : "radial"}};
    if (f.type == Fill::Solid) {
        j["colour"] = colourJson(f.colour);
        if (!f.brand.empty()) j["brand"] = f.brand;
    }
    if (f.type == Fill::Linear || f.type == Fill::Radial) {
        json s = json::array();
        for (auto& st : f.stops) {
            json o = {{"at", st.at}, {"colour", colourJson(st.colour)}};
            if (!st.brand.empty()) o["brand"] = st.brand;
            s.push_back(o);
        }
        j["stops"] = s;
        j["angle"] = f.angle;
    }
    return j;
}

Fill fillFrom(const json& j, const Fill& def) {
    if (!j.is_object()) return def;
    const std::string t = j.value("type", std::string());
    if (t == "none") return Fill::none();
    auto brandOf = [](const json& o) {
        return o.contains("brand") && o["brand"].is_string() ? o["brand"].get<std::string>().substr(0, 16) : std::string();
    };
    if (t == "solid") {
        Fill f = Fill::solid(colourFrom(j.contains("colour") ? j["colour"] : json(), def.colour));
        f.brand = brandOf(j);
        return f;
    }
    if (t == "linear" || t == "radial") {
        std::vector<Stop> stops;
        if (j.contains("stops") && j["stops"].is_array())
            for (auto& s : j["stops"]) {
                if (!s.is_object() || stops.size() >= 16) continue;
                Stop st;
                st.at = s.contains("at") && s["at"].is_number() ? clampf(s["at"].get<float>(), 0, 1) : 0;
                st.colour = colourFrom(s.contains("colour") ? s["colour"] : json(), {255, 255, 255, 255});
                st.brand = brandOf(s);
                stops.push_back(st);
            }
        if (stops.size() < 2) return def;
        std::stable_sort(stops.begin(), stops.end(), [](const Stop& a, const Stop& b) { return a.at < b.at; });
        const float angle = j.contains("angle") && j["angle"].is_number() ? clampf(j["angle"].get<float>(), -360, 360) : 0;
        Fill f = Fill::linear(stops, angle);
        if (t == "radial") f.type = Fill::Radial;
        return f;
    }
    return def;
}

void corners(const Element& e, float out[8]) {
    const float cx = e.x + e.w / 2, cy = e.y + e.h / 2;
    const float a = e.rotation * kPi / 180, c = std::cos(a), s = std::sin(a);
    const float px[4] = {-e.w / 2, e.w / 2, e.w / 2, -e.w / 2}, py[4] = {-e.h / 2, -e.h / 2, e.h / 2, e.h / 2};
    for (int i = 0; i < 4; ++i) {
        out[i * 2] = cx + px[i] * c - py[i] * s;
        out[i * 2 + 1] = cy + px[i] * s + py[i] * c;
    }
}
}  // namespace

Fill Fill::solid(Rgba c) {
    Fill f;
    f.type = Solid;
    f.colour = c;
    return f;
}
Fill Fill::linear(std::vector<Stop> s, float angle) {
    Fill f;
    f.type = Linear;
    f.stops = std::move(s);
    f.angle = angle;
    return f;
}
Fill Fill::radial(std::vector<Stop> s) {
    Fill f = linear(std::move(s), 0);
    f.type = Radial;
    return f;
}
Fill Fill::none() {
    Fill f;
    f.type = None;
    return f;
}

Rgba colourAt(const Fill& f, float t) {
    if (f.type == Fill::Solid) return f.colour;
    if ((f.type != Fill::Linear && f.type != Fill::Radial) || f.stops.empty()) return {0, 0, 0, 0};
    t = std::clamp(t, 0.0f, 1.0f);
    if (t <= f.stops.front().at) return f.stops.front().colour;
    if (t >= f.stops.back().at) return f.stops.back().colour;
    for (std::size_t i = 1; i < f.stops.size(); ++i)
        if (t <= f.stops[i].at) {
            const Stop &a = f.stops[i - 1], &b = f.stops[i];
            const float u = b.at > a.at ? (t - a.at) / (b.at - a.at) : 1.0f;
            return {mix8(a.colour.r, b.colour.r, u), mix8(a.colour.g, b.colour.g, u), mix8(a.colour.b, b.colour.b, u), mix8(a.colour.a, b.colour.a, u)};
        }
    return f.stops.back().colour;
}

const char* key(Kind k) { return kKinds[(int)k]; }
bool parse(const std::string& s, Kind& out) {
    for (int i = 0; i < 8; ++i)
        if (s == kKinds[i]) {
            out = (Kind)i;
            return true;
        }
    return false;
}
const char* key(Anim a) { return kAnims[(int)a]; }
bool parse(const std::string& s, Anim& out) {
    for (int i = 0; i < 8; ++i)
        if (s == kAnims[i]) {
            out = (Anim)i;
            return true;
        }
    return false;
}

const Element* Document::find(const std::string& id) const {
    for (auto& e : elements)
        if (e.id == id) return &e;
    return nullptr;
}
Element* Document::find(const std::string& id) {
    for (auto& e : elements)
        if (e.id == id) return &e;
    return nullptr;
}
int Document::indexOf(const std::string& id) const {
    for (std::size_t i = 0; i < elements.size(); ++i)
        if (elements[i].id == id) return (int)i;
    return -1;
}

std::string toJson(const Document& d) {
    json els = json::array();
    for (auto& e : d.elements) {
        json j = {{"id", e.id},
                  {"name", e.name},
                  {"kind", key(e.kind)},
                  {"x", e.x},
                  {"y", e.y},
                  {"w", e.w},
                  {"h", e.h},
                  {"rotation", e.rotation},
                  {"opacity", e.opacity},
                  {"visible", e.visible},
                  {"locked", e.locked},
                  {"fill", fillJson(e.fill)},
                  {"stroke", colourJson(e.stroke)},
                  {"strokeWidth", e.strokeWidth},
                  {"radius", e.radius},
                  {"shadow", {{"on", e.shadow.on}, {"dx", e.shadow.dx}, {"dy", e.shadow.dy}, {"blur", e.shadow.blur}, {"colour", colourJson(e.shadow.colour)}}},
                  {"anim", key(e.anim)},
                  {"animMs", e.animMs}};
        if (!e.strokeBrand.empty()) j["strokeBrand"] = e.strokeBrand;
        if (!e.shadow.brand.empty()) j["shadow"]["brand"] = e.shadow.brand;
        if (e.kind == Kind::Timer) {
            j["timer"] = {{"mode", e.timerMode}, {"seconds", e.timerSeconds}, {"until", e.timerUntil}, {"end", e.timerEnd},
                          {"date", e.timerDate}, {"pulse", e.timerPulse}, {"scene", e.timerScene}};
        }
        if (e.kind == Kind::Qr) j["qr"] = e.qr;
        if (e.kind == Kind::Icon) j["icon"] = e.icon;
        if (e.kind == Kind::Chat) {
            j["chat"] = {{"count", e.chatCount}, {"fade", e.chatFade}, {"bubbles", e.chatBubbles}, {"bubble", colourJson(e.chatBubble)},
                         {"bubbleBrand", e.chatBubbleBrand}, {"nameColours", e.chatNameColours}};
        }
        if (e.kind == Kind::Text || e.kind == Kind::Timer || e.kind == Kind::Chat) {
            j["text"] = e.text;
            j["font"] = e.font;
            j["size"] = e.size;
            j["bold"] = e.bold;
            j["italic"] = e.italic;
            j["align"] = e.align;
        }
        if (e.kind == Kind::Image) {
            j["file"] = e.file;
            j["cover"] = e.cover;
        }
        els.push_back(j);
    }
    return json{{"format", "northray-capture-design"}, {"version", 1}, {"name", d.name}, {"width", d.width}, {"height", d.height},
                {"background", fillJson(d.background)}, {"elements", els}, {"nextId", d.nextId}, {"onlyWhilePlaying", d.onlyWhilePlaying}}
        .dump();
}

Document fromJson(const std::string& text) {
    Document d;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return d;
    auto num = [](const json& o, const char* k, float def, float lo, float hi) {
        return o.contains(k) && o[k].is_number() ? clampf(o[k].get<float>(), lo, hi) : def;
    };
    auto flag = [](const json& o, const char* k, bool def) { return o.contains(k) && o[k].is_boolean() ? o[k].get<bool>() : def; };
    auto str = [](const json& o, const char* k, const std::string& def, std::size_t max) {
        return o.contains(k) && o[k].is_string() ? o[k].get<std::string>().substr(0, max) : def;
    };
    d.name = str(j, "name", d.name, 80);
    d.width = (int)num(j, "width", 1920, 16, 8192);
    d.height = (int)num(j, "height", 1080, 16, 8192);
    if (j.contains("background")) d.background = fillFrom(j["background"], Fill::none());
    d.onlyWhilePlaying = j.contains("onlyWhilePlaying") && j["onlyWhilePlaying"].is_boolean() && j["onlyWhilePlaying"].get<bool>();
    int maxId = 0;
    if (j.contains("elements") && j["elements"].is_array())
        for (auto& o : j["elements"]) {
            if (!o.is_object() || d.elements.size() >= 200) continue;
            Element e;
            if (!parse(str(o, "kind", "", 16), e.kind)) continue;
            e.id = str(o, "id", "", 32);
            if (e.id.empty() || d.find(e.id)) continue;
            if (e.id.size() > 1 && e.id[0] == 'e') maxId = std::max(maxId, std::atoi(e.id.c_str() + 1));
            e.name = str(o, "name", "", 80);
            e.x = num(o, "x", 0, -20000, 20000);
            e.y = num(o, "y", 0, -20000, 20000);
            e.w = num(o, "w", 100, 1, 20000);
            e.h = num(o, "h", 100, 1, 20000);
            e.rotation = num(o, "rotation", 0, -360, 360);
            e.opacity = num(o, "opacity", 1, 0, 1);
            e.visible = flag(o, "visible", true);
            e.locked = flag(o, "locked", false);
            e.fill = fillFrom(o.contains("fill") ? o["fill"] : json(), e.kind == Kind::Image ? Fill::none() : Fill::solid({255, 255, 255, 255}));
            e.stroke = colourFrom(o.contains("stroke") ? o["stroke"] : json(), {0, 0, 0, 0});
            e.strokeWidth = num(o, "strokeWidth", 0, 0, 200);
            e.strokeBrand = str(o, "strokeBrand", "", 16);
            e.radius = num(o, "radius", 0, 0, 10000);
            if (o.contains("shadow") && o["shadow"].is_object()) {
                const json& s = o["shadow"];
                e.shadow.on = flag(s, "on", false);
                e.shadow.dx = num(s, "dx", 0, -200, 200);
                e.shadow.dy = num(s, "dy", 6, -200, 200);
                e.shadow.blur = num(s, "blur", 12, 0, 100);
                e.shadow.colour = colourFrom(s.contains("colour") ? s["colour"] : json(), {0, 0, 0, 140});
                e.shadow.brand = str(s, "brand", "", 16);
            }
            parse(str(o, "anim", "none", 16), e.anim);
            e.animMs = (int)num(o, "animMs", 500, 0, 5000);
            e.text = str(o, "text", "", 2000);
            e.font = str(o, "font", "Segoe UI", 64);
            e.size = num(o, "size", 64, 4, 1000);
            e.bold = flag(o, "bold", false);
            e.italic = flag(o, "italic", false);
            e.align = str(o, "align", "left", 8);
            if (e.align != "left" && e.align != "centre" && e.align != "right") e.align = "left";
            e.file = str(o, "file", "", 1024);
            e.qr = str(o, "qr", "", 1000);
            e.icon = str(o, "icon", "", 32);
            if (o.contains("timer") && o["timer"].is_object()) {
                const json& t = o["timer"];
                e.timerMode = str(t, "mode", "countdown", 16);
                if (e.timerMode != "countdown" && e.timerMode != "until" && e.timerMode != "stopwatch" && e.timerMode != "live" && e.timerMode != "clock")
                    e.timerMode = "countdown";
                e.timerSeconds = (int)num(t, "seconds", 300, 1, 359999);
                e.timerUntil = (int)num(t, "until", 20 * 60, 0, 24 * 60 - 1);
                e.timerEnd = str(t, "end", "Starting now!", 200);
                e.timerDate = flag(t, "date", false);
                e.timerPulse = flag(t, "pulse", true);
                e.timerScene = str(t, "scene", "", 64);
            }
            if (o.contains("chat") && o["chat"].is_object()) {
                const json& c = o["chat"];
                e.chatCount = (int)num(c, "count", 8, 1, 30);
                e.chatFade = (int)num(c, "fade", 0, 0, 3600);
                e.chatBubbles = flag(c, "bubbles", true);
                e.chatBubble = colourFrom(c.contains("bubble") ? c["bubble"] : json(), {11, 26, 46, 210});
                e.chatBubbleBrand = str(c, "bubbleBrand", "panel", 16);
                e.chatNameColours = flag(c, "nameColours", true);
            }
            e.cover = flag(o, "cover", true);
            d.elements.push_back(e);
        }
    d.nextId = std::max((int)num(j, "nextId", 1, 1, 1000000), maxId + 1);
    return d;
}

std::string add(Document& d, Element e) {
    if (d.elements.size() >= 200) return {};
    e.id = "e" + std::to_string(d.nextId++);
    while (d.find(e.id)) e.id = "e" + std::to_string(d.nextId++);
    if (e.name.empty()) {
        static const char* names[] = {"Text", "Rectangle", "Ellipse", "Picture", "Timer", "QR code", "Icon", "Chat"};
        e.name = names[(int)e.kind];
    }
    d.elements.push_back(e);
    return d.elements.back().id;
}

bool remove(Document& d, const std::string& id) {
    const int i = d.indexOf(id);
    if (i < 0) return false;
    d.elements.erase(d.elements.begin() + i);
    return true;
}

std::string duplicate(Document& d, const std::string& id) {
    const Element* e = d.find(id);
    if (!e) return {};
    Element c = *e;
    c.x += 20;
    c.y += 20;
    c.name += " copy";
    const int at = d.indexOf(id);
    const std::string nid = add(d, c);
    if (nid.empty()) return {};
    // Just above the original.
    Element moved = d.elements.back();
    d.elements.pop_back();
    d.elements.insert(d.elements.begin() + at + 1, moved);
    return nid;
}

bool move(Document& d, const std::string& id, int delta) {
    const int i = d.indexOf(id), j = i + delta;
    if (i < 0 || j < 0 || j >= (int)d.elements.size()) return false;
    std::swap(d.elements[(std::size_t)i], d.elements[(std::size_t)j]);
    return true;
}
bool toTop(Document& d, const std::string& id) {
    const int i = d.indexOf(id);
    if (i < 0 || i + 1 == (int)d.elements.size()) return false;
    Element e = d.elements[(std::size_t)i];
    d.elements.erase(d.elements.begin() + i);
    d.elements.push_back(e);
    return true;
}
bool toBottom(Document& d, const std::string& id) {
    const int i = d.indexOf(id);
    if (i <= 0) return false;
    Element e = d.elements[(std::size_t)i];
    d.elements.erase(d.elements.begin() + i);
    d.elements.insert(d.elements.begin(), e);
    return true;
}

std::string hitTest(const Document& d, float x, float y) {
    for (auto it = d.elements.rbegin(); it != d.elements.rend(); ++it) {
        const Element& e = *it;
        if (!e.visible || e.locked) continue;
        const float cx = e.x + e.w / 2, cy = e.y + e.h / 2;
        const float a = -e.rotation * kPi / 180;
        const float lx = (x - cx) * std::cos(a) - (y - cy) * std::sin(a), ly = (x - cx) * std::sin(a) + (y - cy) * std::cos(a);
        if (std::fabs(lx) > e.w / 2 || std::fabs(ly) > e.h / 2) continue;
        if (e.kind == Kind::Ellipse && (lx * lx) / (e.w * e.w / 4) + (ly * ly) / (e.h * e.h / 4) > 1) continue;
        return e.id;
    }
    return {};
}

Snapped snap(const Document& d, const std::string& movingId, Box p, float threshold, bool resizing) {
    std::vector<float> xs = {0, d.width / 2.0f, (float)d.width}, ys = {0, d.height / 2.0f, (float)d.height};
    for (auto& e : d.elements) {
        if (e.id == movingId || !e.visible || std::fabs(e.rotation) > 0.01f) continue;
        xs.insert(xs.end(), {e.x, e.x + e.w / 2, e.x + e.w});
        ys.insert(ys.end(), {e.y, e.y + e.h / 2, e.y + e.h});
    }
    Snapped out;
    auto best = [&](const std::vector<float>& lines, const std::vector<float>& mine, float& delta, float& line) {
        bool found = false;
        float bd = threshold + 1;
        for (float l : lines)
            for (float m : mine)
                if (std::fabs(l - m) <= threshold && std::fabs(l - m) < bd) {
                    bd = std::fabs(l - m);
                    delta = l - m;
                    line = l;
                    found = true;
                }
        return found;
    };
    float dx = 0, dy = 0, lx = 0, ly = 0;
    const std::vector<float> mx = resizing ? std::vector<float>{p.x + p.w} : std::vector<float>{p.x, p.x + p.w / 2, p.x + p.w};
    const std::vector<float> my = resizing ? std::vector<float>{p.y + p.h} : std::vector<float>{p.y, p.y + p.h / 2, p.y + p.h};
    if (best(xs, mx, dx, lx)) {
        if (resizing) p.w = std::max(1.0f, p.w + dx);
        else p.x += dx;
        out.guidesX.push_back(lx);
    }
    if (best(ys, my, dy, ly)) {
        if (resizing) p.h = std::max(1.0f, p.h + dy);
        else p.y += dy;
        out.guidesY.push_back(ly);
    }
    out.box = p;
    return out;
}

Pose pose(const Element& e, float t, int pageW, int pageH) {
    Pose p;
    if (e.anim == Anim::None || e.animMs <= 0 || t >= e.animMs) return p;
    const float u = std::clamp(t / (float)e.animMs, 0.0f, 1.0f);
    const float k = 1 - (1 - u) * (1 - u) * (1 - u);   // ease out
    switch (e.anim) {
        case Anim::None: break;
        case Anim::Fade: p.opacity = k; break;
        case Anim::SlideLeft: p.dx = -(e.x + e.w) * (1 - k); p.opacity = std::min(1.0f, u * 3); break;          // in from the left edge
        case Anim::SlideRight: p.dx = (pageW - e.x) * (1 - k); p.opacity = std::min(1.0f, u * 3); break;        // from the right
        case Anim::SlideUp: p.dy = (pageH - e.y) * (1 - k); p.opacity = std::min(1.0f, u * 3); break;           // up from the bottom
        case Anim::SlideDown: p.dy = -(e.y + e.h) * (1 - k); p.opacity = std::min(1.0f, u * 3); break;          // down from the top
        case Anim::Pop: {
            // Grows past full size a little, then settles.
            const float s = 1.70158f, v = u - 1;
            p.scale = std::max(0.0f, 1 + (s + 1) * v * v * v + s * v * v);
            p.opacity = std::min(1.0f, u * 4);
            break;
        }
        case Anim::Wipe: p.reveal = k; break;
    }
    return p;
}

bool animating(const Document& d, float t) {
    for (auto& e : d.elements)
        if (e.visible && e.anim != Anim::None && t < e.animMs) return true;
    return false;
}

int entranceMs(const Document& d) {
    int ms = 0;
    for (auto& e : d.elements)
        if (e.visible && e.anim != Anim::None) ms = std::max(ms, e.animMs);
    return ms;
}

std::string fillPlaceholders(const std::string& text, const std::vector<std::pair<std::string, std::string>>& values, bool showMissing) {
    auto fill = [&](std::string part, bool& emptied) {
        bool hadTag = false, allEmpty = true;
        for (auto& [k, v] : values) {
            const std::string tag = "{" + k + "}";
            for (std::size_t at = 0; (at = part.find(tag, at)) != std::string::npos;) {
                hadTag = true;
                const std::string with = v.empty() && showMissing ? tag : v;
                if (!v.empty()) allEmpty = false;
                part.replace(at, tag.size(), with);
                at += with.size();
            }
        }
        std::string trimmed = part;
        trimmed.erase(0, trimmed.find_first_not_of(' '));
        trimmed.erase(trimmed.find_last_not_of(' ') + 1);
        emptied = hadTag && allEmpty && !showMissing && trimmed.empty();
        return part;
    };
    const std::string dot = "·";
    std::string out;
    std::size_t start = 0;
    while (true) {   // line by line
        const std::size_t nl = text.find('\n', start);
        const std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (line.find(dot) == std::string::npos) {
            bool e = false;
            out += fill(line, e);
        } else {
            std::vector<std::string> kept;
            std::size_t s = 0;
            while (true) {
                const std::size_t d = line.find(dot, s);
                std::string part = line.substr(s, d == std::string::npos ? std::string::npos : d - s);
                bool emptied = false;
                part = fill(part, emptied);
                part.erase(0, part.find_first_not_of(' '));
                part.erase(part.find_last_not_of(' ') + 1);
                if (!emptied && !part.empty()) kept.push_back(part);
                if (d == std::string::npos) break;
                s = d + dot.size();
            }
            for (std::size_t i = 0; i < kept.size(); ++i) out += (i ? "   " + dot + "   " : "") + kept[i];
        }
        if (nl == std::string::npos) break;
        out += '\n';
        start = nl + 1;
    }
    return out;
}

// ------------------------------------------------------------------ timers, QR codes, icons

std::string durationText(std::int64_t ms) {
    if (ms < 0) ms = 0;
    const std::int64_t s = ms / 1000, h = s / 3600, m = (s / 60) % 60, sec = s % 60;
    char b[32];
    if (h > 0) std::snprintf(b, sizeof b, "%lld:%02lld:%02lld", (long long)h, (long long)m, (long long)sec);
    else std::snprintf(b, sizeof b, "%lld:%02lld", (long long)m, (long long)sec);
    return b;
}

TimerShown timerText(const Element& e, const Clock& c) {
    TimerShown t;
    auto counting = [&](std::int64_t remaining) {
        t.remainingMs = std::max<std::int64_t>(0, remaining);
        if (remaining <= 0) {
            t.ended = true;
            t.text = e.timerEnd;
            return;
        }
        t.text = durationText(remaining + 999);   // whole seconds left, rounded up: 0:01 until it's zero
        if (e.timerPulse && remaining <= 10000) {
            // Each second: bigger as it ticks, settling back.
            const float within = (float)(remaining % 1000) / 1000.0f;   // 1 → 0 through the second
            t.pulse = 1 + 0.12f * within * within;
        }
    };
    if (e.timerMode == "countdown") {
        counting((std::int64_t)e.timerSeconds * 1000 - c.elapsedMs);
    } else if (e.timerMode == "until") {
        // To the next time it's that time of day; a time passed less than twelve hours ago has ended.
        std::int64_t diff = (std::int64_t)e.timerUntil * 60 - c.localSeconds;
        if (diff < -12 * 3600) diff += 24 * 3600;
        counting(diff * 1000);
    } else if (e.timerMode == "stopwatch") {
        t.text = durationText(c.elapsedMs);
    } else if (e.timerMode == "live") {
        t.text = c.liveMs < 0 ? "Not live" : durationText(c.liveMs);
    } else {   // clock, 24-hour
        char b[16];
        std::snprintf(b, sizeof b, "%02d:%02d", (c.localSeconds / 3600) % 24, (c.localSeconds / 60) % 60);
        t.text = b;
        if (e.timerDate && !c.dateText.empty()) t.text += "\n" + c.dateText;
    }
    if (!e.text.empty() && !t.ended) {   // words around it: "Starting in {time}"
        std::string w = e.text;
        const std::size_t at = w.find("{time}");
        t.text = at == std::string::npos ? w + " " + t.text : w.replace(at, 6, t.text);
    }
    return t;
}

bool usesTime(const Document& d) {
    for (auto& e : d.elements)
        if (e.visible && (e.kind == Kind::Timer || (e.kind == Kind::Chat && e.chatFade > 0))) return true;
    return false;
}

bool usesChat(const Document& d) {
    for (auto& e : d.elements)
        if (e.visible && e.kind == Kind::Chat) return true;
    return false;
}

const std::vector<Icon>& icons() {
    // Drawn for NorthRay: simple, filled, 24 x 24.
    static const std::vector<Icon> v = {
        {"star", "Star", "M12 2 L14.9 8.6 L22 9.3 L16.6 14 L18.2 21 L12 17.3 L5.8 21 L7.4 14 L2 9.3 L9.1 8.6 Z"},
        {"heart", "Heart", "M12 21 C5 15.5 2 12.5 2 8.5 C2 5.5 4.3 3.5 7 3.5 C9 3.5 10.8 4.6 12 6.3 C13.2 4.6 15 3.5 17 3.5 C19.7 3.5 22 5.5 22 8.5 C22 12.5 19 15.5 12 21 Z"},
        {"arrow", "Arrow", "M3 10.5 L14 10.5 L14 5 L21.5 12 L14 19 L14 13.5 L3 13.5 Z"},
        {"check", "Tick", "M3.5 12.5 L6 10 L9.5 13.5 L18 5 L20.5 7.5 L9.5 18.5 Z"},
        {"play", "Play", "M7 4 L20 12 L7 20 Z"},
        {"chat", "Chat", "M3 4 L21 4 L21 16 L10 16 L5 20.5 L5.5 16 L3 16 Z"},
        {"music", "Music", "M9 4 L20 2 L20 15.5 C20 17.4 18.4 18.8 16.5 18.8 C14.6 18.8 13.5 17.6 13.5 16.2 C13.5 14.6 14.9 13.4 16.6 13.4 C17.2 13.4 17.7 13.5 18 13.7 L18 6.4 L11 7.7 L11 18 C11 19.9 9.4 21.3 7.5 21.3 C5.6 21.3 4.5 20.1 4.5 18.7 C4.5 17.1 5.9 15.9 7.6 15.9 C8.2 15.9 8.7 16 9 16.2 Z"},
        {"camera", "Camera", "M2 7 L7 7 L9 4 L15 4 L17 7 L22 7 L22 20 L2 20 Z"},
        {"pin", "Place", "M12 22 C7 15.5 4.5 12 4.5 9 C4.5 4.9 7.9 2 12 2 C16.1 2 19.5 4.9 19.5 9 C19.5 12 17 15.5 12 22 Z"},
        {"mail", "Mail", "M2 5 L22 5 L22 19 L2 19 Z"},
        {"bell", "Bell", "M12 2.5 C15.6 2.5 18 5.3 18 9 L18 14 L20.5 17.5 L3.5 17.5 L6 14 L6 9 C6 5.3 8.4 2.5 12 2.5 Z M9.5 19 L14.5 19 C14.5 20.4 13.4 21.5 12 21.5 C10.6 21.5 9.5 20.4 9.5 19 Z"},
        {"calendar", "Calendar", "M3 5 L7 5 L7 2.5 L9 2.5 L9 5 L15 5 L15 2.5 L17 2.5 L17 5 L21 5 L21 21 L3 21 Z"},
        {"clock", "Clock", "O 12 12 10"},
        {"dot", "Dot", "O 12 12 6"},
        {"book", "Book", "M3 4 C6 3.5 9.5 3.8 11.5 5.5 L11.5 20.5 C9.5 19 6 18.7 3 19.2 Z M12.5 5.5 C14.5 3.8 18 3.5 21 4 L21 19.2 C18 18.7 14.5 19 12.5 20.5 Z"},
        {"brush", "Brush", "M20.5 2.5 L22 4 L13 14.5 L10 11.5 Z M9 12.5 L12 15.5 C11.5 18.5 9.5 21 5.5 21 C3.5 21 2.5 20.5 2 20 C4 19.5 4.5 18 5 16 C5.5 14 7 12.5 9 12.5 Z"},
        {"mic", "Microphone", "M9 3.5 C9 2.4 10.3 1.5 12 1.5 C13.7 1.5 15 2.4 15 3.5 L15 11.5 C15 12.6 13.7 13.5 12 13.5 C10.3 13.5 9 12.6 9 11.5 Z M5.5 10 L7 10 C7 13 9.2 15.2 12 15.2 C14.8 15.2 17 13 17 10 L18.5 10 C18.5 13.5 16 16.3 12.8 16.7 L12.8 20 L16 20 L16 22 L8 22 L8 20 L11.2 20 L11.2 16.7 C8 16.3 5.5 13.5 5.5 10 Z"},
        {"globe", "Globe", "O 12 12 10"},
        {"sparkle", "Sparkle", "M12 2 C12.8 7.5 16.5 11.2 22 12 C16.5 12.8 12.8 16.5 12 22 C11.2 16.5 7.5 12.8 2 12 C7.5 11.2 11.2 7.5 12 2 Z"},
        {"moon", "Moon", "M15 2.5 C10 3.5 7 7.5 7 12 C7 16.5 10 20.5 15 21.5 C9 22.5 3 18 3 12 C3 6 9 1.5 15 2.5 Z"},
    };
    return v;
}

const Icon* iconById(const std::string& id) {
    for (auto& i : icons())
        if (id == i.id) return &i;
    return nullptr;
}

std::vector<std::vector<std::pair<float, float>>> iconFigures(const std::string& path) {
    std::vector<std::vector<std::pair<float, float>>> figs;
    std::string spaced;   // "M12 2" → "M 12 2"
    for (char ch : path) {
        if (std::isalpha((unsigned char)ch)) spaced += std::string(" ") + ch + " ";
        else spaced += ch;
    }
    std::stringstream ss(spaced);
    std::string tok;
    std::vector<std::pair<float, float>>* cur = nullptr;
    float lx = 0, ly = 0;
    auto rd = [&](float& v) { return static_cast<bool>(ss >> v); };
    while (ss >> tok) {
        if (tok == "M") {
            float x, y;
            if (!rd(x) || !rd(y)) break;
            figs.push_back({{x, y}});
            cur = &figs.back();
            lx = x, ly = y;
        } else if (tok == "L" && cur) {
            float x, y;
            if (!rd(x) || !rd(y)) break;
            cur->push_back({x, y});
            lx = x, ly = y;
        } else if (tok == "C" && cur) {
            float x1, y1, x2, y2, x, y;
            if (!rd(x1) || !rd(y1) || !rd(x2) || !rd(y2) || !rd(x) || !rd(y)) break;
            for (int i = 1; i <= 12; ++i) {   // flattened into short lines
                const float t = i / 12.0f, u = 1 - t;
                cur->push_back({u * u * u * lx + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x,
                                u * u * u * ly + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y});
            }
            lx = x, ly = y;
        } else if (tok == "O") {
            float cx, cy, r;
            if (!rd(cx) || !rd(cy) || !rd(r)) break;
            figs.push_back({});
            for (int i = 0; i < 48; ++i) figs.back().push_back({cx + r * std::cos(2 * kPi * i / 48), cy + r * std::sin(2 * kPi * i / 48)});
            cur = nullptr;
        } else if (tok == "Z") {
            cur = nullptr;
        }
    }
    return figs;
}

// ------------------------------------------------------------------ templates

namespace {
Rgba hex(const char* h) {
    Rgba c;
    parseColour(h, c);
    return c;
}
Element textEl(const char* name, const char* text, float x, float y, float w, float h, float size, const char* colour, bool bold,
               const char* align = "centre") {
    Element e;
    e.kind = Kind::Text;
    e.name = name;
    e.text = text;
    e.x = x, e.y = y, e.w = w, e.h = h;
    e.size = size;
    e.bold = bold;
    e.align = align;
    e.font = "Segoe UI";
    e.fill = Fill::solid(hex(colour));
    return e;
}
Element rectEl(const char* name, float x, float y, float w, float h, Fill f, float radius = 0) {
    Element e;
    e.kind = Kind::Rect;
    e.name = name;
    e.x = x, e.y = y, e.w = w, e.h = h;
    e.fill = f;
    e.radius = radius;
    return e;
}
Element ellipseEl(const char* name, float x, float y, float w, float h, Fill f) {
    Element e = rectEl(name, x, y, w, h, f);
    e.kind = Kind::Ellipse;
    return e;
}
// The NorthRay night with aurora light: a deep gradient and two soft glows.
void auroraPage(Document& d) {
    d.background = Fill::linear({{0, hex("#060B17")}, {0.55f, hex("#0B1A2E")}, {1, hex("#0E2A33")}}, 90);
    add(d, ellipseEl("Aurora (green)", -0.15f * d.width, -0.45f * d.height, 0.85f * d.width, 0.95f * d.height,
                     Fill::radial({{0, hex("#3FCF8E55")}, {0.55f, hex("#3FCF8E22")}, {1, hex("#3FCF8E00")}})));
    add(d, ellipseEl("Aurora (violet)", 0.4f * d.width, -0.35f * d.height, 0.75f * d.width, 0.8f * d.height,
                     Fill::radial({{0, hex("#7A5CFF4A")}, {0.6f, hex("#7A5CFF1C")}, {1, hex("#7A5CFF00")}})));
}
Document holding(const char* name, const char* title, const char* sub) {
    Document d;
    d.name = name;
    auroraPage(d);
    Element t = textEl("Title", title, 160, 380, 1600, 200, 132, "#F4F7FB", true);
    t.shadow = {true, 0, 8, 24, hex("#00000099")};
    t.anim = Anim::Pop;
    add(d, t);
    Element s = textEl("Subtitle", sub, 260, 580, 1400, 80, 44, "#9FB3C8", false);
    s.anim = Anim::Fade;
    s.animMs = 900;
    add(d, s);
    Element bar = rectEl("Accent line", 810, 690, 300, 6, Fill::linear({{0, hex("#3FCF8E")}, {1, hex("#7A5CFF")}}, 0), 3);
    bar.anim = Anim::Wipe;
    bar.animMs = 800;
    add(d, bar);
    Element soc = textEl("Socials", "{twitch}   ·   {youtube}   ·   {instagram}", 260, 900, 1400, 60, 34, "#CFE3F0", false);
    add(d, soc);
    return d;
}
}  // namespace

const std::vector<Template>& templates() {
    static const std::vector<Template> t = [] {
        std::vector<Template> v;
        v.push_back({"starting-soon", "Starting Soon", "Holding screens", holding("Starting Soon", "Starting soon", "Grab a drink, we'll be live in a moment")});
        v.push_back({"be-right-back", "Be Right Back", "Holding screens", holding("Be Right Back", "Be right back", "Don't go anywhere")});
        v.push_back({"ending", "Ending", "Holding screens", holding("Ending", "Thanks for watching", "See you next stream")});
        {
            Document d;
            d.name = "Lower third";
            d.width = 1000;
            d.height = 180;
            Element bg = rectEl("Bar", 20, 30, 900, 120, Fill::linear({{0, hex("#0B1A2EF0")}, {1, hex("#10213AE0")}}, 0), 18);
            bg.shadow = {true, 0, 8, 20, hex("#00000080")};
            bg.anim = Anim::SlideLeft;
            add(d, bg);
            Element acc = rectEl("Accent", 20, 30, 12, 120, Fill::linear({{0, hex("#3FCF8E")}, {1, hex("#7A5CFF")}}, 90), 6);
            acc.anim = Anim::SlideLeft;
            add(d, acc);
            Element n = textEl("Name", "{name}", 60, 44, 820, 56, 46, "#F4F7FB", true, "left");
            n.anim = Anim::SlideLeft;
            add(d, n);
            Element r = textEl("Role", "Author · Artist · Broadcaster", 60, 100, 820, 40, 28, "#9FB3C8", false, "left");
            r.anim = Anim::SlideLeft;
            r.animMs = 700;
            add(d, r);
            v.push_back({"lower-third", "Lower third", "On stream", d});
        }
        {
            Document d;
            d.name = "Facecam frame";
            d.width = 680;
            d.height = 420;
            Element f = rectEl("Frame", 20, 20, 640, 360, Fill::none(), 28);
            f.stroke = hex("#3FCF8E");
            f.strokeWidth = 8;
            f.shadow = {true, 0, 0, 24, hex("#3FCF8E88")};
            add(d, f);
            Element tag = rectEl("Name tag", 40, 352, 260, 52, Fill::linear({{0, hex("#3FCF8E")}, {1, hex("#7A5CFF")}}, 0), 26);
            add(d, tag);
            add(d, textEl("Name", "{name}", 40, 358, 260, 40, 28, "#06101C", true));
            v.push_back({"facecam-frame", "Facecam frame", "On stream", d});
        }
        {
            Document d;
            d.name = "Socials bar";
            d.width = 1200;
            d.height = 90;
            add(d, rectEl("Bar", 0, 10, 1200, 70, Fill::solid(hex("#0B1A2ED0")), 35));
            add(d, textEl("Handles", "{twitch}   ·   {youtube}   ·   {instagram}", 30, 22, 1140, 46, 30, "#E8F1F7", false));
            v.push_back({"socials-bar", "Socials bar", "On stream", d});
        }
        {
            Document d;
            d.name = "Twitch panel";
            d.width = 320;
            d.height = 100;
            d.background = Fill::linear({{0, hex("#0B1A2E")}, {1, hex("#13304A")}}, 0);
            add(d, rectEl("Accent", 0, 92, 320, 8, Fill::linear({{0, hex("#3FCF8E")}, {1, hex("#7A5CFF")}}, 0)));
            add(d, textEl("Label", "ABOUT ME", 0, 26, 320, 48, 36, "#F4F7FB", true));
            v.push_back({"twitch-panel", "Twitch panel (320 × 100)", "Panels", d});
        }
        {
            Document d = holding("Offline banner", "Offline", "{schedule}");
            d.name = "Offline banner";
            v.push_back({"offline-banner", "Offline banner (1920 × 1080)", "Panels", d});
        }
        {
            Document d;
            d.name = "Logo watermark";
            d.width = 400;
            d.height = 400;
            Element e;
            e.kind = Kind::Image;
            e.name = "Logo";
            e.x = 0, e.y = 0, e.w = 400, e.h = 400;
            e.file = kBrandLogo;
            e.cover = false;
            e.fill = Fill::none();
            e.opacity = 0.6f;
            add(d, e);
            v.push_back({"logo-watermark", "Logo watermark (your brand logo)", "On stream", d});
        }
        {
            Document d;
            d.name = "Chat box";
            d.width = 600;
            d.height = 700;
            Element e;
            e.kind = Kind::Chat;
            e.name = "Chat";
            e.x = 0, e.y = 0, e.w = 600, e.h = 700;
            e.fill = Fill::solid(hex("#F4F7FB"));
            e.size = 28;
            e.chatCount = 8;
            add(d, e);
            v.push_back({"chat-box", "Chat box (your Twitch chat, live)", "On stream", d});
        }
        {
            Document d;
            d.name = "Now playing";
            d.width = 760;
            d.height = 150;
            d.onlyWhilePlaying = true;
            Element card;
            card.kind = Kind::Rect;
            card.name = "Card";
            card.x = 10, card.y = 10, card.w = 740, card.h = 130;
            card.radius = 28;
            card.fill = Fill::solid(hex("#0B1A2EE6"));
            card.stroke = hex("#3FCF8E55");
            card.strokeWidth = 2;
            card.anim = Anim::SlideLeft;
            add(d, card);
            Element ic;
            ic.kind = Kind::Icon;
            ic.icon = "music";
            ic.name = "Music";
            ic.x = 40, ic.y = 35, ic.w = 80, ic.h = 80;
            ic.fill = Fill::linear({{0, hex("#3FCF8E")}, {1, hex("#7A5CFF")}}, 0);
            ic.anim = Anim::SlideLeft;
            add(d, ic);
            Element k;
            k.kind = Kind::Text;
            k.name = "Label";
            k.text = "NOW PLAYING";
            k.x = 145, k.y = 26, k.w = 580, k.h = 28;
            k.size = 20;
            k.bold = true;
            k.fill = Fill::solid(hex("#3FCF8E"));
            k.anim = Anim::SlideLeft;
            add(d, k);
            Element t;
            t.kind = Kind::Text;
            t.name = "Song";
            t.text = "{song}";
            t.x = 145, t.y = 52, t.w = 580, t.h = 48;
            t.size = 38;
            t.bold = true;
            t.fill = Fill::solid(hex("#F4F7FB"));
            t.anim = Anim::SlideLeft;
            add(d, t);
            Element a;
            a.kind = Kind::Text;
            a.name = "Artist";
            a.text = "{artist}";
            a.x = 145, a.y = 98, a.w = 580, a.h = 34;
            a.size = 26;
            a.fill = Fill::solid(hex("#9FB3C8"));
            a.anim = Anim::SlideLeft;
            add(d, a);
            v.push_back({"now-playing", "Now playing (your music's title and artist)", "On stream", d});
        }
        // Keep each group together (the watermark was added last).
        std::stable_sort(v.begin(), v.end(), [](const Template& x, const Template& y) {
            auto rank = [](const char* g) { return std::string(g) == "Holding screens" ? 0 : std::string(g) == "On stream" ? 1 : 2; };
            return rank(x.group) < rank(y.group);
        });
        const Brand b = defaultBrand();
        for (auto& x : v) linkToBrand(x.doc, b);
        return v;
    }();
    return t;
}

std::vector<std::vector<bool>> qrModules(const std::string& text) {
    std::vector<std::vector<bool>> m;
    if (text.empty() || text.size() > 1000) return m;
    try {
        const qrcodegen::QrCode q = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        const int n = q.getSize();
        m.assign((std::size_t)n, std::vector<bool>((std::size_t)n));
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) m[(std::size_t)y][(std::size_t)x] = q.getModule(x, y);
    } catch (...) {
        m.clear();
    }
    return m;
}


// ------------------------------------------------------------------ brand kit

const std::vector<std::string>& brandKeys() {
    static const std::vector<std::string> k = {"primary", "secondary", "text", "soft", "panel", "night"};
    return k;
}

Brand defaultBrand() {
    Brand b;
    b.colours = {{"primary", "Main colour", hex("#3FCF8E")}, {"secondary", "Second colour", hex("#7A5CFF")},
                 {"text", "Text", hex("#F4F7FB")},          {"soft", "Soft text", hex("#9FB3C8")},
                 {"panel", "Panels", hex("#0B1A2E")},       {"night", "Background", hex("#060B17")}};
    return b;
}

std::string toJson(const Brand& b) {
    json c = json::object();
    for (auto& x : b.colours) c[x.key] = colourText(x.colour);
    json f = json::array();
    for (auto& x : b.fontFiles) f.push_back(x);
    return json{{"format", "northray-brand"}, {"version", 1}, {"colours", c}, {"headingFont", b.headingFont},
                {"bodyFont", b.bodyFont}, {"fontFiles", f}, {"logo", b.logo},
                {"stinger", b.stinger}, {"stingerCutMs", b.stingerCutMs}, {"intro", b.intro}, {"outro", b.outro}}
        .dump(2);
}

Brand brandFromJson(const std::string& text) {
    Brand b = defaultBrand();
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return b;
    if (j.contains("colours") && j["colours"].is_object())
        for (auto& x : b.colours) {
            Rgba c;
            if (j["colours"].contains(x.key) && j["colours"][x.key].is_string() && parseColour(j["colours"][x.key].get<std::string>(), c)) {
                c.a = 255;   // brand colours are solid; designs choose how see-through
                x.colour = c;
            }
        }
    auto font = [&](const char* k, std::string& out) {
        if (j.contains(k) && j[k].is_string()) {
            const std::string v = j[k].get<std::string>().substr(0, 64);
            if (!v.empty() && v.rfind("brand:", 0) != 0) out = v;
        }
    };
    font("headingFont", b.headingFont);
    font("bodyFont", b.bodyFont);
    if (j.contains("fontFiles") && j["fontFiles"].is_array())
        for (auto& x : j["fontFiles"])
            if (x.is_string() && b.fontFiles.size() < 16 && !x.get<std::string>().empty() && x.get<std::string>().size() <= 1024)
                b.fontFiles.push_back(x.get<std::string>());
    if (j.contains("logo") && j["logo"].is_string() && j["logo"].get<std::string>().size() <= 1024) b.logo = j["logo"].get<std::string>();
    auto path = [&](const char* k, std::string& out) {
        if (j.contains(k) && j[k].is_string() && j[k].get<std::string>().size() <= 1024) out = j[k].get<std::string>();
    };
    path("stinger", b.stinger);
    path("intro", b.intro);
    path("outro", b.outro);
    if (j.contains("stingerCutMs") && j["stingerCutMs"].is_number_integer()) b.stingerCutMs = std::clamp(j["stingerCutMs"].get<int>(), 0, 10000);
    return b;
}

const BrandColour* brandColour(const Brand& b, const std::string& key) {
    for (auto& x : b.colours)
        if (x.key == key) return &x;
    return nullptr;
}

namespace {
// Every linkable colour in a design: (the colour, its link).
template <class D, class F>
void eachColour(D& d, F f) {
    auto fill = [&](auto& fl) {
        if (fl.type == Fill::Solid) f(fl.colour, fl.brand);
        if (fl.type == Fill::Linear || fl.type == Fill::Radial)
            for (auto& st : fl.stops) f(st.colour, st.brand);
    };
    fill(d.background);
    for (auto& e : d.elements) {
        fill(e.fill);
        f(e.stroke, e.strokeBrand);
        f(e.shadow.colour, e.shadow.brand);
        if (e.kind == Kind::Chat) f(e.chatBubble, e.chatBubbleBrand);
    }
}
}  // namespace

Document applyBrand(const Document& d, const Brand& b) {
    Document out = d;
    eachColour(out, [&](Rgba& c, std::string& link) {
        if (link.empty()) return;
        if (const BrandColour* x = brandColour(b, link)) c = {x->colour.r, x->colour.g, x->colour.b, c.a};
    });
    for (auto& e : out.elements) {
        if (e.font == kBrandHeading) e.font = b.headingFont;
        else if (e.font == kBrandBody) e.font = b.bodyFont;
        if (e.file == kBrandLogo) e.file = b.logo;
    }
    return out;
}

void linkToBrand(Document& d, const Brand& b) {
    eachColour(d, [&](Rgba& c, std::string& link) {
        if (!link.empty() || c.a == 0) return;
        for (auto& x : b.colours)
            if (x.colour.r == c.r && x.colour.g == c.g && x.colour.b == c.b) {
                link = x.key;
                return;
            }
    });
    for (auto& e : d.elements)
        if ((e.kind == Kind::Text || e.kind == Kind::Timer || e.kind == Kind::Chat) && e.font == "Segoe UI") e.font = e.bold ? kBrandHeading : kBrandBody;
}

bool usesBrand(const Document& d) {
    bool any = false;
    eachColour(d, [&](const Rgba&, const std::string& link) { any = any || !link.empty(); });
    for (auto& e : d.elements) any = any || e.font == kBrandHeading || e.font == kBrandBody || e.file == kBrandLogo;
    return any;
}

Document unlinkBrand(const Document& d, const Brand& b) {
    Document out = applyBrand(d, b);
    eachColour(out, [](Rgba&, std::string& link) { link.clear(); });
    return out;
}

}  // namespace ncap::design
