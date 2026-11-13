// Reading settings and other JSON written by other versions or other apps:
// a value of the wrong type falls back to the default instead of throwing
// (nlohmann's value() throws, which closed apps on start).
#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace cj {

template <class T>
T valueOr(const nlohmann::json& j, const char* key, T def) {
    if (!j.is_object()) return def;
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    try {
        if constexpr (std::is_same_v<T, bool>) {
            if (!it->is_boolean()) return def;
        } else if constexpr (std::is_arithmetic_v<T>) {
            if (!it->is_number()) return def;
        } else if constexpr (std::is_same_v<T, std::string>) {
            if (!it->is_string()) return def;
        }
        return it->template get<T>();
    } catch (...) {
        return def;
    }
}
inline std::string valueOr(const nlohmann::json& j, const char* key, const char* def) { return valueOr<std::string>(j, key, std::string(def)); }

}  // namespace cj
