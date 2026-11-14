// NorthRay Capture: stream keys kept in Windows Credential Manager (generic credentials, per user),
// never in settings files or logs.
#pragma once

#include <string>

namespace ncap::secrets {

// `name`: "stream/<destination>"; stored as "NorthRayCapture/<name>".
bool save(const std::string& name, const std::string& secret);
std::string load(const std::string& name);   // "" if there's none
bool exists(const std::string& name);
bool remove(const std::string& name);

}  // namespace ncap::secrets
