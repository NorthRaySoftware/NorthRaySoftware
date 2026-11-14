#include "nrb/blocker.hpp"

#include "nrb/url.hpp"

#include <algorithm>
#include <cctype>

namespace nrb {

const std::vector<std::string>& trackerDomains() {
    // Advertising networks, ad exchanges, data brokers, analytics and session recording services that
    // follow people from site to site. Kept to services whose blocking doesn't break the pages that use
    // them (no sign-in buttons, consent tools, video players or content networks).
    static const std::vector<std::string> d = {
        // Google's advertising and analytics
        "doubleclick.net", "googlesyndication.com", "googleadservices.com", "googletagservices.com", "google-analytics.com",
        "googletagmanager.com", "adservice.google.com",
        // Ad exchanges and networks
        "amazon-adsystem.com", "adnxs.com", "adsrvr.org", "criteo.com", "criteo.net", "taboola.com", "outbrain.com", "pubmatic.com",
        "rubiconproject.com", "openx.net", "casalemedia.com", "indexww.com", "smartadserver.com", "adform.net", "yieldmo.com",
        "sharethrough.com", "teads.tv", "33across.com", "lijit.com", "sovrn.com", "media.net", "contextweb.com", "bidswitch.net",
        "mathtag.com", "serving-sys.com", "flashtalking.com", "zemanta.com", "revcontent.com", "mgid.com", "adroll.com", "zedo.com",
        "advertising.com", "adcolony.com", "applovin.com", "inmobi.com", "smaato.net", "3lift.com", "gumgum.com", "undertone.com",
        "spotxchange.com", "springserve.com", "tremorhub.com", "adition.com", "yieldlab.net", "stickyadstv.com", "innovid.com",
        "onetag-sys.com", "a-mo.net", "ads.yahoo.com",
        // Data brokers and identity syncing
        "rlcdn.com", "bluekai.com", "krxd.net", "exelator.com", "demdex.net", "everesttech.net", "liadm.com", "id5-sync.com",
        "crwdcntrl.net", "tapad.com", "agkn.com", "permutive.com", "permutive.app",
        // Ad verification
        "moatads.com", "doubleverify.com", "adsafeprotected.com",
        // Measurement and analytics
        "scorecardresearch.com", "quantserve.com", "quantcount.com", "imrworldwide.com", "chartbeat.com", "chartbeat.net", "parsely.com",
        "mixpanel.com", "segment.io", "heapanalytics.com", "nr-data.net", "statcounter.com", "histats.com", "getclicky.com",
        "cloudflareinsights.com", "analytics.yahoo.com", "mc.yandex.ru", "mc.yandex.com", "hm.baidu.com",
        // Session recording
        "hotjar.com", "mouseflow.com", "crazyegg.com", "fullstory.com", "clarity.ms",
        // Social networks' ad pixels (not their sign-in or share buttons)
        "bat.bing.com", "analytics.twitter.com", "ads-twitter.com", "px.ads.linkedin.com", "snap.licdn.com", "analytics.tiktok.com",
        "ct.pinterest.com", "tr.snapchat.com", "sc-static.net", "alb.reddit.com",
        // App install tracking
        "appsflyer.com", "kochava.com",
    };
    return d;
}

std::string siteOf(std::string_view hostIn) {
    std::string host = toLower(hostIn);
    while (!host.empty() && host.back() == '.') host.pop_back();
    if (host.empty() || host.front() == '[' || std::all_of(host.begin(), host.end(), [](char c) { return std::isdigit((unsigned char)c) || c == '.'; }))
        return host;   // an address, not a name
    std::vector<size_t> dots;
    for (size_t i = 0; i < host.size(); ++i) if (host[i] == '.') dots.push_back(i);
    if (dots.empty()) return host;
    // Two-part public suffixes that are common enough to matter ("co.uk"): the site is one label more.
    static const char* twoPart[] = {"co.uk", "org.uk", "ac.uk", "gov.uk", "me.uk", "ltd.uk", "plc.uk", "net.uk", "sch.uk", "nhs.uk",
                                    "com.au", "net.au", "org.au", "edu.au", "gov.au", "co.nz", "org.nz", "govt.nz", "co.jp", "ne.jp",
                                    "or.jp", "ac.jp", "com.br", "com.cn", "net.cn", "org.cn", "co.in", "net.in", "org.in", "co.za",
                                    "com.mx", "com.tr", "co.kr", "com.sg", "com.hk", "com.tw", "co.il", "com.ar", "co.id", "com.my",
                                    "com.ph", "com.vn", "com.ua", "com.pl", "co.at", "com.es", "com.pt", "com.co", "com.pe",
                                    "github.io", "blogspot.com", "herokuapp.com", "netlify.app", "vercel.app", "pages.dev"};
    const std::string lastTwo = host.substr(dots.size() >= 2 ? dots[dots.size() - 2] + 1 : 0);
    const bool deep = std::any_of(std::begin(twoPart), std::end(twoPart), [&](const char* s) { return lastTwo == s; });
    const size_t keep = deep ? 3 : 2;   // labels in the site
    if (dots.size() + 1 <= keep) return host;
    return host.substr(dots[dots.size() - keep] + 1);
}

bool isTrackerHost(std::string_view hostIn) {
    std::string host = toLower(hostIn);
    while (!host.empty() && host.back() == '.') host.pop_back();
    for (const std::string& d : trackerDomains()) {
        if (host == d) return true;
        if (host.size() > d.size() && host[host.size() - d.size() - 1] == '.' && host.compare(host.size() - d.size(), d.size(), d) == 0) return true;
    }
    return false;
}

bool shouldBlock(std::string_view requestUrl, std::string_view pageUrl) {
    const std::string req = hostOf(requestUrl);
    if (req.empty() || !isTrackerHost(req)) return false;
    const std::string page = hostOf(pageUrl);
    if (page.empty()) return true;   // no page (a new tab): nothing of its own to protect
    return siteOf(req) != siteOf(page);
}

std::vector<std::string> blockerFilters() {
    std::vector<std::string> out;
    for (const std::string& d : trackerDomains()) {
        out.push_back("*://" + d + "/*");
        out.push_back("*://*." + d + "/*");
    }
    return out;
}

}  // namespace nrb
