# PDFium

`pdfium.dll` is the Windows x64 build of [PDFium](https://pdfium.googlesource.com/pdfium/) from
[bblanchon/pdfium-binaries](https://github.com/bblanchon/pdfium-binaries) (release "latest" as of 13 November 2026,
PDFium 159.0.8152.0). The Writing Studio loads it at run time for PDF import (`libs/studiocore/src/pdf.cpp`);
nothing links against it.

Licence: BSD-3-Clause / Apache-2.0 (`LICENSE.txt`), plus the permissive licences of the libraries it bundles
(`licenses/`: FreeType, libjpeg-turbo, libpng, OpenJPEG, Little CMS, zlib, ICU, HarfBuzz, Abseil and others).
