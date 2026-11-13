// SkillSwap data: creators and what they offer (lessons, workshops,
// tutorials and commissions). Free offerings are first-class: filters and
// sorting never push them below paid ones.
#pragma once

#include <string>
#include <vector>

namespace cj::skillswap {

enum class Kind { Lesson, Workshop, Tutorial, Commission };
enum class Pricing { Free, PayWhatYouCan, Fixed };

struct Category {
    std::string id;
    std::string name;
};

struct Creator {
    std::string id;
    std::string name;
    std::string handle;
    std::string bio;
    std::vector<std::string> skills;
    std::string availability;   // in the creator's own words
    std::string accent;         // #RRGGBB for their page
    bool demo = false;
};

struct Offering {
    std::string id;
    std::string creatorId;
    Kind kind = Kind::Lesson;
    std::string title;
    std::string description;
    std::string category;       // Category::id
    Pricing pricing = Pricing::Free;
    int pricePence = 0;         // fixed price, or suggested amount for pay-what-you-can
    int durationMinutes = 0;    // lessons and workshops
    int deliveryDays = 0;       // commissions
    std::string format;         // "One-to-one, online", "Group, online", "Recorded"
    std::string conditions;     // usage rights, revisions, etc.
    int slotsOpen = -1;         // commissions: -1 unknown
    bool demo = false;
};

struct Catalogue {
    std::vector<Category> categories;
    std::vector<Creator> creators;
    std::vector<Offering> offerings;
    std::vector<std::string> warnings;
    const Creator* creator(const std::string& id) const;
    const Category* category(const std::string& id) const;
};

Catalogue loadCatalogue(const std::string& categoriesJson, const std::string& listingsJson);

struct Filter {
    std::string category;       // empty = all
    int pricing = -1;           // -1 any, else Pricing
    int kind = -1;              // -1 any, else Kind
};
std::vector<const Offering*> filterOfferings(const Catalogue& c, const Filter& f);

const char* kindLabel(Kind k);
std::string priceLabel(const Offering& o);
std::string formatPounds(int pence);         // "£5", "£4.50"   // "Free", "Pay what you can (suggested £5)", "£12"

}  // namespace cj::skillswap
