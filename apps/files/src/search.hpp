// NorthRay Files: searching (0.4), on worker threads, results posted to the window as ListBatch
// messages (WM_APP_LIST) like a folder's listing, each entry with `location` set to its folder.
//
// Two ways to look:
//  - reading the folders: the folder and everything inside it, as deep as it goes (links aren't
//    followed, so a junction can't send it round in circles). Always works; as fast as the disk.
//  - the Windows Search index (the one File Explorer's search box uses): instant across the whole PC,
//    but only for places Windows has indexed, and only when the Windows Search service is running.
//    When it can't be asked, the batch says so (noIndex) and the folders are read instead.
#pragma once

#include "shell.hpp"

#include "nrf/listing.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nrf {

constexpr std::size_t kMostSearchResults = 20000;

// Reads `roots` (folders) and everything in them for names matching `query`.
void searchFoldersAsync(HWND window, std::vector<std::wstring> roots, std::wstring query, Visibility visibility, std::uint64_t request,
                        std::shared_ptr<std::atomic<std::uint64_t>> latest);

// Asks the Windows Search index (within `scope`, or everywhere indexed when it's ""). If the index
// can't be asked, falls back to reading `fallbackRoots` (marking the batches noIndex).
void searchIndexAsync(HWND window, std::wstring scope, std::wstring query, Visibility visibility, std::vector<std::wstring> fallbackRoots,
                      std::uint64_t request, std::shared_ptr<std::atomic<std::uint64_t>> latest);

}  // namespace nrf
