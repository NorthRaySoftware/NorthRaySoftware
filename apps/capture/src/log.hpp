// NorthRay Capture's log (system 15): plain text in %LOCALAPPDATA%\NorthRay\Capture\logs, one file a
// day ("capture-2026-11-14.log"), at most 8 MB a day, 14 days kept. Every line goes through
// ncap::redact first, with the stream keys the app holds, so no key or token is ever written.
// Safe from any thread.
#pragma once

#include <string>
#include <vector>

namespace ncap::log {

void start(const std::wstring& folder);
void info(const std::string& message);
void warn(const std::string& message);
void error(const std::string& message);
// Secrets to hide wherever they appear (stream keys as they're loaded from Credential Manager).
void addSecret(const std::string& secret);
std::wstring folder();
std::wstring todaysFile();

}  // namespace ncap::log
