// NorthRay Capture: which audio plugins the user has switched on, and the checks around them (the
// portable part; loading DLLs is src/audio/plugin_host). Nothing is enabled unless the user enables it,
// and an enabled plugin is tied to its file's SHA-256: a different file under the same name is off.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ncap::plugins {

// A plugin file found in the plugins folder.
struct Found {
    std::string file;      // "tremolo.dll" (no folders)
    std::string sha256;    // lower-case hex
    std::uint64_t size = 0;
};

// What the user decided about a file.
struct Record {
    std::string file;
    std::string sha256;    // the file they enabled
    bool enabled = false;
    std::string offReason; // why Capture switched it off ("" if the user did, or never enabled)
};

struct Registry {
    std::vector<Record> records;
    const Record* find(const std::string& file) const;
    Record* find(const std::string& file);
};

std::string toJson(const Registry& r);
Registry fromJson(const std::string& json);   // bad entries dropped

// A plain DLL file name: no folders, no device names, ends in .dll, at most 128 characters.
bool validFileName(const std::string& file);
std::string hex(const std::uint8_t* bytes, std::size_t n);

// Compares the registry with the files found now. An enabled plugin whose file is gone, or whose
// SHA-256 is different, is switched off with a reason; returns a message for each one switched off.
std::vector<std::string> reconcile(Registry& r, const std::vector<Found>& found);

// Enabling and disabling. enable() records the hash the user saw.
void enable(Registry& r, const Found& f);
void disable(Registry& r, const std::string& file, const std::string& reason = "");

// A plugin's description of itself (from its nr_audio_plugin()), copied out, and whether it's usable.
struct Param {
    std::string key, label, unit;
    float min = 0, max = 1, def = 0;
};
struct Info {
    std::uint32_t api = 0;
    std::string name, author, version;
    std::vector<Param> params;
    bool hasCreate = false, hasDestroy = false, hasSetParam = false, hasProcess = false;
};
bool validate(const Info& i, std::string& why);   // why: in plain words

// After a crash: the plugins that were running when Capture last stopped without closing properly
// (kept in a small file while any run). Each is switched off with a reason; returns the messages.
std::string runningToText(const std::vector<std::string>& files);
std::vector<std::string> runningFromText(const std::string& text);
std::vector<std::string> disableAfterCrash(Registry& r, const std::vector<std::string>& wereRunning);

}  // namespace ncap::plugins
