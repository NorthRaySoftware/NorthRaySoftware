#include "audio/devices.hpp"

#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <algorithm>
#include <map>

namespace ncap::audio {

using Microsoft::WRL::ComPtr;

namespace {

// Defined here: MinGW declares it but its import library doesn't have it.
const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

ComPtr<IMMDeviceEnumerator> enumerator() {
    ComPtr<IMMDeviceEnumerator> e;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e));
    return e;
}

std::wstring friendly(IMMDevice* d) {
    ComPtr<IPropertyStore> props;
    std::wstring name;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) name = v.pwszVal;
        PropVariantClear(&v);
    }
    return name;
}

std::vector<Device> list(EDataFlow flow) {
    std::vector<Device> out;
    auto e = enumerator();
    if (!e) return out;
    const std::wstring def = defaultDevice(flow == eCapture);
    ComPtr<IMMDeviceCollection> all;
    if (FAILED(e->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &all))) return out;
    UINT n = 0;
    all->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        ComPtr<IMMDevice> d;
        LPWSTR id = nullptr;
        if (FAILED(all->Item(i, &d)) || FAILED(d->GetId(&id))) continue;
        Device dev;
        dev.id = id;
        CoTaskMemFree(id);
        dev.name = friendly(d.Get());
        if (dev.name.empty()) dev.name = flow == eCapture ? L"Input device" : L"Output device";
        dev.isDefault = dev.id == def;
        out.push_back(dev);
    }
    return out;
}

struct Proc {
    DWORD parent = 0;
    std::wstring exe;
};
std::map<DWORD, Proc> processes() {
    std::map<DWORD, Proc> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) out[pe.th32ProcessID] = {pe.th32ParentProcessID, pe.szExeFile};
    CloseHandle(snap);
    return out;
}

// Up the tree while the parent is the same program: Chrome's audio service and Discord's helpers
// are children of the main process of the same name.
DWORD treeTop(DWORD pid, const std::map<DWORD, Proc>& procs) {
    auto it = procs.find(pid);
    if (it == procs.end()) return pid;
    const std::wstring exe = lower(it->second.exe);
    for (int guard = 0; guard < 32; ++guard) {
        auto p = procs.find(it->second.parent);
        if (p == procs.end() || p->first == it->first || lower(p->second.exe) != exe) break;
        it = p;
    }
    return it->first;
}

std::wstring imagePath(DWORD pid) {
    std::wstring out;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = (DWORD)(sizeof buf / sizeof buf[0]);
        if (QueryFullProcessImageNameW(p, 0, buf, &n)) out.assign(buf, n);
        CloseHandle(p);
    }
    return out;
}

// The program's own description ("Google Chrome"), from its version information.
std::wstring description(const std::wstring& path) {
    if (path.empty()) return {};
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!size) return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
    struct Lang { WORD lang, page; }* langs = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &len) || len < sizeof(Lang)) return {};
    wchar_t key[64];
    swprintf(key, 64, L"\\StringFileInfo\\%04x%04x\\FileDescription", langs[0].lang, langs[0].page);
    wchar_t* value = nullptr;
    if (!VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&value), &len) || !value || !*value) return {};
    std::wstring d = value;
    while (!d.empty() && d.back() == L' ') d.pop_back();
    return d;
}

}  // namespace

std::wstring defaultDevice(bool input) {
    auto e = enumerator();
    ComPtr<IMMDevice> d;
    LPWSTR id = nullptr;
    if (!e || FAILED(e->GetDefaultAudioEndpoint(input ? eCapture : eRender, eConsole, &d)) || FAILED(d->GetId(&id))) return {};
    std::wstring s = id;
    CoTaskMemFree(id);
    return s;
}

std::wstring deviceName(const std::wstring& id) {
    auto e = enumerator();
    ComPtr<IMMDevice> d;
    if (!e || FAILED(e->GetDevice(id.c_str(), &d))) return {};
    return friendly(d.Get());
}

std::vector<Device> listInputs() { return list(eCapture); }
std::vector<Device> listOutputs() { return list(eRender); }

std::vector<App> listAudioApps() {
    const auto procs = processes();
    std::map<std::wstring, App> byExe;   // one per program
    auto e = enumerator();
    ComPtr<IMMDeviceCollection> all;
    if (e && SUCCEEDED(e->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &all))) {
        UINT n = 0;
        all->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            ComPtr<IMMDevice> d;
            ComPtr<IAudioSessionManager2> mgr;
            ComPtr<IAudioSessionEnumerator> sessions;
            if (FAILED(all->Item(i, &d)) || FAILED(d->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &mgr)) ||
                FAILED(mgr->GetSessionEnumerator(&sessions)))
                continue;
            int count = 0;
            sessions->GetCount(&count);
            std::wstring devId;
            {
                LPWSTR raw = nullptr;
                if (SUCCEEDED(d->GetId(&raw)) && raw) {
                    devId = raw;
                    CoTaskMemFree(raw);
                }
            }
            for (int s = 0; s < count; ++s) {
                ComPtr<IAudioSessionControl> ctl;
                ComPtr<IAudioSessionControl2> ctl2;
                if (FAILED(sessions->GetSession(s, &ctl)) || FAILED(ctl.As(&ctl2))) continue;
                if (ctl2->IsSystemSoundsSession() == S_OK) continue;
                DWORD pid = 0;
                if (FAILED(ctl2->GetProcessId(&pid)) || pid == 0 || pid == GetCurrentProcessId()) continue;
                auto p = procs.find(pid);
                if (p == procs.end()) continue;
                AudioSessionState state = AudioSessionStateInactive;
                ctl->GetState(&state);
                const std::wstring key = lower(p->second.exe);
                App& app = byExe[key];
                if (app.exe.empty()) {
                    app.exe = p->second.exe;
                    app.pid = treeTop(pid, procs);
                    app.name = description(imagePath(app.pid));
                    if (app.name.empty()) app.name = describeExe(app.exe);
                }
                app.playing = app.playing || state == AudioSessionStateActive;
                if (std::find(app.devices.begin(), app.devices.end(), devId) == app.devices.end()) app.devices.push_back(devId);
            }
        }
    }
    std::vector<App> out;
    for (auto& [k, a] : byExe) out.push_back(a);
    std::sort(out.begin(), out.end(), [](const App& a, const App& b) {
        return a.playing != b.playing ? a.playing : lower(a.name) < lower(b.name);
    });
    return out;
}

DWORD findProgram(const std::wstring& exe) {
    const std::wstring want = lower(exe);
    for (auto& a : listAudioApps())
        if (lower(a.exe) == want) return a.pid;
    // Running without an audio session yet (it may start playing later): the top of its tree.
    const auto procs = processes();
    for (auto& [pid, p] : procs)
        if (lower(p.exe) == want) return treeTop(pid, procs);
    return 0;
}

std::wstring describeExe(const std::wstring& exe) {
    std::wstring n = exe;
    if (n.size() > 4 && lower(n.substr(n.size() - 4)) == L".exe") n.resize(n.size() - 4);
    return n;
}

bool applicationAudioSupported() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{sizeof v};
    if (auto f = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))) f(&v);
    return v.dwMajorVersion > 10 || (v.dwMajorVersion == 10 && v.dwBuildNumber >= 20348);
}

}  // namespace ncap::audio
