// NorthRay Capture: loading the audio-effect plugins the user has switched on (M9 part 3).
//
// Plugins live in %LOCALAPPDATA%\NorthRay\Capture\plugins as DLLs using sdk/northray_audio_plugin.h.
// Nothing there is loaded until the user switches it on after seeing its SHA-256 and signature; the
// choice is tied to that hash (core/plugins), so a changed file is off again. An enabled plugin is
// loaded from a copy named by its hash (plugin-cache), held open read-only while its hash is checked
// and the DLL is loaded, so the bytes that run are the bytes that were checked. Loaded modules stay
// loaded until Capture closes (instances may be running); switching one off stops new instances, and
// the chains using it are rebuilt without it.
#pragma once

#include "ncap/dsp.hpp"
#include "ncap/plugins.hpp"

#include <windows.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

struct NrAudioPlugin;

namespace ncap {

struct PluginEntry {
    std::string file;             // "tremolo.dll"
    std::wstring path;            // in the plugins folder
    std::string sha256;           // of the file now
    std::uint64_t size = 0;
    bool signedOk = false;        // a valid Authenticode signature
    std::wstring signer;          // who signed it ("" if unsigned)
    std::wstring signature;       // in plain words: "Signed by …" / "Not signed" / why it isn't valid
    bool enabled = false;
    std::string offReason;        // why Capture switched it off, if it did
    bool loaded = false;          // running in Capture now
    plugins::Info info;           // its own description, once loaded
    std::string problem;          // why it couldn't be loaded
};

class PluginHost {
public:
    static PluginHost& get();
    // Reads the choices, switches off plugins left running by a crash or changed since, and loads the
    // rest. Messages for the user about anything switched off come back from takeMessages().
    void init(const std::wstring& stateFolder);
    std::vector<std::string> takeMessages();
    void rescan();                // the folder again (files added or changed)
    const std::vector<PluginEntry>& entries() const { return entries_; }
    const PluginEntry* entry(const std::string& file) const;
    // A loaded plugin's values, for the filters page (null if it isn't loaded); stable while Capture runs.
    const std::vector<dsp::Param>* paramsOf(const std::string& file) const;
    std::wstring folder() const { return folder_; }
    // Switches on the file whose hash the user was shown (refused if it's different now); loads it.
    bool enable(const std::string& file, const std::string& shownSha256, std::wstring& error);
    void disable(const std::string& file);
    // Capture closing normally: no plugins were running when it stopped.
    void cleanExit();
    // For dsp::setExternalFactory.
    static std::unique_ptr<dsp::External> make(const std::string& file, const std::map<std::string, float>& values);

private:
    struct Loaded {
        HMODULE module = nullptr;
        const NrAudioPlugin* desc = nullptr;
        std::string sha256;
        plugins::Info info;
        std::vector<dsp::Param> params;   // pointers into info
    };
    bool load(PluginEntry& e, std::wstring& error);
    void save();
    void markRunning(const std::string& file);
    std::wstring folder_, cache_, registryPath_, runningPath_;
    plugins::Registry registry_;
    std::vector<PluginEntry> entries_;
    std::map<std::string, Loaded> loaded_;   // by file name; never unloaded while Capture runs
    std::vector<std::string> running_, messages_;
};

}  // namespace ncap
