#include "system.hpp"

#include <dxgi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <psapi.h>

#include <thread>

namespace ncap::sys {

namespace {

ULONGLONG ft(const FILETIME& f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

// 'AV01' (MinGW's headers may not have it).
const GUID kAV1 = {0x31305641, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

std::wstring vendorOf(const std::wstring& name, const std::wstring& vendorId) {
    auto has = [&](const wchar_t* s) { return name.find(s) != std::wstring::npos; };
    if (vendorId.find(L"10DE") != std::wstring::npos || has(L"NVIDIA") || has(L"NVENC")) return L"NVIDIA";
    if (vendorId.find(L"1002") != std::wstring::npos || vendorId.find(L"1022") != std::wstring::npos || has(L"AMD") || has(L"AMF")) return L"AMD";
    if (vendorId.find(L"8086") != std::wstring::npos || has(L"Intel") || has(L"Quick Sync")) return L"Intel";
    if (has(L"Microsoft")) return L"Microsoft";
    return L"";
}

void enumerate(const GUID& category, const GUID& major, const GUID& subtype, const char* codec, UINT32 flags, std::vector<Encoder>& out) {
    MFT_REGISTER_TYPE_INFO info{major, subtype};
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    if (FAILED(MFTEnumEx(category, flags, nullptr, &info, &acts, &n)) || !acts) return;
    for (UINT32 i = 0; i < n; ++i) {
        Encoder e;
        e.codec = codec;
        e.hardware = (flags & MFT_ENUM_FLAG_HARDWARE) != 0;
        WCHAR* s = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &s, &len)) && s) {
            e.name = s;
            CoTaskMemFree(s);
        }
        std::wstring vid;
        if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_ENUM_HARDWARE_VENDOR_ID_Attribute, &s, &len)) && s) {
            vid = s;
            CoTaskMemFree(s);
        }
        e.vendor = vendorOf(e.name, vid);
        if (e.name.empty()) e.name = (e.vendor.empty() ? L"Unnamed" : e.vendor) + std::wstring(L" encoder");
        bool dup = false;
        for (auto& o : out) dup = dup || (o.name == e.name && o.codec == e.codec);
        if (!dup) out.push_back(e);
        acts[i]->Release();
    }
    CoTaskMemFree(acts);
}

}  // namespace

Usage UsageSampler::sample() {
    FILETIME c, x, k, u, idle, sk, su;
    GetProcessTimes(GetCurrentProcess(), &c, &x, &k, &u);
    GetSystemTimes(&idle, &sk, &su);
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const ULONGLONG wall = ft(now), proc = ft(k) + ft(u);
    Usage r = last_;
    if (have_ && wall > lastWall_) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        const double cores = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
        r.processCpu = 100.0 * (double)(proc - lastProc_) / ((double)(wall - lastWall_) * cores);
        const ULONGLONG total = (ft(sk) - lastKernel_) + (ft(su) - lastUser_);   // kernel time includes idle
        const ULONGLONG idleD = ft(idle) - lastIdle_;
        r.systemCpu = total ? 100.0 * (double)(total - idleD) / (double)total : 0;
    }
    lastWall_ = wall;
    lastProc_ = proc;
    lastIdle_ = ft(idle);
    lastKernel_ = ft(sk);
    lastUser_ = ft(su);
    have_ = true;
    PROCESS_MEMORY_COUNTERS_EX pm{};
    pm.cb = sizeof pm;
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm)) {
        r.privateBytes = pm.PrivateUsage;
        r.workingSet = pm.WorkingSetSize;
    }
    last_ = r;
    return r;
}

std::vector<Encoder> findEncoders() {
    std::vector<Encoder> out;
    const UINT32 hw = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;
    const UINT32 sw = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER;
    struct C { const GUID* sub; const char* name; };
    const C video[] = {{&MFVideoFormat_H264, "H.264"}, {&MFVideoFormat_HEVC, "HEVC"}, {&kAV1, "AV1"}};
    for (const C& c : video) {
        enumerate(MFT_CATEGORY_VIDEO_ENCODER, MFMediaType_Video, *c.sub, c.name, hw, out);
        enumerate(MFT_CATEGORY_VIDEO_ENCODER, MFMediaType_Video, *c.sub, c.name, sw, out);
    }
    enumerate(MFT_CATEGORY_AUDIO_ENCODER, MFMediaType_Audio, MFAudioFormat_AAC, "AAC", sw, out);
    return out;
}

void findEncodersAsync(HWND window, UINT message) {
    std::thread([window, message] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
        auto* list = new std::vector<Encoder>(mf ? findEncoders() : std::vector<Encoder>{});
        if (mf) MFShutdown();
        if (SUCCEEDED(co)) CoUninitialize();
        if (!IsWindow(window) || !PostMessageW(window, message, 0, reinterpret_cast<LPARAM>(list))) delete list;
    }).detach();
}

std::wstring windowsVersion() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{sizeof v};
    if (auto f = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))) f(&v);
    const wchar_t* name = v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000 ? L"Windows 11" : v.dwMajorVersion == 10 ? L"Windows 10" : L"Windows";
    return std::wstring(name) + L" (build " + std::to_wstring(v.dwBuildNumber) + L")";
}

std::wstring cpuName() {
    wchar_t buf[256] = L"";
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, buf,
                     &size) != ERROR_SUCCESS)
        return L"Unknown processor";
    std::wstring s = buf;
    while (!s.empty() && s.back() == L' ') s.pop_back();
    while (!s.empty() && s.front() == L' ') s.erase(0, 1);
    return s;
}

std::wstring gpuNames() {
    std::wstring out;
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f))) || !f) return L"Unknown";
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            if (!out.empty()) out += L", ";
            out += d.Description;
            out += L" (" + std::to_wstring(d.DedicatedVideoMemory / (1024 * 1024)) + L" MB)";
        }
        a->Release();
    }
    f->Release();
    return out.empty() ? L"Unknown" : out;
}

}  // namespace ncap::sys
