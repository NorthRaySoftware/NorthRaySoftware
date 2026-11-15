#include "audio/soundboard_player.hpp"

#include "log.hpp"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <map>

namespace ncap::audio {

using Microsoft::WRL::ComPtr;

namespace {

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// A sound file as 48 kHz stereo floats (Media Foundation converts the format and the rate).
std::vector<float> decode(const std::wstring& path, std::wstring& error) {
    std::vector<float> out;
    ComPtr<IMFSourceReader> r;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &r))) {
        error = L"the file can't be opened or isn't a sound Windows can play";
        return out;
    }
    r->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    r->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    ComPtr<IMFMediaType> t;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, soundboard::kRate);
    t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
    t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 8);
    t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, soundboard::kRate * 8);
    if (FAILED(r->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, t.Get()))) {
        error = L"the file has no sound Windows can convert";
        return out;
    }
    const std::size_t maxFloats = (std::size_t)soundboard::kRate * 2 * 30;   // 30 seconds
    while (out.size() < maxFloats) {
        DWORD flags = 0;
        ComPtr<IMFSample> s;
        if (FAILED(r->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &s)) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
        if (!s) continue;
        ComPtr<IMFMediaBuffer> b;
        BYTE* data = nullptr;
        DWORD len = 0;
        if (FAILED(s->ConvertToContiguousBuffer(&b)) || FAILED(b->Lock(&data, nullptr, &len))) continue;
        const float* f = reinterpret_cast<const float*>(data);
        out.insert(out.end(), f, f + std::min<std::size_t>(len / sizeof(float), maxFloats - out.size()));
        b->Unlock();
    }
    out.resize(out.size() / 2 * 2);
    if (out.empty()) error = L"the file has no sound in it";
    return out;
}

}  // namespace

SoundboardPlayer::SoundboardPlayer() : ring_(std::make_shared<Ring>(kRate / 2)) {
    thread_ = std::thread([this] { run(); });
}

SoundboardPlayer::~SoundboardPlayer() {
    quit_ = true;
    if (thread_.joinable()) thread_.join();
    if (loader_.joinable()) loader_.join();
}

// Keeps about 20 ms of the mixed sounds (or silence) in the ring, a block of 5 ms at a time.
void SoundboardPlayer::run() {
    const std::size_t block = kRate / 200, keep = kRate / 50;
    std::vector<float> buf(block * 2);
    while (!quit_) {
        while (ring_->available() < keep && !quit_) {
            std::fill(buf.begin(), buf.end(), 0.0f);
            voices_.mix(buf.data(), block);
            ring_->write(buf.data(), block);
        }
        Sleep(2);
    }
}

void SoundboardPlayer::setSounds(const std::vector<SoundboardSound>& sounds) {
    {
        std::lock_guard<std::mutex> l(m_);
        if (sounds == sounds_) return;
        // Keep what's already loaded for sounds that stay the same.
        std::map<std::string, soundboard::Voices::Pcm> have;
        for (std::size_t i = 0; i < sounds_.size() && i < pcm_.size(); ++i)
            if (pcm_[i]) have[sounds_[i].starter + "|" + sounds_[i].file] = pcm_[i];
        sounds_ = sounds;
        pcm_.assign(sounds.size(), nullptr);
        problems_.assign(sounds.size(), L"");
        for (std::size_t i = 0; i < sounds.size(); ++i) {
            auto it = have.find(sounds[i].starter + "|" + sounds[i].file);
            if (it != have.end()) pcm_[i] = it->second;
        }
    }
    const unsigned gen = ++generation_;
    if (loader_.joinable()) loader_.join();
    loader_ = std::thread([this, gen] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        MFStartup(MF_VERSION, MFSTARTUP_LITE);
        for (std::size_t i = 0;; ++i) {
            SoundboardSound s;
            {
                std::lock_guard<std::mutex> l(m_);
                if (gen != generation_ || i >= sounds_.size()) break;
                if (pcm_[i]) continue;
                s = sounds_[i];
            }
            std::wstring error;
            std::vector<float> pcm = !s.starter.empty() ? soundboard::synth(s.starter) : decode(wide(s.file), error);
            if (pcm.empty() && error.empty()) error = L"it isn't a sound Capture knows";
            std::lock_guard<std::mutex> l(m_);
            if (gen != generation_ || i >= sounds_.size()) break;
            if (!pcm.empty()) pcm_[i] = std::make_shared<const std::vector<float>>(std::move(pcm));
            problems_[i] = error;
            if (!error.empty()) log::info("soundboard: a sound couldn't be loaded");
        }
        MFShutdown();
        CoUninitialize();
    });
}

void SoundboardPlayer::play(std::size_t i) {
    soundboard::Voices::Pcm p;
    {
        std::lock_guard<std::mutex> l(m_);
        if (i < pcm_.size()) p = pcm_[i];
    }
    voices_.play(p);
}

std::wstring SoundboardPlayer::problem(std::size_t i) const {
    std::lock_guard<std::mutex> l(m_);
    return i < problems_.size() ? problems_[i] : L"";
}

}  // namespace ncap::audio
