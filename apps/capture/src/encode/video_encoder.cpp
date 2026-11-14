#include "encode/video_encoder.hpp"

#include <codecapi.h>
#include <d3d11.h>
#include <strmif.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>

#include <algorithm>
#include <cstring>

// MinGW declares ICodecAPI without its IID for __uuidof.
__CRT_UUID_DECL(ICodecAPI, 0x901db4c7, 0x31ce, 0x41a2, 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda)

namespace ncap::encode {

using Microsoft::WRL::ComPtr;

namespace {

const GUID kRateControl = {STATIC_CODECAPI_AVEncCommonRateControlMode};
const GUID kMeanBitRate = {STATIC_CODECAPI_AVEncCommonMeanBitRate};
const GUID kMaxBitRate = {STATIC_CODECAPI_AVEncCommonMaxBitRate};
const GUID kGopSize = {STATIC_CODECAPI_AVEncMPVGOPSize};
const GUID kBFrames = {STATIC_CODECAPI_AVEncMPVDefaultBPictureCount};

constexpr int kSlots = 8;         // NV12 frames in flight
constexpr std::size_t kQueue = 3; // frames waiting for the encoder before new ones are dropped

std::wstring nameOf(IMFActivate* a) {
    std::wstring n;
    WCHAR* s = nullptr;
    UINT32 len = 0;
    if (SUCCEEDED(a->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &s, &len)) && s) {
        n = s;
        CoTaskMemFree(s);
    }
    return n;
}

void setUint(ICodecAPI* api, const GUID& g, UINT32 v) {
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_UI4;
    var.ulVal = v;
    api->SetValue(&g, &var);
}

std::wstring hrText(HRESULT hr) {
    wchar_t b[16];
    swprintf(b, 16, L"%08lX", (unsigned long)hr);
    return b;
}

}  // namespace

std::vector<EncoderInfo> listH264Encoders() {
    std::vector<EncoderInfo> out;
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12}, outType{MFMediaType_Video, MFVideoFormat_H264};
    for (UINT32 flags : {(UINT32)(MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER),
                         (UINT32)(MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER)}) {
        IMFActivate** acts = nullptr;
        UINT32 n = 0;
        if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &in, &outType, &acts, &n)) || !acts) continue;
        for (UINT32 i = 0; i < n; ++i) {
            const std::wstring name = nameOf(acts[i]);
            bool dup = false;
            for (auto& e : out) dup = dup || e.name == name;
            if (!dup && !name.empty()) out.push_back({name, (flags & MFT_ENUM_FLAG_HARDWARE) != 0});
            acts[i]->Release();
        }
        CoTaskMemFree(acts);
    }
    return out;
}

struct VideoEncoder::Impl {
    video::Gpu* gpu = nullptr;
    VideoSettings s;
    ComPtr<IMFTransform> mft;
    ComPtr<IMFMediaEventGenerator> events;
    ComPtr<IMFDXGIDeviceManager> manager;
    bool async = false, providesSamples = false;
    DWORD inId = 0, outId = 0;
    UINT32 outBufferSize = 0;
    // Conversion to NV12 on the graphics card.
    ComPtr<ID3D11VideoDevice> vdev;
    ComPtr<ID3D11VideoContext> vctx;
    ComPtr<ID3D11VideoProcessorEnumerator> venum;
    ComPtr<ID3D11VideoProcessor> vproc;
    ID3D11Texture2D* viewOf = nullptr;
    ComPtr<ID3D11VideoProcessorInputView> inView;
    ComPtr<ID3D11Texture2D> slots[kSlots];
    ComPtr<ID3D11VideoProcessorOutputView> outViews[kSlots];
    int nextSlot = 0;
    ComPtr<ID3D11Texture2D> staging;   // software encoders read from memory
    // Frames for the encoder thread, packets back.
    struct Frame { int slot; std::int64_t pts; };
    std::mutex m;
    std::condition_variable cv;
    std::deque<Frame> frames;
    bool draining = false;
    std::mutex pm;
    std::deque<media::Packet> packets;
    std::thread thread;
    std::atomic<bool> quit{false};
    std::mutex em;
    std::wstring error;
    bool finished = false;
    std::atomic<bool> threadDone{false};

