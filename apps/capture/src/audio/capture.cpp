#include "audio/capture.hpp"

#include "audio/devices.hpp"

#include "nlohmann/json.hpp"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <wrl/client.h>

#include <cstring>
#include <vector>

// Process loopback activation (audioclientactivationparams.h, missing from MinGW's headers).
namespace {
enum AUDIOCLIENT_ACTIVATION_TYPE_ { kActivationDefault = 0, kActivationProcessLoopback = 1 };
enum PROCESS_LOOPBACK_MODE_ { kIncludeTree = 0, kExcludeTree = 1 };
struct ProcessLoopbackParams {
    DWORD TargetProcessId;
    int ProcessLoopbackMode;
};
struct ActivationParams {
    int ActivationType;
    ProcessLoopbackParams ProcessLoopbackParams;
};
const wchar_t kProcessLoopbackDevice[] = L"VAD\\Process_Loopback";
}  // namespace

namespace ncap::audio {

using Microsoft::WRL::ComPtr;
using nlohmann::json;

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string str(const json& j, const char* k) { return j.is_object() && j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); }

// The completion handler ActivateAudioInterfaceAsync calls back on (must be agile).
class Activation : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    Activation() { done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    virtual ~Activation() { CloseHandle(done_); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler))
            *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if (riid == __uuidof(IAgileObject))
            *out = static_cast<IAgileObject*>(this);
        else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --refs_;
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT hr = E_FAIL;
        ComPtr<IUnknown> unk;
        if (SUCCEEDED(op->GetActivateResult(&hr, &unk)) && SUCCEEDED(hr) && unk) hr = unk.As(&client);
        result = hr;
        SetEvent(done_);
        return S_OK;
    }
    bool wait(DWORD ms) { return WaitForSingleObject(done_, ms) == WAIT_OBJECT_0; }
    ComPtr<IAudioClient> client;
    HRESULT result = E_PENDING;

private:
    std::atomic<ULONG> refs_{1};
    HANDLE done_;
};

WAVEFORMATEX floatFormat() {
    WAVEFORMATEX f{};
    f.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    f.nChannels = kChannels;
    f.nSamplesPerSec = kRate;
    f.wBitsPerSample = 32;
    f.nBlockAlign = kChannels * 4;
    f.nAvgBytesPerSec = kRate * f.nBlockAlign;
    return f;
}

WAVEFORMATEX pcm16Format() {
    WAVEFORMATEX f = floatFormat();
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.wBitsPerSample = 16;
    f.nBlockAlign = kChannels * 2;
    f.nAvgBytesPerSec = kRate * f.nBlockAlign;
    return f;
}

}  // namespace

Capture::Capture(SourceKind kind) : kind_(kind) {}

Capture::~Capture() {
    quit_ = true;
    if (thread_.joinable()) thread_.join();
}

void Capture::setSettings(const std::string& j) {
    {
        std::lock_guard<std::mutex> l(m_);
        settings_ = j;
    }
    restart_ = true;
    if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
}

std::wstring Capture::status() const {
    std::lock_guard<std::mutex> l(m_);
    return status_;
}

void Capture::setStatus(const std::wstring& s, bool problem) {
    std::lock_guard<std::mutex> l(m_);
    status_ = s;
    problem_ = problem;
}

void Capture::run() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    while (!quit_) {
        restart_ = false;
        json s;
        {
            std::lock_guard<std::mutex> l(m_);
            s = json::parse(settings_, nullptr, false);
        }
        HRESULT hr = S_OK;
        if (kind_ == SourceKind::ApplicationAudio) {
            const std::wstring exe = widen(str(s, "exe"));
            const std::wstring name = describeExe(exe);
            if (!applicationAudioSupported()) {
                setStatus(L"Needs Windows 11 (Windows 10 can't capture one program's sound)", true);
                hr = S_FALSE;
            } else if (const DWORD pid = findProgram(exe)) {
                hr = readFrom({}, true, pid);
                if (hr == S_OK) continue;   // the program closed or the settings changed: look again at once
                if (FAILED(hr)) {
                    wchar_t code[16];
                    swprintf(code, 16, L"%08lX", (unsigned long)hr);
                    setStatus(L"Couldn't capture " + name + L"'s sound. Error " + code, true);
                }
            } else {
                setStatus(L"Waiting for " + name + L" to open");
                hr = S_FALSE;
            }
        } else {
            const bool input = kind_ == SourceKind::InputAudio;
            std::wstring id = widen(str(s, "device"));
            if (id.empty() || id == L"default") id = defaultDevice(input);
            if (id.empty()) {
                setStatus(input ? L"No microphone or input device found" : L"No output device found", true);
                hr = S_FALSE;
            } else {
                hr = readFrom(id, !input, 0);
                if (hr == S_OK) continue;
                if (hr == E_ACCESSDENIED)
                    setStatus(L"Windows' microphone privacy settings are blocking desktop apps (Settings › Privacy & security › Microphone)", true);
                else if (FAILED(hr)) {
                    wchar_t code[16];
                    swprintf(code, 16, L"%08lX", (unsigned long)hr);
                    setStatus(L"Unavailable (unplugged or in use?) \u00B7 error " + std::wstring(code), true);
                }
            }
        }
        live_ = false;
        for (int i = 0; i < 40 && !quit_ && !restart_; ++i) Sleep(50);   // look again in 2 s
    }
    live_ = false;
    if (SUCCEEDED(co)) CoUninitialize();
}

