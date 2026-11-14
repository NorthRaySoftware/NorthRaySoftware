#include "video/devices.hpp"

#include <dwmapi.h>
#include <mfapi.h>
#include <mfidl.h>

#include <algorithm>

namespace ncap::video {

namespace {

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// The monitor's own name ("DELL U2415"), through the display configuration API.
std::wstring friendlyName(const std::wstring& gdiDevice) {
    UINT32 paths = 0, modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS) return {};
    std::vector<DISPLAYCONFIG_PATH_INFO> p(paths);
    std::vector<DISPLAYCONFIG_MODE_INFO> m(modes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, p.data(), &modes, m.data(), nullptr) != ERROR_SUCCESS) return {};
    for (UINT32 i = 0; i < paths; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof src;
        src.header.adapterId = p[i].sourceInfo.adapterId;
        src.header.id = p[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS || gdiDevice != src.viewGdiDeviceName) continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
        tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tgt.header.size = sizeof tgt;
        tgt.header.adapterId = p[i].targetInfo.adapterId;
        tgt.header.id = p[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tgt.header) == ERROR_SUCCESS) return tgt.monitorFriendlyDeviceName;
    }
    return {};
}

int numberOf(const std::wstring& device) {
    // "\\.\DISPLAY12" -> 12
    size_t i = device.size();
    while (i > 0 && iswdigit(device[i - 1])) --i;
    return i < device.size() ? _wtoi(device.c_str() + i) : 0;
}

std::wstring exeOf(DWORD pid) {
    std::wstring out;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = (DWORD)(sizeof buf / sizeof buf[0]);
        if (QueryFullProcessImageNameW(p, 0, buf, &n)) {
            out.assign(buf, n);
            const size_t slash = out.find_last_of(L"\\/");
            if (slash != std::wstring::npos) out = out.substr(slash + 1);
        }
        CloseHandle(p);
    }
    return out;
}

bool capturable(HWND h) {
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return false;
    const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return false;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked) return false;
    if (GetWindowTextLengthW(h) == 0) return false;
    RECT r{};
    GetWindowRect(h, &r);
    if (r.right - r.left < 2 || r.bottom - r.top < 2) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid != GetCurrentProcessId();
}

}  // namespace

std::wstring MonitorInfo::label() const {
    wchar_t size[48];
    swprintf(size, 48, L"%ld\u00D7%ld", rect.right - rect.left, rect.bottom - rect.top);
    std::wstring s = L"Display " + std::to_wstring(number);
    if (!name.empty()) s += L": " + name;
    s += L" (" + std::wstring(size) + (primary ? L", main)" : L")");
    return s;
}

std::vector<MonitorInfo> listMonitors() {
    std::vector<MonitorInfo> out;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof mi;
            if (GetMonitorInfoW(m, &mi)) {
                MonitorInfo info;
                info.device = mi.szDevice;
                info.rect = mi.rcMonitor;
                info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
                info.handle = m;
                info.number = numberOf(info.device);
                reinterpret_cast<std::vector<MonitorInfo>*>(lp)->push_back(info);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&out));
    std::sort(out.begin(), out.end(), [](const MonitorInfo& a, const MonitorInfo& b) { return a.number < b.number; });
    for (auto& m : out) m.name = friendlyName(m.device);
    return out;
}

WindowIdentity identityOf(HWND h) {
    WindowIdentity id;
    const int n = GetWindowTextLengthW(h);
    std::wstring t((size_t)n + 1, L'\0');
    GetWindowTextW(h, t.data(), n + 1);
    t.resize(wcslen(t.c_str()));
    id.title = utf8(t);
    wchar_t cls[256] = L"";
    GetClassNameW(h, cls, 256);
    id.windowClass = utf8(cls);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    id.exe = utf8(exeOf(pid));
    return id;
}

std::vector<WindowInfo> listWindows() {
    std::vector<WindowInfo> out;
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            if (!capturable(h)) return TRUE;
            WindowInfo w;
            w.hwnd = h;
            w.id = identityOf(h);
            GetWindowThreadProcessId(h, &w.pid);
            // Windows' own shell surfaces aren't application windows.
            if (w.id.windowClass == "Progman" || w.id.windowClass == "Shell_TrayWnd" || w.id.windowClass == "Windows.UI.Core.CoreWindow") return TRUE;
            reinterpret_cast<std::vector<WindowInfo>*>(lp)->push_back(w);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&out));
    auto lower = [](std::string s) {
        for (auto& c : s) c = (char)tolower((unsigned char)c);
        return s;
    };
    std::sort(out.begin(), out.end(), [&](const WindowInfo& a, const WindowInfo& b) {
        const std::string ea = lower(a.id.exe), eb = lower(b.id.exe);
        return ea != eb ? ea < eb : lower(a.id.title) < lower(b.id.title);
    });
    return out;
}

HWND findWindow(const WindowIdentity& wanted) {
    const std::vector<WindowInfo> all = listWindows();
    std::vector<WindowIdentity> ids;
    ids.reserve(all.size());
    for (auto& w : all) ids.push_back(w.id);
    const int i = matchWindow(wanted, ids);
    return i >= 0 ? all[(size_t)i].hwnd : nullptr;
}

std::vector<CameraInfo> listCameras() {
    std::vector<CameraInfo> out;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return out;
    IMFAttributes* attrs = nullptr;
    if (SUCCEEDED(MFCreateAttributes(&attrs, 1)) &&
        SUCCEEDED(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) {
        IMFActivate** devices = nullptr;
        UINT32 n = 0;
        if (SUCCEEDED(MFEnumDeviceSources(attrs, &devices, &n)) && devices) {
            for (UINT32 i = 0; i < n; ++i) {
                CameraInfo c;
                WCHAR* s = nullptr;
                UINT32 len = 0;
                if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &s, &len)) && s) {
                    c.name = s;
                    CoTaskMemFree(s);
                }
                if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &s, &len)) && s) {
                    c.link = s;
                    CoTaskMemFree(s);
                }
                if (!c.link.empty()) out.push_back(c);
                devices[i]->Release();
            }
            CoTaskMemFree(devices);
        }
    }
    if (attrs) attrs->Release();
    MFShutdown();
    return out;
}

}  // namespace ncap::video
