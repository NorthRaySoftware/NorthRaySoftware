#include "ncap/profiles.hpp"

#include "ncap/media.hpp"

#include <algorithm>
#include <cctype>

namespace ncap::profiles {

namespace {
std::string upper(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}
}  // namespace

bool validName(const std::string& n, const std::vector<std::string>& others, std::string& why) {
    if (n.empty() || n.find_first_not_of(' ') == std::string::npos) {
        why = "type a name";
        return false;
    }
    if (n.size() > 40) {
        why = "keep it to 40 characters";
        return false;
    }
    for (unsigned char c : n)
        if (c < 32 || std::string("\\/:*?\"<>|").find((char)c) != std::string::npos) {
            why = "it can't contain \\ / : * ? \" < > |";
            return false;
        }
    if (n.back() == '.' || n.back() == ' ' || n.front() == ' ') {
        why = "it can't start with a space or end with a dot or space";
        return false;
    }
    const std::string u = upper(n.substr(0, n.find('.')));
    static const char* kReserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
                                      "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    for (auto r : kReserved)
        if (u == r) {
            why = "Windows keeps that name for itself";
            return false;
        }
    for (auto& o : others)
        if (upper(o) == upper(n)) {
            why = "there's one called that already";
            return false;
        }
    return true;
}

std::string uniqueName(const std::string& base, const std::vector<std::string>& existing) {
    auto taken = [&](const std::string& n) {
        return std::any_of(existing.begin(), existing.end(), [&](const std::string& e) { return upper(e) == upper(n); });
    };
    if (!taken(base)) return base;
    for (int i = 2; i < 1000; ++i)
        if (!taken(base + " " + std::to_string(i))) return base + " " + std::to_string(i);
    return base + " copy";
}

std::string recordingName(const std::string& profile, int y, int mo, int d, int h, int mi, int s) {
    const std::string when = media::recordingName(y, mo, d, h, mi, s);
    return (profile.empty() || upper(profile) == "DEFAULT" ? std::string("Recording") : profile) + " " + when;
}

}  // namespace ncap::profiles
