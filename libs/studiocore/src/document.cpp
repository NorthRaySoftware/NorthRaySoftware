#include "studio/document.hpp"
#include "studio/artwork.hpp"

#include <cmath>

#include <nlohmann/json.hpp>

#include <set>

namespace studio::doc {

using nlohmann::json;

const std::vector<StyleInfo> kParagraphStyles = {
    {"body", "Body"},         {"first", "First paragraph (no indent)"}, {"centered", "Centred"},
    {"epigraph", "Epigraph"}, {"attribution", "Attribution"},           {"verse", "Verse"},
    {"letter", "Letter"},     {"note", "Note"},
};

namespace {

bool knownStyle(const std::string& s) {
    for (auto& st : kParagraphStyles) if (s == st.id) return true;
    return false;
}

const std::set<std::string> kMarks = {"em", "strong", "underline", "smallcaps", "strike", "sup", "sub", "code", "link"};

// UTF-8 decode one code point; advances i. Invalid bytes become U+FFFD.
char32_t nextCp(const std::string& s, size_t& i) {
    const unsigned char c = (unsigned char)s[i];
    auto cont = [&](size_t k) { return k < s.size() && ((unsigned char)s[k] & 0xC0) == 0x80; };
    if (c < 0x80) { ++i; return c; }
    if ((c >> 5) == 6 && cont(i + 1)) { char32_t v = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); i += 2; return v; }
    if ((c >> 4) == 14 && cont(i + 1) && cont(i + 2)) {
        char32_t v = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); i += 3; return v;
    }
    if ((c >> 3) == 30 && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
        char32_t v = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
        i += 4; return v;
    }
    ++i;
    return 0xFFFD;
}

bool isWordChar(char32_t c) {
    if (c < 0x80) return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    if (c >= 0x2000 && c <= 0x206F) return false;  // general punctuation: dashes, quotes, ellipsis
    if (c >= 0x00A0 && c <= 0x00BF) return false;  // nbsp, symbols
    if (c >= 0x3000 && c <= 0x303F) return false;  // CJK punctuation
    if (c == 0x00D7 || c == 0x00F7 || c == 0xFFFD || c == 0xFEFF) return false;
    return true;
}
bool isJoiner(char32_t c) { return c == '\'' || c == 0x2019 || c == '-' || c == 0x2010 || c == 0x2011; }

void collectText(const json& n, std::string& out) {
    if (!n.is_object()) return;
    const std::string t = n.value("type", "");
    if (t == "text") { out += n.value("text", ""); return; }
    if (t == "hard_break") { out += '\n'; return; }
    if (n.contains("content") && n["content"].is_array())
        for (auto& c : n["content"]) collectText(c, out);
}

void blocksText(const json& n, std::string& out) {
    const std::string t = n.value("type", "");
    if (t == "paragraph" || t == "heading") {
        std::string line;
        collectText(n, line);
        if (!out.empty()) out += "\n\n";
        out += line;
    } else if (t == "scene_break") {
        if (!out.empty()) out += "\n\n";
        out += "* * *";
    } else if (t == "image") {
        const std::string cap = n.contains("attrs") ? n["attrs"].value("caption", "") : "";
        if (!cap.empty()) { if (!out.empty()) out += "\n\n"; out += cap; }
    } else if (n.contains("content") && n["content"].is_array()) {
        for (auto& c : n["content"]) blocksText(c, out);
    }
}

// --- sanitising

