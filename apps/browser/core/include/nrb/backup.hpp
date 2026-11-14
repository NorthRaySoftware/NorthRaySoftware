// NorthRay Browser: a backup file of what the person has made and chosen (the browser's "sync",
// kept local: no account, no server). Portable, so it's tested on every platform.
//
// A backup is a zip ("NorthRay Browser backup 2026-11-14.zip") holding:
//   manifest.json            what it is, when it was made, by which version, what's in it
//   settings.json            the browser's settings
//   User/bookmarks.db        bookmarks (a consistent copy, made with SQLite's VACUUM INTO)
//   User/history.db          history, only if the person chose to include it
//   User/references.db       reference boards, and their pictures in User/references/
//   User/start-notes.txt     the start page's notes, and its background picture
// Not in it: saved passwords and site sign-ins (they stay in the engine's own protected store on this
// PC), the downloads list (it points at files on this PC) and cached pages.
//
// Restoring is in two steps so nothing is replaced while it's open: `unpack` checks a backup and
// puts it in the Restore folder; the next start-up's `applyPending` moves what it replaces into a
// "Before restore ..." folder (so it can be got back) and moves the backup's files into place.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace nrb::backup {

struct Options {
    bool history = true;
    std::string appVersion;   // "0.8.0"
    std::string made;         // "2026-11-14 19:40"
};

struct Manifest {
    int version = 0;
    std::string app, kind, appVersion, made;
    bool history = false;
    int files = 0;
};

// A consistent copy of a SQLite database (safe while it's open elsewhere, WAL or not).
bool snapshotDatabase(const std::filesystem::path& from, const std::filesystem::path& to, std::string& error);
// Whether a file is a sound SQLite database with `table` in it.
bool checkDatabase(const std::filesystem::path& file, const std::string& table, std::string& error);

// Writes a backup of `stateDir` (the folder holding settings.json and User) to `out`.
bool make(const std::filesystem::path& stateDir, const std::filesystem::path& out, const Options& options, std::string& error,
          Manifest* made = nullptr);

// Reads and checks a backup and unpacks it into stateDir/Restore, ready for the next start-up.
bool unpack(const std::filesystem::path& zip, const std::filesystem::path& stateDir, Manifest& manifest, std::string& error);

// At start-up, before anything is opened: finishes a restore that's waiting. Returns false with
// `error` set if one was waiting and couldn't be finished (nothing is half-replaced: what was
// moved is put back). `keptIn` gets the folder the replaced files went to.
bool applyPending(const std::filesystem::path& stateDir, std::string& error, std::filesystem::path* keptIn = nullptr,
                  const std::string& stamp = "");
bool pending(const std::filesystem::path& stateDir);
// Gives up a restore that's waiting.
void cancelPending(const std::filesystem::path& stateDir);

// Whether a name inside a backup is one of the files a backup holds (never a path that climbs out).
bool allowedEntry(const std::string& name);

}  // namespace nrb::backup
