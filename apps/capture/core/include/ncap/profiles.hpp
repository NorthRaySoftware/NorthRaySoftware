// NorthRay Capture: names for profiles (sets of settings: frame rate, encoders, bitrates, recording,
// streaming destinations) and scene collections, which are files named after them, and recording
// names ("<profile> 2026-11-15 14-00-05"). Portable and unit-tested.
#pragma once

#include <string>
#include <vector>

namespace ncap::profiles {

// A name that can be a file name: 1-40 characters, none of \ / : * ? " < > | or control characters,
// not ending in a dot or space, not one of Windows' reserved names (CON, NUL, COM1...), and not the
// same as another (ignoring case). `why` says what's wrong, in words.
bool validName(const std::string& name, const std::vector<std::string>& others, std::string& why);
// "Streaming" -> "Streaming 2" (or 3...) if taken (ignoring case).
std::string uniqueName(const std::string& base, const std::vector<std::string>& existing);
// A recording's name: "<profile> 2026-11-15 14-00-05" ("Recording ..." for the Default profile).
std::string recordingName(const std::string& profile, int year, int month, int day, int hour, int minute, int second);

}  // namespace ncap::profiles
