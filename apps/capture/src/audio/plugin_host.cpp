#include "audio/plugin_host.hpp"

#include "log.hpp"
#include "ncap/oauth.hpp"

#include "../../sdk/northray_audio_plugin.h"

#include <softpub.h>
#include <wintrust.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ncap {

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string readText(const std::wstring& path) {
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool writeText(const std::wstring& path, const std::string& text) {
    const std::wstring tmp = path + L".new";
    {
        std::ofstream f(std::filesystem::path(tmp), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << text;
        if (!f) return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

constexpr std::uint64_t kMaxBytes = 64ull << 20;

// The SHA-256 of what an open handle reads, from the start.
bool hashHandle(HANDLE h, std::string& out, std::uint64_t& size) {
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || (std::uint64_t)sz.QuadPart > kMaxBytes) return false;
    size = (std::uint64_t)sz.QuadPart;
    std::vector<std::uint8_t> data((size_t)size);
    LARGE_INTEGER zero{};
    SetFilePointerEx(h, zero, nullptr, FILE_BEGIN);
    DWORD got = 0;
    std::size_t at = 0;
    while (at < data.size()) {
        if (!ReadFile(h, data.data() + at, (DWORD)std::min<std::size_t>(data.size() - at, 1 << 20), &got, nullptr) || got == 0) return false;
        at += got;
    }
    const auto d = oauth::sha256(data.data(), data.size());
    out = plugins::hex(d.data(), d.size());
    return true;
}

bool hashFile(const std::wstring& path, std::string& out, std::uint64_t& size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const bool ok = hashHandle(h, out, size);
    CloseHandle(h);
    return ok;
}

// Authenticode: whether the file carries a valid signature, and whose. No network: revocation isn't
// checked online (no telemetry), so a revoked certificate shows as signed; the words say so.
void checkSignature(const std::wstring& path, bool& ok, std::wstring& signer, std::wstring& words) {
    ok = false;
    signer.clear();
    WINTRUST_FILE_INFO fi{};
    fi.cbStruct = sizeof fi;
    fi.pcwszFilePath = path.c_str();
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd{};
    wd.cbStruct = sizeof wd;
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    const LONG r = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    if (r == ERROR_SUCCESS) {
        ok = true;
        if (CRYPT_PROVIDER_DATA* pd = WTHelperProvDataFromStateData(wd.hWVTStateData))
            if (CRYPT_PROVIDER_SGNR* sg = WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0))
                if (sg->csCertChain > 0 && sg->pasCertChain && sg->pasCertChain[0].pCert) {
                    wchar_t name[256] = {};
                    CertGetNameStringW(sg->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 256);
                    signer = name;
                }
        words = L"Signed by " + (signer.empty() ? std::wstring(L"an unnamed publisher") : signer) + L" (signature valid; revocation not checked online)";
    } else if (r == TRUST_E_NOSIGNATURE || r == TRUST_E_SUBJECT_FORM_UNKNOWN || r == TRUST_E_PROVIDER_UNKNOWN) {
        words = L"Not signed: nobody vouches for who made it";
    } else if (r == CERT_E_UNTRUSTEDROOT || r == CERT_E_CHAINING) {
        words = L"Signed, but by a certificate Windows doesn't trust";
    } else if (r == CERT_E_EXPIRED) {
        words = L"Signed, but the certificate had expired";
    } else if (r == TRUST_E_BAD_DIGEST) {
        words = L"Its signature doesn't match the file: it was changed after signing";
    } else {
        wchar_t b[64];
        swprintf(b, 64, L"The signature isn't valid (0x%08lX)", (unsigned long)r);
        words = b;
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
}

// One running instance of a plugin, as a filter.
class Instance : public dsp::External {
public:
    Instance(const NrAudioPlugin* d, void* inst) : d_(d), inst_(inst) {}
    ~Instance() override { d_->destroy(inst_); }
    void process(float* frames, std::size_t count) override { d_->process(inst_, frames, (std::uint32_t)count); }
    int latency() const override { return d_->latency ? (int)std::min<std::uint32_t>(d_->latency(inst_), 96000) : 0; }

private:
    const NrAudioPlugin* d_;
    void* inst_;
};

}  // namespace

PluginHost& PluginHost::get() {
    static PluginHost h;
    return h;
}

void PluginHost::init(const std::wstring& state) {
    folder_ = state + L"\\plugins";
    cache_ = state + L"\\plugin-cache";
    registryPath_ = state + L"\\plugins.json";
    runningPath_ = state + L"\\plugins-running.txt";
    SHCreateDirectoryExW(nullptr, folder_.c_str(), nullptr);
    registry_ = plugins::fromJson(readText(registryPath_));
    // Left running by a session that didn't close properly: off, in case one of them was why.
    const auto were = plugins::runningFromText(readText(runningPath_));
    for (auto& m : plugins::disableAfterCrash(registry_, were)) messages_.push_back(m);
    DeleteFileW(runningPath_.c_str());
    rescan();
    dsp::setExternalFactory(&PluginHost::make);
}

std::vector<std::string> PluginHost::takeMessages() {
    std::vector<std::string> m;
    m.swap(messages_);
    return m;
}

void PluginHost::rescan() {
    std::vector<PluginEntry> found;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((folder_ + L"\\*.dll").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            PluginEntry e;
            e.file = narrow(fd.cFileName);
            if (!plugins::validFileName(e.file)) continue;
            e.path = folder_ + L"\\" + fd.cFileName;
            if (!hashFile(e.path, e.sha256, e.size)) {
                e.problem = "It couldn't be read (or it's over 64 MB).";
            }
            checkSignature(e.path, e.signedOk, e.signer, e.signature);
            found.push_back(e);
        } while (FindNextFileW(h, &fd) && found.size() < 64);
        FindClose(h);
    }
    std::sort(found.begin(), found.end(), [](const PluginEntry& a, const PluginEntry& b) { return a.file < b.file; });
    std::vector<plugins::Found> f;
    for (auto& e : found)
        if (!e.sha256.empty()) f.push_back({e.file, e.sha256, e.size});
    for (auto& m : plugins::reconcile(registry_, f)) messages_.push_back(m);
    for (auto& e : found) {
        if (const plugins::Record* r = registry_.find(e.file)) {
            e.enabled = r->enabled;
            e.offReason = r->offReason;
        }
        if (auto it = loaded_.find(e.file); it != loaded_.end() && it->second.sha256 == e.sha256) {
            e.loaded = e.enabled;
            e.info = it->second.info;
        }
    }
    entries_ = std::move(found);
    // Load what's switched on and not running yet.
    for (auto& e : entries_)
        if (e.enabled && !e.loaded) {
            std::wstring err;
            if (!load(e, err)) {
                e.problem = narrow(err);
                messages_.push_back(e.file + ": " + e.problem);
            }
        }
    save();
}

const PluginEntry* PluginHost::entry(const std::string& file) const {
    for (auto& e : entries_)
        if (e.file == file) return &e;
    return nullptr;
}

const std::vector<dsp::Param>* PluginHost::paramsOf(const std::string& file) const {
    auto it = loaded_.find(file);
    const PluginEntry* e = entry(file);
    if (it == loaded_.end() || !e || !e->loaded) return nullptr;
    return &it->second.params;
}

bool PluginHost::load(PluginEntry& e, std::wstring& error) {
    if (auto it = loaded_.find(e.file); it != loaded_.end()) {
        if (it->second.sha256 == e.sha256) {
            e.loaded = true;
            e.info = it->second.info;
            return true;
        }
        error = L"A different version of it is already loaded; restart Capture to use this one.";
        return false;
    }
    // A copy named by its hash, held open (read-only, no writing or deleting by others) while it's
    // checked and loaded.
    SHCreateDirectoryExW(nullptr, cache_.c_str(), nullptr);
    const std::wstring copy = cache_ + L"\\" + widen(e.sha256) + L".dll";
    if (GetFileAttributesW(copy.c_str()) == INVALID_FILE_ATTRIBUTES && !CopyFileW(e.path.c_str(), copy.c_str(), TRUE)) {
        error = L"It couldn't be copied for loading.";
        return false;
    }
    HANDLE hold = CreateFileW(copy.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hold == INVALID_HANDLE_VALUE) {
        error = L"Its copy couldn't be opened.";
        return false;
    }
    std::string h;
    std::uint64_t size = 0;
    if (!hashHandle(hold, h, size) || h != e.sha256) {
        CloseHandle(hold);
        DeleteFileW(copy.c_str());
        error = L"Its copy didn't match the file you switched on, so it wasn't loaded.";
        return false;
    }
    HMODULE m = LoadLibraryExW(copy.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    CloseHandle(hold);
    if (!m) {
        const DWORD le = GetLastError();
        wchar_t b[128];
        if (le == ERROR_BAD_EXE_FORMAT) swprintf(b, 128, L"It isn't a 64-bit Windows DLL (error %lu), so it can't run in Capture.", le);
        else if (le == ERROR_MOD_NOT_FOUND) swprintf(b, 128, L"It needs another DLL that isn't on this PC (error %lu).", le);
        else swprintf(b, 128, L"Windows couldn't load it (error %lu).", le);
        error = b;
        DeleteFileW(copy.c_str());   // no copies of things that don't load
        return false;
    }
    auto entryFn = reinterpret_cast<NrAudioPluginEntry>(reinterpret_cast<void*>(GetProcAddress(m, NR_AUDIO_PLUGIN_ENTRY)));
    const NrAudioPlugin* d = entryFn ? entryFn() : nullptr;
    if (!d) {
        FreeLibrary(m);
        DeleteFileW(copy.c_str());
        error = L"It isn't a NorthRay Capture plugin (it has no nr_audio_plugin).";
        return false;
    }
    plugins::Info info;
    info.api = d->api;
    if (d->api == NR_AUDIO_PLUGIN_API) {
        info.name = d->name ? d->name : "";
        info.author = d->author ? d->author : "";
        info.version = d->version ? d->version : "";
        info.hasCreate = d->create != nullptr;
        info.hasDestroy = d->destroy != nullptr;
        info.hasSetParam = d->set_param != nullptr;
        info.hasProcess = d->process != nullptr;
        if (d->param_count > NR_AUDIO_PLUGIN_MAX_PARAMS || (d->param_count && !d->params)) {
            info.params.resize(NR_AUDIO_PLUGIN_MAX_PARAMS + 1);   // refused by validate()
        } else {
            for (std::uint32_t i = 0; i < d->param_count; ++i) {
                const NrAudioParam& p = d->params[i];
                info.params.push_back({p.key ? p.key : "", p.label ? p.label : (p.key ? p.key : ""), p.unit ? p.unit : "", p.min, p.max, p.def});
            }
        }
    }
    std::string why;
    if (!plugins::validate(info, why)) {
        FreeLibrary(m);
        DeleteFileW(copy.c_str());
        error = widen(why);
        return false;
    }
    Loaded& l = loaded_[e.file];
    l.module = m;
    l.desc = d;
    l.sha256 = e.sha256;
    l.info = info;
    for (auto& p : l.info.params) l.params.push_back({p.key.c_str(), p.label.c_str(), p.min, p.max, p.def, p.unit.c_str()});
    e.loaded = true;
    e.info = info;
    e.problem.clear();
    log::info("Plugin loaded: " + e.file + " (" + info.name + " " + info.version + ", sha256 " + e.sha256.substr(0, 16) + "…)");
    return true;
}

bool PluginHost::enable(const std::string& file, const std::string& shown, std::wstring& error) {
    rescan();
    PluginEntry* e = nullptr;
    for (auto& x : entries_)
        if (x.file == file) e = &x;
    if (!e || e->sha256.empty()) {
        error = L"The file isn't in the plugins folder any more.";
        return false;
    }
    if (e->sha256 != shown) {
        error = L"The file changed since its details were shown. Look at them again before switching it on.";
        return false;
    }
    if (!load(*e, error)) {
        e->problem = narrow(error);
        return false;
    }
    plugins::enable(registry_, {e->file, e->sha256, e->size});
    e->enabled = true;
    e->offReason.clear();
    save();
    log::info("Plugin switched on by the user: " + file);
    return true;
}

void PluginHost::disable(const std::string& file) {
    plugins::disable(registry_, file);
    for (auto& e : entries_)
        if (e.file == file) {
            e.enabled = false;
            e.loaded = false;
            e.offReason.clear();
        }
    save();
    log::info("Plugin switched off by the user: " + file);
}

void PluginHost::save() { writeText(registryPath_, plugins::toJson(registry_)); }

void PluginHost::markRunning(const std::string& file) {
    if (std::find(running_.begin(), running_.end(), file) != running_.end()) return;
    running_.push_back(file);
    writeText(runningPath_, plugins::runningToText(running_));
}

void PluginHost::cleanExit() {
    running_.clear();
    DeleteFileW(runningPath_.c_str());
}

std::unique_ptr<dsp::External> PluginHost::make(const std::string& file, const std::map<std::string, float>& values) {
    PluginHost& h = get();
    const PluginEntry* e = h.entry(file);
    auto it = h.loaded_.find(file);
    if (!e || !e->enabled || !e->loaded || it == h.loaded_.end()) return nullptr;
    const NrAudioPlugin* d = it->second.desc;
    void* inst = d->create(48000, 2);
    if (!inst) return nullptr;
    const auto& ps = it->second.info.params;
    for (std::uint32_t i = 0; i < ps.size(); ++i) {
        auto v = values.find(ps[i].key);
        const float x = v == values.end() ? ps[i].def : std::clamp(v->second, ps[i].min, ps[i].max);
        d->set_param(inst, i, x);
    }
    h.markRunning(file);
    return std::make_unique<Instance>(d, inst);
}

}  // namespace ncap
