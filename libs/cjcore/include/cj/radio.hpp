// CJ Radio schedule and broadcast status.
//
// The ground rules this module enforces:
//   * Saturday is the usual broadcast day, but a Saturday never gets a
//     programme just because it's Saturday. Only programmes the host has
//     confirmed are shown to listeners.
//   * "Live" comes only from the broadcasting infrastructure (a LiveSignal),
//     never from the calendar. If no infrastructure is connected, the station
//     can't be live.
//   * A Saturday with nothing on it is a normal state, not an error.
#pragma once

#include "cj/time.hpp"

#include <optional>
#include <string>
#include <vector>

namespace cj::radio {

enum class Kind { RegularSaturday, Additional, Spontaneous };

enum class Status {
    Draft,       // host-side only, never shown to listeners
    Confirmed,   // announced and expected to go ahead
    Cancelled,
    Postponed,   // moved; `movedTo` may point at the replacement programme
};

struct Programme {
    std::string id;
    std::string title;
    std::string description;
    std::string presenter;
    std::string category;
    Kind kind = Kind::Additional;
    Status status = Status::Draft;
    UtcSeconds startUtc = 0;
    int durationMinutes = 60;
    std::string authoredUkTime;  // as the host typed it, e.g. "2026-11-14T19:00"
    std::string seriesId;        // optional
    std::string movedTo;         // for postponed programmes
    std::string note;            // e.g. reason for cancellation
    bool demo = false;

    UtcSeconds endUtc() const { return startUtc + static_cast<UtcSeconds>(durationMinutes) * 60; }
    bool isPublic() const { return status != Status::Draft; }
};

// A host-side template such as "The CJ Philip Show, Saturdays around 19:00".
// Series only generate *draft* suggestions for the host's planning view. They
// never publish anything on their own.
struct Series {
    std::string id;
    std::string title;
    std::string presenter;
    std::string category;
    int defaultUkHour = 19;
    int defaultUkMinute = 0;
    int defaultDurationMinutes = 120;
};

// Reported by the streaming infrastructure. In the prototype there is none,
// so `connected` is false and the station is always reported off air.
struct LiveSignal {
    bool connected = false;
    bool onAir = false;
    std::string programmeId;   // empty for a spontaneous broadcast with no listing
    std::string title;         // what the host called it when going live
    UtcSeconds startedUtc = 0;
    int listeners = -1;        // -1 = unknown
};

enum class StationState { Live, StartingSoon, Upcoming, OffAir };

struct StationStatus {
    StationState state = StationState::OffAir;
    std::optional<Programme> current;   // set only when Live and the signal matches a listing
    std::optional<Programme> next;      // next confirmed programme, if any
    // A confirmed programme whose start time has passed but which isn't live.
    // We say so plainly rather than pretending it's on.
    std::optional<Programme> overdue;
    std::vector<Programme> recentChanges;  // cancellations/postponements still relevant
    bool infrastructureConnected = false;
};

struct Schedule {
    std::vector<Programme> programmes;
    std::vector<Series> series;
    std::vector<std::string> warnings;  // validation problems found while loading
};

// Minutes before a confirmed start that we switch to "Starting soon".
constexpr int kStartingSoonMinutes = 30;

StationStatus computeStatus(const Schedule& schedule, const LiveSignal& signal, UtcSeconds now);

// Confirmed + changed programmes, sorted by start, within [from, to).
std::vector<Programme> publicProgrammes(const Schedule& schedule, UtcSeconds from, UtcSeconds to);

// Host planning aid: the Saturdays in [from, to) that have no public programme,
// each with a draft suggestion from the given series. Nothing here is shown
// to listeners.
std::vector<Programme> draftSaturdaySuggestions(const Schedule& schedule, const Series& series,
                                                UtcSeconds from, UtcSeconds to);

// Loads a schedule document (see shared/metadata/radio/README.md).
// `now` is used to resolve relative dates in demonstration files.
Schedule loadSchedule(const std::string& jsonText, UtcSeconds now);

const char* kindLabel(Kind k);
const char* statusLabel(Status s);
const char* stateLabel(StationState s);

}  // namespace cj::radio