    void setError(const std::wstring& e) {
        std::lock_guard<std::mutex> l(em);
        if (error.empty()) error = e;
    }

    bool setupConversion(std::wstring& why) {
        ID3D11Device* dev = gpu->device();
        if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&vdev))) || FAILED(gpu->context()->QueryInterface(IID_PPV_ARGS(&vctx)))) {
            why = L"The graphics driver has no video processor (needed to convert the picture for the encoder).";
            return false;
        }
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputFrameRate = {(UINT)s.fps, 1};
        cd.InputWidth = (UINT)s.width;
        cd.InputHeight = (UINT)s.height;
        cd.OutputFrameRate = {(UINT)s.fps, 1};
        cd.OutputWidth = (UINT)s.width;
        cd.OutputHeight = (UINT)s.height;
        cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
        if (FAILED(vdev->CreateVideoProcessorEnumerator(&cd, &venum)) || FAILED(vdev->CreateVideoProcessor(venum.Get(), 0, &vproc))) {
            why = L"The graphics driver's video processor couldn't be set up.";
            return false;
        }
        // RGB (full range) in; BT.709 limited range out, which is what players expect of H.264.
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{}, out{};
        in.RGB_Range = 0;
        out.YCbCr_Matrix = 1;
        out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        vctx->VideoProcessorSetStreamColorSpace(vproc.Get(), 0, &in);
        vctx->VideoProcessorSetOutputColorSpace(vproc.Get(), &out);
        vctx->VideoProcessorSetStreamFrameFormat(vproc.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)s.width;
        td.Height = (UINT)s.height;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        for (int i = 0; i < kSlots; ++i) {
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};
            ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &slots[i])) ||
                FAILED(vdev->CreateVideoProcessorOutputView(slots[i].Get(), venum.Get(), &ov, &outViews[i]))) {
                why = L"The graphics card couldn't make the encoder's frames (NV12).";
                return false;
            }
        }
        if (!async) {
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &staging))) {
                why = L"The graphics card couldn't make a copy for the software encoder.";
                return false;
            }
        }
        return true;
    }

    bool setupEncoder(IMFActivate* act, bool hw, std::wstring& why) {
        HRESULT hr = act->ActivateObject(IID_PPV_ARGS(&mft));
        if (FAILED(hr)) {
            why = L"The encoder couldn't be started (" + hrText(hr) + L").";
            return false;
        }
        ComPtr<IMFAttributes> attrs;
        if (SUCCEEDED(mft->GetAttributes(&attrs))) {
            UINT32 isAsync = 0;
            if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync)) && isAsync) {
                async = true;
                attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            }
        }
        if (async && FAILED(mft.As(&events))) {
            why = L"The encoder didn't offer its events.";
            return false;
        }
        DWORD nIn = 0, nOut = 0;
        if (SUCCEEDED(mft->GetStreamCount(&nIn, &nOut)) && nIn && nOut) {
            DWORD ins[4] = {}, outs[4] = {};
            if (SUCCEEDED(mft->GetStreamIDs(std::min<DWORD>(nIn, 4), ins, std::min<DWORD>(nOut, 4), outs))) {
                inId = ins[0];
                outId = outs[0];
            }
        }
        // Hardware encoders take the frames from the graphics card directly.
        if (hw) {
            UINT token = 0;
            UINT32 aware = 0;
            if (attrs && SUCCEEDED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &aware)) && aware && SUCCEEDED(MFCreateDXGIDeviceManager(&token, &manager)) &&
                SUCCEEDED(manager->ResetDevice(gpu->device(), token)))
                mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager.Get()));
            else
                manager.Reset();
        }
        // Settings: rate control, bitrate, keyframes every N seconds, no B-frames (so decode order is
        // show order, which keeps timestamps simple and latency low).
        ComPtr<ICodecAPI> api;
        if (SUCCEEDED(mft.As(&api))) {
            setUint(api.Get(), kRateControl, s.constantBitrate ? eAVEncCommonRateControlMode_CBR : eAVEncCommonRateControlMode_PeakConstrainedVBR);
            setUint(api.Get(), kMeanBitRate, (UINT32)s.bitrateKbps * 1000);
            if (!s.constantBitrate) setUint(api.Get(), kMaxBitRate, (UINT32)s.bitrateKbps * 1500);
            setUint(api.Get(), kGopSize, (UINT32)(s.fps * s.keyframeSeconds));
            setUint(api.Get(), kBFrames, 0);
        }
        ComPtr<IMFMediaType> outType, inType;
        MFCreateMediaType(&outType);
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        outType->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)s.bitrateKbps * 1000);
        MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, (UINT32)s.width, (UINT32)s.height);
        MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, (UINT32)s.fps, 1);
        MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        outType->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        outType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        outType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        hr = mft->SetOutputType(outId, outType.Get(), 0);
        if (FAILED(hr)) {   // some encoders don't take High: try Main
            outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
            hr = mft->SetOutputType(outId, outType.Get(), 0);
        }
        if (FAILED(hr)) {
            why = L"The encoder doesn't take this size or frame rate (" + hrText(hr) + L").";
            return false;
        }
        MFCreateMediaType(&inType);
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, (UINT32)s.width, (UINT32)s.height);
        MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, (UINT32)s.fps, 1);
        MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        hr = mft->SetInputType(inId, inType.Get(), 0);
        if (FAILED(hr)) {
            why = L"The encoder doesn't take NV12 frames (" + hrText(hr) + L").";
            return false;
        }
        if (api) {   // some encoders only take these once the types are set
            setUint(api.Get(), kMeanBitRate, (UINT32)s.bitrateKbps * 1000);
            setUint(api.Get(), kGopSize, (UINT32)(s.fps * s.keyframeSeconds));
            setUint(api.Get(), kBFrames, 0);
        }
        MFT_OUTPUT_STREAM_INFO info{};
        if (SUCCEEDED(mft->GetOutputStreamInfo(outId, &info))) {
            providesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            outBufferSize = std::max<UINT32>(info.cbSize, (UINT32)(s.width * s.height * 3 / 2));
        }
        mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    // One frame into the encoder.
    HRESULT feed(const Frame& f) {
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buf;
        HRESULT hr = MFCreateSample(&sample);
        if (SUCCEEDED(hr) && manager) {
            hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), slots[f.slot].Get(), 0, FALSE, &buf);
            if (SUCCEEDED(hr)) {   // the whole frame counts as data
                ComPtr<IMF2DBuffer> b2;
                DWORD len = 0;
                if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->GetContiguousLength(&len))) buf->SetCurrentLength(len);
            }
        } else if (SUCCEEDED(hr)) {
            // Through memory: copy the NV12 frame down from the graphics card.
            const DWORD size = (DWORD)(s.width * s.height * 3 / 2);
            hr = MFCreateMemoryBuffer(size, &buf);
            BYTE* dst = nullptr;
            D3D11_MAPPED_SUBRESOURCE map{};
            if (SUCCEEDED(hr)) {
                gpu->context()->CopyResource(staging.Get(), slots[f.slot].Get());
                hr = gpu->context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map);
            }
            if (SUCCEEDED(hr) && SUCCEEDED(hr = buf->Lock(&dst, nullptr, nullptr))) {
                const BYTE* src = static_cast<const BYTE*>(map.pData);
                for (int y = 0; y < s.height * 3 / 2; ++y) std::memcpy(dst + (size_t)y * s.width, src + (size_t)y * map.RowPitch, (size_t)s.width);
                buf->Unlock();
                buf->SetCurrentLength(size);
            }
            if (map.pData) gpu->context()->Unmap(staging.Get(), 0);
        }
        if (SUCCEEDED(hr)) hr = sample->AddBuffer(buf.Get());
        if (SUCCEEDED(hr)) {
            sample->SetSampleTime(f.pts * 10);
            sample->SetSampleDuration(10000000LL / s.fps);
            hr = mft->ProcessInput(inId, sample.Get(), 0);
        }
        return hr;
    }

    // Takes whatever the encoder has ready. S_OK: got one; MF_E_TRANSFORM_NEED_MORE_INPUT: none yet.
    HRESULT drainOutput() {
        MFT_OUTPUT_DATA_BUFFER out{};
        out.dwStreamID = outId;
        ComPtr<IMFSample> own;
        if (!providesSamples) {
            ComPtr<IMFMediaBuffer> b;
            MFCreateSample(&own);
            MFCreateMemoryBuffer(outBufferSize, &b);
            own->AddBuffer(b.Get());
            out.pSample = own.Get();
        }
        DWORD status = 0;
        HRESULT hr = mft->ProcessOutput(0, 1, &out, &status);
        if (out.pEvents) out.pEvents->Release();
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // The encoder changed its output format: take the one it offers now.
            ComPtr<IMFMediaType> t;
            if (SUCCEEDED(mft->GetOutputAvailableType(outId, 0, &t))) mft->SetOutputType(outId, t.Get(), 0);
            if (providesSamples && out.pSample) out.pSample->Release();
            return S_FALSE;
        }
        if (FAILED(hr)) {
            if (providesSamples && out.pSample) out.pSample->Release();
            return hr;
        }
        IMFSample* sample = out.pSample;
        ComPtr<IMFMediaBuffer> buf;
        if (sample && SUCCEEDED(sample->ConvertToContiguousBuffer(&buf))) {
            BYTE* d = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(buf->Lock(&d, nullptr, &len))) {
                media::Packet p;
                p.stream = 0;
                LONGLONG t = 0;
                sample->GetSampleTime(&t);
                p.pts = p.dts = t / 10;
                UINT32 clean = 0;
                p.keyframe = (SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &clean)) && clean) || media::containsKeyframe(d, len);
                p.data.assign(d, d + len);
                buf->Unlock();
                std::lock_guard<std::mutex> l(pm);
                packets.push_back(std::move(p));
            }
        }
        if (providesSamples && sample) sample->Release();
        return S_OK;
    }

    // Waits for the next frame; false when stopping with nothing left.
    bool nextFrame(Frame& f) {
        std::unique_lock<std::mutex> l(m);
        cv.wait(l, [&] { return !frames.empty() || draining || quit; });
        if (frames.empty()) return false;
        f = frames.front();
        frames.pop_front();
        return true;
    }

    void runAsync() {
        bool drainSent = false;
        while (!quit) {
            ComPtr<IMFMediaEvent> ev;
            if (FAILED(events->GetEvent(0, &ev))) break;   // blocks until the encoder has something to say
            MediaEventType type = MEUnknown;
            ev->GetType(&type);
            if (type == METransformNeedInput) {
                Frame f;
                if (nextFrame(f)) {
                    const HRESULT hr = feed(f);
                    if (FAILED(hr)) setError(L"The encoder refused a frame (" + hrText(hr) + L").");
                } else if (!drainSent) {
                    drainSent = true;
                    mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
                }
            } else if (type == METransformHaveOutput) {
                drainOutput();
            } else if (type == METransformDrainComplete) {
                break;
            } else if (type == MEError) {
                HRESULT st = S_OK;
                ev->GetStatus(&st);
                setError(L"The encoder stopped with an error (" + hrText(st) + L").");
                break;
            }
        }
    }

    void runSync() {
        Frame f;
        while (nextFrame(f)) {
            HRESULT hr = feed(f);
            if (hr == MF_E_NOTACCEPTING) {   // take its output first, then give the frame again
                while (drainOutput() == S_OK) {}
                hr = feed(f);
            }
            if (FAILED(hr)) setError(L"The encoder refused a frame (" + hrText(hr) + L").");
            while (drainOutput() == S_OK) {}
        }
        mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        for (int i = 0; i < 64 && drainOutput() != MF_E_TRANSFORM_NEED_MORE_INPUT; ++i) {}
    }
};