// Reads until stopped (S_OK: the settings changed, the default device changed, or the program
// closed) or until the device fails (an error).
HRESULT Capture::readFrom(const std::wstring& deviceId, bool loopback, DWORD pid) {
    ComPtr<IAudioClient> client;
    HRESULT hr;
    std::wstring what;
    HANDLE process = nullptr;
    if (pid) {
        ActivationParams params{};
        params.ActivationType = kActivationProcessLoopback;
        params.ProcessLoopbackParams.TargetProcessId = pid;
        params.ProcessLoopbackParams.ProcessLoopbackMode = kIncludeTree;
        PROPVARIANT pv{};
        pv.vt = VT_BLOB;
        pv.blob.cbSize = sizeof params;
        pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
        Activation* handler = new Activation();
        ComPtr<IActivateAudioInterfaceAsyncOperation> op;
        hr = ActivateAudioInterfaceAsync(kProcessLoopbackDevice, __uuidof(IAudioClient), &pv, handler, &op);
        if (SUCCEEDED(hr)) {
            if (!handler->wait(5000)) hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            else hr = handler->result;
            client = handler->client;
        }
        handler->Release();
        if (FAILED(hr)) return hr;
        process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    } else {
        ComPtr<IMMDeviceEnumerator> e;
        ComPtr<IMMDevice> device;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e));
        if (SUCCEEDED(hr)) hr = e->GetDevice(deviceId.c_str(), &device);
        if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
        if (FAILED(hr)) return hr;
        what = deviceName(deviceId);
    }
    // 48 kHz stereo float, converted by Windows; 16-bit if float isn't taken.
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                        (loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
    WAVEFORMATEX fmt = floatFormat();
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000 /* 200 ms */, 0, &fmt, nullptr);
    bool pcm16 = false;
    if (FAILED(hr)) {
        fmt = pcm16Format();
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, &fmt, nullptr);
        pcm16 = SUCCEEDED(hr);
    }
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ComPtr<IAudioCaptureClient> cap;
    if (SUCCEEDED(hr)) hr = client->SetEventHandle(ready);
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&cap));
    if (SUCCEEDED(hr)) hr = client->Start();
    if (FAILED(hr)) {
        CloseHandle(ready);
        if (process) CloseHandle(process);
        return hr;
    }
    live_ = true;
    if (pid) setStatus(L"Capturing only this program’s sound");
    else setStatus(what.empty() ? L"Capturing" : what);
    const std::wstring defaultAtStart = deviceId;
    const bool followsDefault = [&] {
        std::lock_guard<std::mutex> l(m_);
        const json s = json::parse(settings_, nullptr, false);
        const std::string d = str(s, "device");
        return !pid && (d.empty() || d == "default");
    }();
    std::vector<float> conv;
    ULONGLONG lastCheck = GetTickCount64();
    hr = S_OK;
    while (!quit_ && !restart_) {
        // Loopback signals nothing while nothing plays, so wait briefly and look anyway.
        WaitForSingleObject(ready, 10);
        UINT32 packet = 0;
        while (SUCCEEDED(hr = cap->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD f = 0;
            if (FAILED(hr = cap->GetBuffer(&data, &frames, &f, nullptr, nullptr))) break;
            if ((f & AUDCLNT_BUFFERFLAGS_SILENT) || !data) ring_.writeSilence(frames);
            else if (pcm16) {
                conv.resize((size_t)frames * kChannels);
                const int16_t* s16 = reinterpret_cast<const int16_t*>(data);
                for (size_t i = 0; i < conv.size(); ++i) conv[i] = s16[i] / 32768.0f;
                ring_.write(conv.data(), frames);
            } else {
                ring_.write(reinterpret_cast<const float*>(data), frames);
            }
            cap->ReleaseBuffer(frames);
        }
        if (FAILED(hr)) break;   // AUDCLNT_E_DEVICE_INVALIDATED: unplugged
        const ULONGLONG now = GetTickCount64();
        if (now - lastCheck >= 1000) {
            lastCheck = now;
            if (process && WaitForSingleObject(process, 0) == WAIT_OBJECT_0) break;   // the program closed
            if (followsDefault && defaultDevice(kind_ == SourceKind::InputAudio) != defaultAtStart) break;   // a new default
        }
    }
    client->Stop();
    live_ = false;
    CloseHandle(ready);
    if (process) CloseHandle(process);
    return FAILED(hr) ? hr : S_OK;
}

}  // namespace ncap::audio
