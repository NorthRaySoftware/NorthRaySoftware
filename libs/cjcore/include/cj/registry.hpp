// Application registry and community data.
//
// Every application CJ Community knows about is described by one JSON file in
// shared/metadata/apps/. The launcher reads that folder at start-up, and the
// website reads the same files at build time, so adding an app never needs a
// launcher code change and the two listings can't drift apart.
#pragma once

#include "cj/time.hpp"

#include <optional>
#include <string>
#include <vector>

namespace cj {

enum class AppStatus { InDevelopment, Preview, Available, Retired };

struct AppEntry {
    int schema = 1;
    std::string id;           // stable, lowercase, e.g. "cj-radio"
    std::string name;
    std::string tagline;
    std::string description;
    std::vector<std::string> features;
    std::string category;
    AppStatus status = AppStatus::InDevelopment;
    std::string version;      // empty = no released version
    std::string accent;       // "#RRGGBB"
    std::string artworkStyle; // fallback artwork generator: "broadcast", "sketchbook", ...
    std::string iconFile;     // relative to the app's metadata, optional
    std::string artworkFile;  // optional
    std::vector<std::string> screenshots;
    std::string developer;    // empty = not yet established
    std::string publisher;
    std::string associatedCreator;
    std::string executable;   // relative to the app's install folder
    bool canOpenInsideLauncher = false;
    std::string sourceFile;   // where this entry was loaded from
};

struct Announcement {
    std::string id;
    std::string title;
    std::string body;
    std::string author;
    UtcSeconds postedUtc = 0;
    bool demo = false;
};

struct CommunityEvent {
    std::string id;
    std::string title;
    std::string description;
    std::string sourceApp;    // app id that published it, or "community"
    UtcSeconds startUtc = 0;
    int durationMinutes = 60;
    bool cancelled = false;
    bool demo = false;
};

struct Profile {
    std::string displayName;
    std::string username;
    std::string bio;
    std::vector<std::string> interests;
    bool localDemo = true;    // false only for a real, signed-in online account
};

struct LoadResult {
    std::vector<std::string> warnings;
};

const char* appStatusLabel(AppStatus s);
std::optional<AppEntry> parseAppEntry(const std::string& jsonText, std::string& error);

// Validates an app id: lowercase letters, digits and single hyphens.
bool isValidAppId(const std::string& id);

std::vector<Announcement> parseAnnouncements(const std::string& jsonText, UtcSeconds now, LoadResult& r);
std::vector<CommunityEvent> parseEvents(const std::string& jsonText, UtcSeconds now, LoadResult& r);
std::optional<Profile> parseProfile(const std::string& jsonText, LoadResult& r);

}  // namespace cj
