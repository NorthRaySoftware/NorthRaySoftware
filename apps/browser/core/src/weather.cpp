#include "nrb/weather.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cmath>
#include <cstdio>

namespace nrb {

using nlohmann::json;

namespace {

std::string encode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else if (c == ' ') out += "%20";
        else { char b[4]; std::snprintf(b, sizeof b, "%%%02X", c); out += b; }
    }
    return out;
}

double num(const json& j, const char* key, double fallback = 0) {
    if (!j.is_object() || !j.contains(key)) return fallback;
    const json& v = j[key];
    if (v.is_number()) return v.get<double>();
    if (v.is_array() && !v.empty() && v[0].is_number()) return v[0].get<double>();
    return fallback;
}

}  // namespace

std::string Place::label() const {
    std::string s = name;
    if (!region.empty() && region != name) s += ", " + region;
    if (!country.empty()) s += ", " + country;
    return s;
}

std::string placeSearchUrl(const std::string& name) {
    return "https://geocoding-api.open-meteo.com/v1/search?name=" + encode(name) + "&count=6&language=en&format=json";
}

std::string forecastUrl(const Place& p) {
    char b[320];
    std::snprintf(b, sizeof b,
                  "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
                  "&current=temperature_2m,weather_code,wind_speed_10m,is_day&daily=temperature_2m_max,temperature_2m_min"
                  "&timezone=auto&forecast_days=1",
                  p.latitude, p.longitude);
    return b;
}

std::vector<Place> parsePlaces(const std::string& text) {
    std::vector<Place> out;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("results") || !j["results"].is_array()) return out;
    for (const json& r : j["results"]) {
        if (!r.is_object() || !r.contains("latitude") || !r.contains("longitude")) continue;
        Place p;
        p.name = r.value("name", std::string());
        p.region = r.value("admin1", std::string());
        p.country = r.value("country", std::string());
        p.latitude = num(r, "latitude");
        p.longitude = num(r, "longitude");
        if (!p.name.empty()) out.push_back(p);
    }
    return out;
}

Weather parseForecast(const std::string& text) {
    Weather w;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("current") || !j["current"].is_object()) return w;
    const json& c = j["current"];
    if (!c.contains("temperature_2m") || !c.contains("weather_code")) return w;
    w.temperature = num(c, "temperature_2m");
    w.code = (int)num(c, "weather_code", -1);
    w.wind = num(c, "wind_speed_10m");
    w.day = num(c, "is_day", 1) != 0;
    if (j.contains("daily")) {
        w.high = num(j["daily"], "temperature_2m_max", w.temperature);
        w.low = num(j["daily"], "temperature_2m_min", w.temperature);
    } else w.high = w.low = w.temperature;
    w.ok = true;
    return w;
}

std::string weatherWords(int code) {
    switch (code) {
        case 0: return "Clear";
        case 1: return "Mainly clear";
        case 2: return "Partly cloudy";
        case 3: return "Overcast";
        case 45: case 48: return "Fog";
        case 51: return "Light drizzle";
        case 53: return "Drizzle";
        case 55: return "Heavy drizzle";
        case 56: case 57: return "Freezing drizzle";
        case 61: return "Light rain";
        case 63: return "Rain";
        case 65: return "Heavy rain";
        case 66: case 67: return "Freezing rain";
        case 71: return "Light snow";
        case 73: return "Snow";
        case 75: return "Heavy snow";
        case 77: return "Snow grains";
        case 80: return "Light showers";
        case 81: return "Showers";
        case 82: return "Heavy showers";
        case 85: case 86: return "Snow showers";
        case 95: return "Thunderstorm";
        case 96: case 99: return "Thunderstorm with hail";
        default: return "Unknown";
    }
}

std::string Weather::words() const { return weatherWords(code); }

std::string temperatureText(double c, bool f) {
    char b[16];
    std::snprintf(b, sizeof b, "%d\xC2\xB0%s", (int)std::lround(f ? c * 9 / 5 + 32 : c), f ? "F" : "C");
    return b;
}

}  // namespace nrb
