// Blocking ads and trackers: requests a page makes to well-known advertising and tracking services
// (a list written for NorthRay Browser, built in) are refused when they come from another site than
// the page's own. A site can be let off (its exceptions are kept in the settings).
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace nrb {

// The services blocked: domains (a request to the domain or any of its subdomains is blocked).
const std::vector<std::string>& trackerDomains();

// The site a host belongs to (its registrable domain): "news.bbc.co.uk" -> "bbc.co.uk",
// "a.b.example.com" -> "example.com". IP addresses and single names are their own site.
std::string siteOf(std::string_view host);

// The host is (or is under) one of the blocked services.
bool isTrackerHost(std::string_view host);

// A request from a page: blocked when it goes to a tracker on another site than the page's.
// Both are full addresses ("https://...").
bool shouldBlock(std::string_view requestUrl, std::string_view pageUrl);

// The engine's request filters for the list ("*://*.doubleclick.net/*" and "*://doubleclick.net/*"),
// so only requests that might be blocked are looked at.
std::vector<std::string> blockerFilters();

}  // namespace nrb