bool VideoEncoder::open(video::Gpu& gpu, const VideoSettings& s, std::wstring& error) {
    close();
    impl_ = new Impl();
    impl_->gpu = &gpu;
    impl_->s = s;
    // The encoders on offer: the named one, else hardware first.
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12}, out{MFMediaType_Video, MFVideoFormat_H264};
    std::vector<std::pair<ComPtr<IMFActivate>, bool>> candidates;
    for (UINT32 flags : {(UINT32)(MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER),
                         (UINT32)(MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER)}) {
        IMFActivate** acts = nullptr;
        UINT32 n = 0;
        if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &in, &out, &acts, &n)) || !acts) continue;
        for (UINT32 i = 0; i < n; ++i) {
            ComPtr<IMFActivate> a;
            a.Attach(acts[i]);
            if (s.encoder.empty() || nameOf(a.Get()) == s.encoder) candidates.push_back({a, (flags & MFT_ENUM_FLAG_HARDWARE) != 0});
        }
        CoTaskMemFree(acts);
    }
    if (candidates.empty()) {
        error = s.encoder.empty() ? L"No H.264 encoder was found on this PC. A graphics driver update usually adds the hardware ones."
                                  : L"The chosen encoder (" + s.encoder + L") isn't available. Choose another in Settings › Recording.";
        close();
        return false;
    }
    std::wstring why;
    for (auto& [act, hw] : candidates) {
        Impl* fresh = new Impl();
        fresh->gpu = &gpu;
        fresh->s = s;
        if (fresh->setupEncoder(act.Get(), hw, why) && fresh->setupConversion(why)) {
            delete impl_;
            impl_ = fresh;
            name_ = nameOf(act.Get());
            hardware_ = hw && fresh->manager != nullptr;
            // Hardware encoders without a device manager are fed through memory like software ones.
            if (!fresh->manager && !fresh->staging) {
                D3D11_TEXTURE2D_DESC td{};
                fresh->slots[0]->GetDesc(&td);
                td.Usage = D3D11_USAGE_STAGING;
                td.BindFlags = 0;
                td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                gpu.device()->CreateTexture2D(&td, nullptr, &fresh->staging);
            }
            impl_->thread = std::thread([this] {
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                if (impl_->async) impl_->runAsync();
                else impl_->runSync();
                CoUninitialize();
                impl_->threadDone = true;
            });
            return true;
        }
        act->ShutdownObject();
        delete fresh;
    }
    error = why;
    close();
    return false;
}

