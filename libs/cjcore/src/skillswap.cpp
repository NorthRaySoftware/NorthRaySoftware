#include "cj/skillswap.hpp"

#include <algorithm>
#include <cstdio>
#include <nlohmann/json.hpp>

namespace cj::skillswap {

using nlohmann::json;

static std::string str(const json& j, const char* k) {
    auto it = j.find(k);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}

const Creator* Catalogue::creator(const std::string& id) const {
    for (auto& c : creators) if (c.id == id) return &c;
    return nullptr;
}

const Category* Catalogue::category(const std::string& id) const {
    for (auto& c : categories) if (c.id == id) return &c;
    return nullptr;
}

const char* kindLabel(Kind k) {
    switch (k) {
        case Kind::Lesson: return "Lesson";
        case Kind::Workshop: return "Group workshop";
        case Kind::Tutorial: return "Recorded tutorial";
        case Kind::Commission: return "Commission";
    }
    return "";
}

std::string formatPounds(int pence) {
    char buf[32];
    if (pence % 100 == 0) std::snprintf(buf, sizeof buf, "\xC2\xA3%d", pence / 100);
    else std::snprintf(buf, sizeof buf, "\xC2\xA3%d.%02d", pence / 100, pence % 100);
    return buf;
}

std::string priceLabel(const Offering& o) {
    switch (o.pricing) {
        case Pricing::Free: return "Free";
        case Pricing::PayWhatYouCan:
            return o.pricePence > 0 ? "Pay what you can (suggested " + formatPounds(o.pricePence) + ")" : "Pay what you can";
        case Pricing::Fixed: return formatPounds(o.pricePence);
    }
    return "";
}

Catalogue loadCatalogue(const std::string& categoriesJson, const std::string& listingsJson) {
    Catalogue c;
    json cats = json::parse(categoriesJson, nullptr, false);
    if (cats.is_object())
        for (const json& j : cats.value("categories", json::array())) c.categories.push_back({str(j, "id"), str(j, "name")});
    else c.warnings.push_back("SkillSwap categories file isn't valid JSON.");

    json doc = json::parse(listingsJson, nullptr, false);
    if (!doc.is_object()) { c.warnings.push_back("SkillSwap listings file isn't valid JSON."); return c; }
    const bool demo = doc.value("demo", false);
    for (const json& j : doc.value("creators", json::array())) {
        Creator cr;
        cr.id = str(j, "id");
        cr.name = str(j, "name");
        cr.handle = str(j, "handle");
        cr.bio = str(j, "bio");
        cr.availability = str(j, "availability");
        cr.accent = str(j, "accent");
        for (const json& s : j.value("skills", json::array())) if (s.is_string()) cr.skills.push_back(s.get<std::string>());
        cr.demo = demo;
        if (cr.id.empty() || cr.name.empty()) { c.warnings.push_back("A creator is missing an id or name."); continue; }
        c.creators.push_back(cr);
    }
    for (const json& j : doc.value("offerings", json::array())) {
        Offering o;
        o.id = str(j, "id");
        o.creatorId = str(j, "creator");
        o.title = str(j, "title");
        o.description = str(j, "description");
        o.category = str(j, "category");
        const std::string k = str(j, "kind");
        o.kind = k == "workshop" ? Kind::Workshop : k == "tutorial" ? Kind::Tutorial : k == "commission" ? Kind::Commission : Kind::Lesson;
        const std::string p = str(j, "pricing");
        o.pricing = p == "fixed" ? Pricing::Fixed : p == "pay-what-you-can" ? Pricing::PayWhatYouCan : Pricing::Free;
        o.pricePence = std::max(0, j.value("pricePence", 0));
        o.durationMinutes = j.value("durationMinutes", 0);
        o.deliveryDays = j.value("deliveryDays", 0);
        o.format = str(j, "format");
        o.conditions = str(j, "conditions");
        o.slotsOpen = j.value("slotsOpen", -1);
        o.demo = demo;
        if (o.pricing == Pricing::Fixed && o.pricePence == 0) {
            c.warnings.push_back("Offering '" + o.id + "' is fixed-price with no price; listed as free.");
            o.pricing = Pricing::Free;
        }
        if (!c.creator(o.creatorId)) { c.warnings.push_back("Offering '" + o.id + "' has an unknown creator."); continue; }
        c.offerings.push_back(o);
    }
    return c;
}

std::vector<const Offering*> filterOfferings(const Catalogue& c, const Filter& f) {
    std::vector<const Offering*> out;
    for (const auto& o : c.offerings) {
        if (!f.category.empty() && o.category != f.category) continue;
        if (f.pricing >= 0 && (int)o.pricing != f.pricing) continue;
        if (f.kind >= 0 && (int)o.kind != f.kind) continue;
        out.push_back(&o);
    }
    // Keep the creators' own order. Price never decides position, so free
    // offerings are as visible as paid ones.
    return out;
}

}  // namespace cj::skillswap
