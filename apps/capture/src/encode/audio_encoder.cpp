#include "encode/audio_encoder.hpp"

#include "ncap/audio.hpp"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ncap::encode {

using Microsoft::WRL::ComPtr;

bool AudioEncoder::open(int kbps, std::wstring& error) {
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Audio, MFAudioFormat_PCM}, out{MFMediaType_Audio, MFAudioFormat_AAC};
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out, &acts, &n)) || !acts || !n) {
        if (acts) CoTaskMemFree(acts);
        error = L"Windows' AAC audio encoder wasn't found (it's part of Media Foundation; N editions of Windows need the Media Feature Pack).";
        return false;
    }
    HRESULT hr = acts[0]->ActivateObject(IID_PPV_ARGS(&mft_));
    for (UINT32 i = 0; i < n; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
    if (FAILED(hr)) {
        error = L"The AAC encoder couldn't be started.";
        return false;
    }
    // Windows' encoder takes 96, 128, 160 or 192 kbit/s (as bytes per second).
    const int allowed[] = {96, 128, 160, 192};
    int rate = 160;
    for (int a : allowed)
        if (kbps >= a) rate = a;
    ComPtr<IMFMediaType> o, i;
    MFCreateMediaType(&o);
    o->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    o->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    o->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    o->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, audio::kRate);
    o->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, audio::kChannels);
    o->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)(rate * 1000 / 8));
    o->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);   // raw frames
    o->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
    MFCreateMediaType(&i);
    i->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    i->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    i->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    i->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, audio::kRate);
    i->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, audio::kChannels);
    i->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, audio::kChannels * 2);
    i->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, audio::kRate * audio::kChannels * 2);
    if (FAILED(hr = mft_->SetOutputType(0, o.Get(), 0)) || FAILED(hr = mft_->SetInputType(0, i.Get(), 0))) {
        error = L"The AAC encoder didn't take 48 kHz stereo.";
        mft_.Reset();
        return false;
    }
    MFT_OUTPUT_STREAM_INFO info{};
    mft_->GetOutputStreamInfo(0, &info);
    bufferSize_ = std::max<DWORD>(info.cbSize, 8192);
    config_ = media::aacConfig(audio::kRate, audio::kChannels);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

media::Bytes AudioEncoder::config() const { return config_; }

bool AudioEncoder::encode(const float* frames, std::size_t count, std::int64_t ptsUs) {
    if (!mft_) return false;
    if (!started_) {
        started_ = true;
        firstUs_ = ptsUs;
    }
    const std::size_t samples = count * audio::kChannels;
    pcm_.resize(samples);
    for (std::size_t k = 0; k < samples; ++k) pcm_[k] = (std::int16_t)std::lround(std::clamp(frames[k], -1.0f, 1.0f) * 32767.0f);
    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buf;
    BYTE* d = nullptr;
    const DWORD bytes = (DWORD)(samples * 2);
    if (FAILED(MFCreateSample(&sample)) || FAILED(MFCreateMemoryBuffer(bytes, &buf)) || FAILED(buf->Lock(&d, nullptr, nullptr))) return false;
    std::memcpy(d, pcm_.data(), bytes);
    buf->Unlock();
    buf->SetCurrentLength(bytes);
    sample->AddBuffer(buf.Get());
    // Time by counting samples from the first, so it's exact and never drifts.
    sample->SetSampleTime((firstUs_ + samplesIn_ * 1000000 / audio::kRate) * 10);
    sample->SetSampleDuration((LONGLONG)count * 10000000 / audio::kRate);
    samplesIn_ += (std::int64_t)count;
    HRESULT hr = mft_->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        pull(0);
        hr = mft_->ProcessInput(0, sample.Get(), 0);
    }
    return SUCCEEDED(hr);
}

bool AudioEncoder::pull(int stream) {
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER out{};
        ComPtr<IMFSample> s;
        ComPtr<IMFMediaBuffer> b;
        MFCreateSample(&s);
        MFCreateMemoryBuffer(bufferSize_, &b);
        s->AddBuffer(b.Get());
        out.pSample = s.Get();
        DWORD status = 0;
        const HRESULT hr = mft_->ProcessOutput(0, 1, &out, &status);
        if (out.pEvents) out.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
        if (FAILED(hr)) return false;
        BYTE* d = nullptr;
        DWORD len = 0;
        if (SUCCEEDED(b->Lock(&d, nullptr, &len))) {
            media::Packet p;
            p.stream = stream;
            LONGLONG t = 0;
            s->GetSampleTime(&t);
            p.pts = p.dts = t / 10;
            p.keyframe = true;
            p.data.assign(d, d + len);
            b->Unlock();
            if (!p.data.empty()) out_.push_back(std::move(p));
        }
    }
}

bool AudioEncoder::finish() {
    if (!mft_) return false;
    mft_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    return pull(0);
}

bool AudioEncoder::take(media::Packet& out, int stream) {
    if (mft_) pull(stream);
    if (out_.empty()) return false;
    out = std::move(out_.front());
    out_.pop_front();
    out.stream = stream;
    return true;
}

}  // namespace ncap::encode