void VideoEncoder::submit(ID3D11Texture2D* canvas, std::int64_t pts) {
    Impl* i = impl_;
    if (!i || !i->vproc) return;
    {
        std::lock_guard<std::mutex> l(i->m);
        if (i->frames.size() >= kQueue) {   // the encoder is behind: drop this frame
            ++dropped_;
            return;
        }
    }
    // The input view follows the canvas texture (it changes when the canvas is resized).
    if (i->viewOf != canvas) {
        i->inView.Reset();
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
        iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        if (FAILED(i->vdev->CreateVideoProcessorInputView(canvas, i->venum.Get(), &iv, &i->inView))) return;
        i->viewOf = canvas;
    }
    const int slot = i->nextSlot;
    i->nextSlot = (i->nextSlot + 1) % kSlots;
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = i->inView.Get();
    if (FAILED(i->vctx->VideoProcessorBlt(i->vproc.Get(), i->outViews[slot].Get(), 0, 1, &stream))) {
        ++dropped_;
        return;
    }
    {
        std::lock_guard<std::mutex> l(i->m);
        i->frames.push_back({slot, pts});
    }
    i->cv.notify_one();
}

bool VideoEncoder::take(media::Packet& out) {
    Impl* i = impl_;
    if (!i) return false;
    std::lock_guard<std::mutex> l(i->pm);
    if (i->packets.empty()) return false;
    out = std::move(i->packets.front());
    i->packets.pop_front();
    return true;
}

std::wstring VideoEncoder::error() const {
    if (!impl_) return {};
    std::lock_guard<std::mutex> l(impl_->em);
    return impl_->error;
}

void VideoEncoder::finish() {
    Impl* i = impl_;
    if (!i || i->finished) return;
    i->finished = true;
    {
        std::lock_guard<std::mutex> l(i->m);
        i->draining = true;
    }
    i->cv.notify_all();
    if (i->thread.joinable()) {
        // An async encoder may be waiting for its next event; give it time to drain, then shut it.
        const ULONGLONG until = GetTickCount64() + 3000;
        while (GetTickCount64() < until && !i->threadDone) Sleep(10);
        i->quit = true;
        i->cv.notify_all();
        if (!i->threadDone && i->events) {
            ComPtr<IMFShutdown> sd;
            if (SUCCEEDED(i->mft.As(&sd))) sd->Shutdown();   // releases a waiting GetEvent
        }
        i->thread.join();
    }
    if (i->mft) {
        i->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        i->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
}

void VideoEncoder::close() {
    if (!impl_) return;
    finish();
    delete impl_;
    impl_ = nullptr;
}

}  // namespace ncap::encode