json cleanInline(const json& n, std::vector<std::string>* problems) {
    // Returns an array of valid inline nodes (text, hard_break).
    json out = json::array();
    if (!n.is_object()) return out;
    const std::string t = n.value("type", "");
    if (t == "text") {
        std::string text = n.value("text", "");
        if (text.empty()) return out;
        json node = {{"type", "text"}, {"text", text}};
        if (n.contains("marks") && n["marks"].is_array()) {
            json marks = json::array();
            std::set<std::string> seen;
            for (auto& m : n["marks"]) {
                const std::string mt = m.is_object() ? m.value("type", "") : "";
                if (mt == "comment") {
                    // A comment's place in the text (several may overlap): its id, 8 to 32 hex digits.
                    const std::string cid = m.contains("attrs") && m["attrs"].is_object() ? m["attrs"].value("id", "") : "";
                    if (cid.size() >= 8 && cid.size() <= 32 && cid.find_first_not_of("0123456789abcdef") == std::string::npos &&
                        !seen.count("comment:" + cid)) {
                        seen.insert("comment:" + cid);
                        marks.push_back({{"type", "comment"}, {"attrs", {{"id", cid}}}});
                    }
                    continue;
                }
                if (!kMarks.count(mt) || seen.count(mt)) continue;
                seen.insert(mt);
                if (mt == "link") {
                    std::string href = m.contains("attrs") && m["attrs"].is_object() ? m["attrs"].value("href", "") : "";
                    if (href.rfind("https://", 0) != 0 && href.rfind("http://", 0) != 0 && href.rfind("mailto:", 0) != 0) continue;
                    marks.push_back({{"type", "link"}, {"attrs", {{"href", href}}}});
                } else {
                    marks.push_back({{"type", mt}});
                }
            }
            if (!marks.empty()) node["marks"] = marks;
        }
        out.push_back(node);
        return out;
    }
    if (t == "hard_break") { out.push_back({{"type", "hard_break"}}); return out; }
    // Anything else inside a paragraph: keep its text.
    if (problems && !t.empty()) problems->push_back("Unsupported inline element '" + t + "' kept as text.");
    if (n.contains("content") && n["content"].is_array())
        for (auto& c : n["content"])
            for (auto& x : cleanInline(c, problems)) out.push_back(x);
    return out;
}

json inlineContent(const json& n, std::vector<std::string>* problems) {
    json out = json::array();
    if (n.contains("content") && n["content"].is_array())
        for (auto& c : n["content"])
            for (auto& x : cleanInline(c, problems)) out.push_back(x);
    return out;
}

void cleanBlocks(const json& arr, json& out, std::vector<std::string>* problems, int depth);

json cleanBlock(const json& n, std::vector<std::string>* problems, int depth, bool& ok) {
    ok = true;
    const std::string t = n.is_object() ? n.value("type", "") : "";
    const json attrs = n.is_object() && n.contains("attrs") && n["attrs"].is_object() ? n["attrs"] : json::object();
    if (t == "paragraph") {
        std::string style = attrs.value("style", "body");
        if (!knownStyle(style)) style = "body";
        json p = {{"type", "paragraph"}, {"attrs", {{"style", style}}}};
        json c = inlineContent(n, problems);
        if (!c.empty()) p["content"] = c;
        return p;
    }
    if (t == "heading") {
        int level = attrs.value("level", 2);
        if (level < 1) level = 1;
        if (level > 3) level = 3;
        json h = {{"type", "heading"}, {"attrs", {{"level", level}}}};
        json c = inlineContent(n, problems);
        if (!c.empty()) h["content"] = c;
        return h;
    }
    if (t == "scene_break") return {{"type", "scene_break"}};
    if (t == "image") {
        const std::string blob = attrs.value("blob", "");
        if (blob.size() != 64 || blob.find_first_not_of("0123456789abcdef") != std::string::npos) {
            if (problems) problems->push_back("An image without stored artwork was removed.");
            ok = false;
            return {};
        }
        json out = {{"blob", blob}, {"alt", attrs.value("alt", "")}, {"caption", attrs.value("caption", "")}};
        // Placement (crop, width, alignment): kept only when it differs from the full picture at full width.
        auto num = [&](const char* k, double def) {
            const auto it = attrs.find(k);
            return it != attrs.end() && it->is_number() ? it->get<double>() : def;
        };
        Placement p;
        p.cropX = num("cropX", 0); p.cropY = num("cropY", 0); p.cropW = num("cropW", 1); p.cropH = num("cropH", 1);
        p.width = num("width", 1);
        p.align = attrs.value("align", "center");
        p = clampPlacement(p);
        auto round4 = [](double v) { return std::round(v * 10000) / 10000; };
        if (p.cropX > 0 || p.cropY > 0 || p.cropW < 1 || p.cropH < 1) {
            out["cropX"] = round4(p.cropX); out["cropY"] = round4(p.cropY); out["cropW"] = round4(p.cropW); out["cropH"] = round4(p.cropH);
        }
        if (p.width < 1) out["width"] = round4(p.width);
        if (p.align != "center") out["align"] = p.align;
        return {{"type", "image"}, {"attrs", out}};
    }
    if ((t == "blockquote" || t == "list_item") && depth < 8) {
        json c = json::array();
        if (n.contains("content")) cleanBlocks(n["content"], c, problems, depth + 1);
        if (c.empty()) c.push_back({{"type", "paragraph"}, {"attrs", {{"style", "body"}}}});
        return {{"type", t}, {"content", c}};
    }
    if ((t == "bullet_list" || t == "ordered_list") && depth < 8) {
        json items = json::array();
        if (n.contains("content") && n["content"].is_array())
            for (auto& it : n["content"]) {
                bool iok;
                json li = cleanBlock(it.is_object() && it.value("type", "") == "list_item" ? it
                                         : json{{"type", "list_item"}, {"content", json::array({it})}},
                                     problems, depth + 1, iok);
                if (iok) items.push_back(li);
            }
        if (items.empty()) { ok = false; return {}; }
        json l = {{"type", t}, {"content", items}};
        if (t == "ordered_list") l["attrs"] = {{"start", std::max(1, attrs.value("start", 1))}};
        return l;
    }
    // Unknown block: keep its text as a paragraph.
    if (problems) problems->push_back(t.empty() ? "An unreadable block was removed." : "Unsupported block '" + t + "' kept as text.");
    json c = inlineContent(n, problems);
    if (c.empty()) { ok = false; return {}; }
    return {{"type", "paragraph"}, {"attrs", {{"style", "body"}}}, {"content", c}};
}

