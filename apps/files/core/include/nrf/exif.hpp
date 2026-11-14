// NorthRay Files: the EXIF orientation of a JPEG (how a camera or phone says
// "turn me this way up"), read from the first bytes of the file. Portable, so
// it's tested without Windows; used when Windows' own metadata reader doesn't
// give an answer.
#pragma once

#include <cstddef>
#include <vector>

namespace nrf {

// 1-8 as in EXIF (1 = as stored). 1 when there's no tag, or the data isn't a
// JPEG, or anything about it is out of bounds.
int jpegOrientation(const unsigned char* data, std::size_t size);

// Turns 4-byte pixels (w by h, rows packed) the right way up for `orientation`.
// Orientations 5-8 swap the width and height.
void applyOrientation(std::vector<unsigned char>& px, int& w, int& h, int orientation);

}  // namespace nrf
