#include "output/destination.hpp"

#include "output/net.hpp"

#include "ncap/rtmp.hpp"

#include <windows.h>

#include <algorithm>
#include <memory>

namespace ncap::output {

namespace {
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
constexpr std::int64_t kMaxBacklogUs = 3000000;
}  // namespace

void Destination::start(const Settings& s) {
    if (running_) return;
    s_ = s;
    {
        std::lock_guard<std::mutex> l(m_);
        queue_.clear();
        dropped_ = 0;
        st_ = DestinationStatus{};
        st_.state = avcC_.empty() ? DestinationStatus::Waiting : DestinationStatus::Connecting;
        st_.message = avcC_.empty() ? L"Waiting for the first encoded frames…" : L"Connecting…";
    }
    stopping_ = false;
    finished_ = false;
    running_ = true;
    thread_ = std::thread([this] { run(); });
}

void Destination::stop() {
    if (!running_) return;
    stopping_ = true;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    running_ = false;
    std::lock_guard<std::mutex> l(m_);
    if (st_.state != DestinationStatus::Failed) st_.state = DestinationStatus::Off;
    queue_.clear();
}

DestinationStatus Destination::status() const {
    std::lock_guard<std::mutex> l(m_);
    DestinationStatus s = st_;
    s.droppedFrames = dropped_;
    return s;
}

void Destination::configure(const media::Bytes& avcC, const media::Bytes& aacConfig) {
    {
        std::lock_guard<std::mutex> l(m_);
        avcC_ = avcC;
        aac_ = aacConfig;
    }
    cv_.notify_all();
}

void Destination::push(const media::Packet& p) {
    if (!running_ || stopping_) return;
    {
        std::lock_guard<std::mutex> l(m_);
        queue_.push_back(p);
        // Never let this destination fall behind: keep at most ~3 s, dropping from the front up to a keyframe.
        if (queue_.back().dts - queue_.front().dts > kMaxBacklogUs) {
            std::size_t cut = queue_.size();
            for (std::size_t i = queue_.size(); i-- > 1;)
                if (queue_[i].stream == 0 && queue_[i].keyframe) {
                    cut = i;
                    break;
                }
            if (cut == queue_.size()) {   // no keyframe in it at all: drop everything older than 3 s
                cut = 0;
                while (cut < queue_.size() && queue_.back().dts - queue_[cut].dts > kMaxBacklogUs) ++cut;
            }
            if (connected_)
                for (std::size_t i = 0; i < cut; ++i)
                    if (queue_[i].stream == 0) ++dropped_;
            queue_.erase(queue_.begin(), queue_.begin() + (long)cut);
        }
    }
    cv_.notify_all();
}

void Destination::run() {
    rtmp::Url url;
    rtmp::parseUrl(s_.url, url);
    std::unique_ptr<net::Tcp> tcp;
    std::unique_ptr<net::Tls> tls;
    std::unique_ptr<rtmp::Client> client;
    int attempt = 0;
    ULONGLONG retryAt = 0;
    bool needKeyframe = true;
    std::int64_t baseUs = -1;
    double liveBefore = 0;
    ULONGLONG liveSince = 0, rateAt = GetTickCount64();
    std::uint64_t rateBytes = 0, sentBefore = 0;
    const std::wstring name = wide(s_.name);

    auto setState = [&](DestinationStatus::State s, const std::wstring& msg) {
        std::lock_guard<std::mutex> l(m_);
        st_.state = s;
        st_.message = msg;
    };
    auto disconnect = [&](bool polite) {
        if (client) {
            if (polite) client->unpublish(s_.key);
            sentBefore += client->bytesSent();
        }
        client.reset();
        connected_ = false;
        tls.reset();
        tcp.reset();
        if (liveSince) liveBefore += (double)(GetTickCount64() - liveSince) / 1000.0;
        liveSince = 0;
    };
    auto scheduleRetry = [&](const std::wstring& why) {
        ++attempt;
        const int wait = std::min(std::max(2, s_.longestWaitSeconds), 1 << std::min(attempt, 10));   // 2, 4, 8, ...
        retryAt = GetTickCount64() + (ULONGLONG)wait * 1000;
        std::lock_guard<std::mutex> l(m_);
        st_.state = DestinationStatus::Reconnecting;
        st_.message = why;
        st_.attempts = attempt;
    };

    std::deque<media::Packet> batch;
    while (!stopping_) {
        media::Bytes avcC, aac;
        {
            std::unique_lock<std::mutex> l(m_);
            cv_.wait_for(l, std::chrono::milliseconds(client ? 20 : 100), [&] { return stopping_.load() || (!queue_.empty() && client); });
            avcC = avcC_;
            aac = aac_;
            if (client) batch.swap(queue_);
            else if (queue_.size() > 4000) queue_.clear();   // not connected for a long time: nothing old is worth sending
        }
        if (stopping_) break;
        const ULONGLONG now = GetTickCount64();

        if (!client && !avcC.empty() && now >= retryAt) {
            setState(attempt ? DestinationStatus::Reconnecting : DestinationStatus::Connecting, L"Connecting to " + name + L"…");
            const ULONGLONG t0 = GetTickCount64();
            std::string err;
            tcp = std::make_unique<net::Tcp>();
            rtmp::Transport* t = tcp.get();
            bool ok = tcp->open(url.host, url.port, 10000, err);
            if (ok) tcp->setSendTimeout(10000);
            if (ok && url.tls) {
                tls = std::make_unique<net::Tls>(*tcp);
                ok = tls->handshake(url.host, 10000, err);
                t = tls.get();
            }
            if (ok) {
                client = std::make_unique<rtmp::Client>(*t);
                ok = client->publish(url, s_.key, err);
            }
            if (ok)
                ok = client->metadata(s_.width, s_.height, s_.fps, s_.videoKbps, s_.audioKbps) && client->videoConfig(avcC) &&
                     client->audioConfig(aac);
            if (!ok) {
                disconnect(false);
                const bool badKey = err.find("Publishing failed") != std::string::npos || err.find("refused") != std::string::npos;
                if (badKey && attempt == 0) {   // a wrong key or a closed channel: retrying won't help
                    setState(DestinationStatus::Failed, wide(err));
                    break;
                }
                scheduleRetry(wide(err) + L" Trying again soon.");
                continue;
            }
            {
                std::lock_guard<std::mutex> l(m_);
                if (attempt) ++st_.reconnects;
                st_.connectMs = (int)(GetTickCount64() - t0);
                st_.attempts = 0;
                // Start from the newest keyframe already waiting (or the next one to come).
                std::size_t k = queue_.size();
                for (std::size_t i = queue_.size(); i-- > 0;)
                    if (queue_[i].stream == 0 && queue_[i].keyframe) {
                        k = i;
                        break;
                    }
                queue_.erase(queue_.begin(), queue_.begin() + (long)(k == queue_.size() ? queue_.size() : k));
            }
            attempt = 0;
            connected_ = true;
            needKeyframe = true;
            baseUs = -1;
            liveSince = GetTickCount64();
            setState(DestinationStatus::Live, L"Live on " + name);
        }

        if (client) {
            bool ok = true;
            std::string err;
            while (ok && !batch.empty()) {
                media::Packet& q = batch.front();
                if (needKeyframe) {   // a viewer's player can only start at a keyframe
                    if (q.stream == 0 && q.keyframe) needKeyframe = false;
                    else {
                        batch.pop_front();
                        continue;
                    }
                }
                if (baseUs < 0) baseUs = q.dts;
                const std::uint32_t ms = (std::uint32_t)std::max<std::int64_t>(0, (q.dts - baseUs) / 1000);
                ok = q.stream == 0 ? client->video(ms, q.keyframe, q.data) : client->audio(ms, q.data);
                batch.pop_front();
            }
            batch.clear();
            if (ok) ok = client->service(err, 0);
            if (!ok) {
                disconnect(false);
                scheduleRetry((err.empty() ? std::wstring(L"The connection was lost.") : wide(err)) + L" Reconnecting.");
            }
        }

        if (now - rateAt >= 1000) {
            const std::uint64_t sent = sentBefore + (client ? client->bytesSent() : 0);
            std::lock_guard<std::mutex> l(m_);
            st_.kbps = (double)(sent - std::min(sent, rateBytes)) * 8.0 / (double)(now - rateAt);
            st_.bytesSent = sent;
            rateBytes = sent;
            rateAt = now;
            st_.seconds = liveBefore + (liveSince ? (double)(GetTickCount64() - liveSince) / 1000.0 : 0);
            st_.retryIn = client ? 0 : (int)((retryAt > now ? retryAt - now : 0) / 1000);
        }
    }
    disconnect(true);
    std::lock_guard<std::mutex> l(m_);
    if (st_.state != DestinationStatus::Failed) st_.state = DestinationStatus::Off;
    finished_ = true;
}

}  // namespace ncap::output