void cleanBlocks(const json& arr, json& out, std::vector<std::string>* problems, int depth) {
    if (!arr.is_array()) return;
    for (auto& b : arr) {
        bool ok;
        json c = cleanBlock(b, problems, depth, ok);
        if (ok) out.push_back(c);
    }
}


}  // namespace

std::string emptyDoc() {
    return R"({"type":"doc","content":[{"type":"paragraph","attrs":{"style":"body"}}]})";
}

std::string sanitise(const std::string& text, std::vector<std::string>* problems) {
    if (text.empty()) return emptyDoc();
    json d = json::parse(text, nullptr, false);
    if (d.is_discarded() || !d.is_object() || d.value("type", "") != "doc") {
        if (problems) problems->push_back("The text wasn't a valid document; it was replaced with an empty one.");
        return emptyDoc();
    }
    json content = json::array();
    if (d.contains("content")) cleanBlocks(d["content"], content, problems, 0);
    if (content.empty()) content.push_back({{"type", "paragraph"}, {"attrs", {{"style", "body"}}}});
    return json{{"type", "doc"}, {"content", content}}.dump();
}

std::int64_t countWords(const std::string& text) {
    std::int64_t n = 0;
    bool inWord = false;
    size_t i = 0;
    while (i < text.size()) {
        const char32_t c = nextCp(text, i);
        if (isWordChar(c)) {
            if (!inWord) ++n;
            inWord = true;
        } else if (inWord && isJoiner(c) && i < text.size()) {
            size_t j = i;
            if (!isWordChar(nextCp(text, j))) inWord = false;  // "word- " ends the word
        } else {
            inWord = false;
        }
    }
    return n;
}

std::string plainText(const std::string& docJson) {
    json d = json::parse(docJson, nullptr, false);
    std::string out;
    if (d.is_object()) blocksText(d, out);
    return out;
}

std::int64_t wordCount(const std::string& docJson) { return countWords(plainText(docJson)); }

// ------------------------------------------------------------------ Builder

struct Builder::Impl {
    json root = json::array();             // top-level blocks
    std::vector<json> containers;          // open blockquote / list / list_item nodes
    json block;                            // open paragraph or heading (null when none)
    std::string blockText;                 // its text, for heading titles
    std::string firstHeading;
    std::int64_t words = 0;
    bool anything = false;

    json& target() { return containers.empty() ? root : containers.back()["content"]; }
    void closeBlock() {
        if (block.is_null()) return;
        const bool isHeading = block["type"] == "heading";
        const bool hasContent = block.contains("content") && !block["content"].empty();
        if (hasContent) {
            if (isHeading && firstHeading.empty()) firstHeading = blockText;
            words += countWords(blockText);
            anything = true;
            // Inside a list, a paragraph needs a list item.
            if (!containers.empty() && (containers.back()["type"] == "bullet_list" || containers.back()["type"] == "ordered_list"))
                containers.back()["content"].push_back({{"type", "list_item"}, {"content", json::array({block})}});
            else
                target().push_back(block);
        }
        block = nullptr;
        blockText.clear();
    }
    void open(json b) { closeBlock(); block = std::move(b); }
    void closeContainer(const char* t1, const char* t2) {
        closeBlock();
        if (containers.empty()) return;
        const std::string t = containers.back()["type"];
        if (t != t1 && t != t2) return;
        json c = containers.back();
        containers.pop_back();
        if (!c["content"].empty()) target().push_back(c);
    }
};

