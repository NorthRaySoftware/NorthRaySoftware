// The start page's weather (optional, off until a place is chosen): today's weather for one place
// from Open-Meteo (open-meteo.com, free, no account; its data is CC BY 4.0 and credited on the page).
// The requests and answers are built and read here; the window fetches them.
#pragma once

#include <string>
#include <vector>

namespace nrb {

struct Place {
    std::string name, region, country;
    double latitude = 0, longitude = 0;
    std::string label() const;   // "Glasgow, Scotland, United Kingdom"
};

struct Weather {
    bool ok = false;
    double temperature = 0;      // °C now
    double high = 0, low = 0;    // today's
    double wind = 0;             // km/h
    int code = -1;               // WMO weather code
    bool day = true;
    std::string words() const;   // "Light rain"
};

// Addresses of the two requests.
std::string placeSearchUrl(const std::string& name);
std::string forecastUrl(const Place& p);

// The answers (JSON); empty / !ok when they can't be read.
std::vector<Place> parsePlaces(const std::string& json);
Weather parseForecast(const std::string& json);

// What a WMO weather code means, in words.
std::string weatherWords(int code);

// "12°C" or "54°F".
std::string temperatureText(double celsius, bool fahrenheit);

}  // namespace nrb
