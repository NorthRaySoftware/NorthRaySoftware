#include "ncap/chat.hpp"

#include <algorithm>
#include <cctype>

namespace ncap::chat {

std::string unescapeTag(const std::string& v) {
    std::string out;
    out.reserve(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i] != '\\') {
            out += v[i];
            continue;
        }
        if (++i >= v.size()) break;   // a lone trailing backslash is dropped
        switch (v[i]) {
        case 's': out += ' '; break;
        case ':': out += ';'; break;
        case '\\': out += '\\'; break;
        case 'r': out += '\r'; break;
        case 'n': out += '\n'; break;
        default: out += v[i];
        }
    }
    return out;
}

bool parseLine(const std::string& raw, IrcLine& out) {
    out = IrcLine{};
    std::string s = raw;
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    std::size_t p = 0;
    auto word = [&]() {
        const std::size_t sp = s.find(' ', p);
        std::string w = s.substr(p, sp == std::string::npos ? std::string::npos : sp - p);
        p = sp == std::string::npos ? s.size() : sp + 1;
        while (p < s.size() && s[p] == ' ') ++p;
        return w;
    };
    if (p < s.size() && s[p] == '@') {
        ++p;
        const std::string tags = word();
        std::size_t q = 0;
        while (q <= tags.size()) {
            const std::size_t semi = tags.find(';', q);
            const std::string kv = tags.substr(q, semi == std::string::npos ? std::string::npos : semi - q);
            if (!kv.empty()) {
                const std::size_t eq = kv.find('=');
                if (eq == std::string::npos) out.tags[kv] = "";
                else out.tags[kv.substr(0, eq)] = unescapeTag(kv.substr(eq + 1));
            }
            if (semi == std::string::npos) break;
            q = semi + 1;
        }
    }
    if (p < s.size() && s[p] == ':') {
        ++p;
        out.prefix = word();
    }
    out.command = word();
    if (out.command.empty()) return false;
    while (p < s.size()) {
        if (s[p] == ':') {
            out.params.push_back(s.substr(p + 1));
            break;
        }
        out.params.push_back(word());
    }
    return true;
}

namespace {

std::string tag(const IrcLine& l, const char* k) {
    auto it = l.tags.find(k);
    return it == l.tags.end() ? std::string() : it->second;
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool parseHex(const std::string& h, Rgba& out) {
    if (h.size() != 7 || h[0] != '#') return false;
    unsigned v = 0;
    for (std::size_t i = 1; i < 7; ++i) {
        const char c = (char)std::tolower((unsigned char)h[i]);
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else return false;
    }
    out = {(std::uint8_t)(v >> 16), (std::uint8_t)(v >> 8), (std::uint8_t)v, 255};
    return true;
}

}  // namespace

Event interpret(const IrcLine& l, std::int64_t nowMs) {
    Event e;
    if (l.command == "PING") {
        e.type = Event::Ping;
        e.arg = l.params.empty() ? "tmi.twitch.tv" : l.params.back();
    } else if (l.command == "RECONNECT") {
        e.type = Event::Reconnect;
    } else if (l.command == "ROOMSTATE" || l.command == "366") {
        e.type = Event::Joined;
    } else if (l.command == "NOTICE") {
        e.type = Event::Notice;
        e.arg = l.params.empty() ? "" : l.params.back();
    } else if (l.command == "CLEARCHAT") {
        const std::string uid = tag(l, "target-user-id");
        if (!uid.empty()) {
            e.type = Event::ClearUser;
            e.arg = uid;
        } else if (l.params.size() >= 2) {   // older servers: the login as the trailing part
            e.type = Event::ClearUser;
            e.arg = "login:" + lower(l.params.back());
        } else {
            e.type = Event::ClearAll;
        }
    } else if (l.command == "CLEARMSG") {
        e.type = Event::Delete;
        e.arg = tag(l, "target-msg-id");
        if (e.arg.empty()) e.type = Event::None;
    } else if (l.command == "PRIVMSG" && l.params.size() >= 2) {
        Message m;
        m.id = tag(l, "id");
        m.userId = tag(l, "user-id");
        m.login = lower(l.prefix.substr(0, l.prefix.find('!')));
        m.name = tag(l, "display-name");
        if (m.name.empty()) m.name = m.login;
        if (!parseHex(tag(l, "color"), m.colour)) m.colour = {0, 0, 0, 0};
        m.text = l.params.back();
        // "/me": \x01ACTION text\x01
        if (m.text.size() >= 9 && m.text.rfind("\x01" "ACTION ", 0) == 0) {
            m.action = true;
            m.text = m.text.substr(8);
            if (!m.text.empty() && m.text.back() == '\x01') m.text.pop_back();
        }
        const std::string b = tag(l, "badges");
        std::size_t q = 0;
        while (q < b.size()) {
            const std::size_t c = b.find(',', q);
            const std::string one = b.substr(q, c == std::string::npos ? std::string::npos : c - q);
            const std::string nm = one.substr(0, one.find('/'));
            if (!nm.empty()) m.badges.push_back(nm);
            if (c == std::string::npos) break;
            q = c + 1;
        }
        m.atMs = nowMs;
        // Control characters out (they'd draw as boxes); new lines become spaces.
        std::string clean;
        for (char ch : m.text) clean += ((unsigned char)ch < 32) ? ' ' : ch;
        m.text = clean;
        e.type = Event::Chat;
        e.message = m;
    }
    return e;
}

std::string channelFrom(const std::string& typed) {
    std::string s = lower(typed);
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && (std::isspace((unsigned char)s.back()) || s.back() == '/')) s.pop_back();
    for (const char* pre : {"https://", "http://"})
        if (s.rfind(pre, 0) == 0) s = s.substr(std::string(pre).size());
    for (const char* pre : {"www.", "m."})
        if (s.rfind(pre, 0) == 0) s = s.substr(std::string(pre).size());
    if (s.rfind("twitch.tv/", 0) == 0) s = s.substr(10);
    if (!s.empty() && (s[0] == '@' || s[0] == '#')) s = s.substr(1);
    const std::size_t cut = s.find_first_of("/?#");
    if (cut != std::string::npos) s = s.substr(0, cut);
    if (s.empty() || s.size() > 25) return {};
    for (char c : s)
        if (!(std::isalnum((unsigned char)c) || c == '_')) return {};
    return s;
}