Builder::Builder() : d_(new Impl) {}
Builder::~Builder() { delete d_; }
Builder::Builder(Builder&& o) noexcept : d_(o.d_) { o.d_ = nullptr; }
Builder& Builder::operator=(Builder&& o) noexcept { std::swap(d_, o.d_); return *this; }

void Builder::paragraph(const std::string& style) {
    d_->open({{"type", "paragraph"}, {"attrs", {{"style", knownStyle(style) ? style : "body"}}}});
}
void Builder::heading(int level) {
    d_->open({{"type", "heading"}, {"attrs", {{"level", level < 1 ? 1 : level > 3 ? 3 : level}}}});
}
void Builder::text(const std::string& utf8, unsigned marks, const std::string& href) {
    if (utf8.empty()) return;
    if (d_->block.is_null()) paragraph();
    json t = {{"type", "text"}, {"text", utf8}};
    json m = json::array();
    static const std::pair<unsigned, const char*> names[] = {{Em, "em"}, {Strong, "strong"}, {Underline, "underline"},
                                                            {SmallCaps, "smallcaps"}, {Strike, "strike"}, {Sup, "sup"},
                                                            {Sub, "sub"}, {Code, "code"}};
    for (auto& [bit, name] : names) if (marks & bit) m.push_back({{"type", name}});
    if (!href.empty() && (href.rfind("https://", 0) == 0 || href.rfind("http://", 0) == 0 || href.rfind("mailto:", 0) == 0))
        m.push_back({{"type", "link"}, {"attrs", {{"href", href}}}});
    if (!m.empty()) t["marks"] = m;
    json& c = d_->block["content"];
    if (!c.is_array()) c = json::array();
    // Merge with the previous run when the marks match.
    if (!c.empty() && c.back()["type"] == "text" && c.back().value("marks", json::array()) == t.value("marks", json::array()))
        c.back()["text"] = c.back()["text"].get<std::string>() + utf8;
    else
        c.push_back(t);
    d_->blockText += utf8;
}
void Builder::hardBreak() {
    if (d_->block.is_null()) return;
    json& c = d_->block["content"];
    if (!c.is_array()) c = json::array();
    c.push_back({{"type", "hard_break"}});
    d_->blockText += ' ';
}
void Builder::sceneBreak() {
    d_->closeBlock();
    // Never two in a row, and never as the first block.
    json& t = d_->target();
    if (t.empty() || t.back()["type"] == "scene_break") return;
    t.push_back({{"type", "scene_break"}});
}
void Builder::image(const std::string& blob, const std::string& alt, const std::string& caption) {
    d_->closeBlock();
    if (blob.size() != 64) return;
    d_->target().push_back({{"type", "image"}, {"attrs", {{"blob", blob}, {"alt", alt}, {"caption", caption}}}});
    d_->anything = true;
}
void Builder::beginQuote() {
    d_->closeBlock();
    d_->containers.push_back({{"type", "blockquote"}, {"content", json::array()}});
}
void Builder::beginList(bool ordered) {
    d_->closeBlock();
    json l = {{"type", ordered ? "ordered_list" : "bullet_list"}, {"content", json::array()}};
    if (ordered) l["attrs"] = {{"start", 1}};
    d_->containers.push_back(l);
}
void Builder::listItem() { paragraph(); }
void Builder::endQuote() { d_->closeContainer("blockquote", "blockquote"); }
void Builder::endList() { d_->closeContainer("bullet_list", "ordered_list"); }
void Builder::endBlock() { d_->closeBlock(); }

bool Builder::empty() const { return !d_->anything && (d_->block.is_null() || d_->blockText.empty()); }
std::int64_t Builder::words() const { return d_->words + countWords(d_->blockText); }
std::string Builder::firstHeadingText() const { return d_->firstHeading; }

std::string Builder::toJson() const {
    // Close everything on a copy so the builder can keep going.
    Impl copy = *d_;
    copy.closeBlock();
    while (!copy.containers.empty()) {
        nlohmann::json c = copy.containers.back();
        copy.containers.pop_back();
        if (!c["content"].empty()) copy.target().push_back(c);
    }
    // Trailing scene breaks mean nothing.
    while (!copy.root.empty() && copy.root.back()["type"] == "scene_break") copy.root.erase(copy.root.size() - 1);
    if (copy.root.empty()) return emptyDoc();
    return nlohmann::json{{"type", "doc"}, {"content", copy.root}}.dump();
}

}  // namespace studio::doc