std::vector<std::string> loginLines(const std::string& channel, unsigned anonNumber) {
    return {"CAP REQ :twitch.tv/tags twitch.tv/commands", "PASS SCHMOOPIIE", "NICK justinfan" + std::to_string(10000 + anonNumber % 90000),
            "JOIN #" + channel};
}

Rgba defaultColour(const std::string& login) {
    static const Rgba kColours[] = {{0xFF, 0x00, 0x00, 255}, {0x00, 0x00, 0xFF, 255}, {0x00, 0x80, 0x00, 255}, {0xB2, 0x22, 0x22, 255},
                                    {0xFF, 0x7F, 0x50, 255}, {0x9A, 0xCD, 0x32, 255}, {0xFF, 0x45, 0x00, 255}, {0x2E, 0x8B, 0x57, 255},
                                    {0xDA, 0xA5, 0x20, 255}, {0xD2, 0x69, 0x1E, 255}, {0x5F, 0x9E, 0xA0, 255}, {0x1E, 0x90, 0xFF, 255},
                                    {0xFF, 0x69, 0xB4, 255}, {0x8A, 0x2B, 0xE2, 255}, {0x00, 0xFF, 0x7F, 255}};
    unsigned h = 0;
    for (char c : login) h = h * 31 + (unsigned char)c;
    return kColours[h % (sizeof kColours / sizeof kColours[0])];
}

bool Log::apply(const Event& e) {
    const std::size_t before = msgs_.size();
    switch (e.type) {
    case Event::Chat:
        msgs_.push_back(e.message);
        if (msgs_.size() > keep_) msgs_.erase(msgs_.begin(), msgs_.begin() + (std::ptrdiff_t)(msgs_.size() - keep_));
        ++version_;
        return true;
    case Event::ClearAll:
        if (msgs_.empty()) return false;
        msgs_.clear();
        break;
    case Event::ClearUser:
        if (e.arg.rfind("login:", 0) == 0) {
            const std::string login = e.arg.substr(6);
            std::erase_if(msgs_, [&](const Message& m) { return m.login == login; });
        } else {
            std::erase_if(msgs_, [&](const Message& m) { return m.userId == e.arg; });
        }
        break;
    case Event::Delete: std::erase_if(msgs_, [&](const Message& m) { return !m.id.empty() && m.id == e.arg; }); break;
    default: return false;
    }
    if (msgs_.size() == before) return false;
    ++version_;
    return true;
}

void Log::clear() {
    if (msgs_.empty()) return;
    msgs_.clear();
    ++version_;
}

std::vector<Message> recent(const std::vector<Message>& all, std::size_t count, int fadeSec, std::int64_t nowMs) {
    std::vector<Message> out;
    for (auto it = all.rbegin(); it != all.rend() && out.size() < count; ++it) {
        if (fadeSec > 0 && nowMs - it->atMs > (std::int64_t)fadeSec * 1000) break;   // older ones are older still
        out.push_back(*it);
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::vector<Message> samples(std::int64_t nowMs) {
    auto m = [&](const char* name, Rgba c, const char* text, std::vector<std::string> badges) {
        Message x;
        x.login = lower(name);
        x.name = name;
        x.colour = c;
        x.text = text;
        x.badges = std::move(badges);
        x.atMs = nowMs;
        return x;
    };
    return {m("SampleViewer", {0x3F, 0xCF, 0x8E, 255}, "hello! first time here", {}),
            m("SampleMod", {0x7A, 0x5C, 0xFF, 255}, "welcome in, grab a seat", {"moderator"}),
            m("SampleStreamer", {0xF4, 0xB9, 0x42, 255}, "thanks for being here, everyone!", {"broadcaster"}),
            m("SampleFriend", {0xFF, 0x69, 0xB4, 255}, "this overlay looks great", {"subscriber"}),
            m("SampleReader", {0x1E, 0x90, 0xFF, 255}, "can't wait for the next chapter", {})};
}

}  // namespace ncap::chat
