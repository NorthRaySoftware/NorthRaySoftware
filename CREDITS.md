# Credits

**Developed by the NorthRay Software project.** (The name is set in one place, `shared/metadata/brand/identity.json`, and can change before release.)

CJ Philip Community brings together creative applications for writing, publishing and broadcasting. It has a creative association with author and broadcaster CJ Philip, while the software is presented under its own development identity, NorthRay Software. CJ Radio features The CJ Philip Show.

NorthRay Software is a project identity, not a company or a team: credits never list invented people, staff or history. CJ Philip's books, games, characters, worlds and branding remain his own.

## Third-party software

- JSON for Modern C++ by Niels Lohmann, MIT licence (`libs/third_party/nlohmann/LICENSE.MIT`)
- miniaudio by David Reid, public domain or MIT No Attribution (`libs/third_party/miniaudio/LICENSE`)
- Microsoft Edge WebView2 SDK 1.0.961.33 (WebView2Loader.dll and interface definitions), Microsoft Corporation, BSD 3-Clause (`apps/cj-radio/licenses/WebView2-LICENSE.txt`). Twitch's player and embed script are loaded from Twitch at run time and are Twitch's.
- SQLite 3.54.1, public domain (`libs/third_party/sqlite3`), used by the Writing & Publishing Studio
- NSIS (installers), zlib licence; the installers include NSIS's own runtime code
- Astro (website build), MIT licence

## Writing Studio (0.5.0)

- **ProseMirror** (MIT): the manuscript editor, bundled into `apps/studio/web/editor.js`. Licence: `apps/studio/licenses/ProseMirror-LICENSE.txt`.
- **re2js** (MIT, 2.9.0): runs writers' own style-hint patterns in linear time (a JavaScript port of RE2), bundled into `apps/studio/web/editor.js`. Licence: `apps/studio/licenses/re2js-LICENSE.txt`. The style-hint word lists were written for NorthRay; none are copied from other tools.
- **nspell** (MIT, 2.1.5): the Writing Studio's spell checker, bundled into `apps/studio/web/editor.js`. Licence: `apps/studio/licenses/nspell-LICENSE.txt`.
- **SCOWL Hunspell dictionaries** (MIT AND BSD, via dictionary-en-gb 3.0.0 and dictionary-en 4.0.0): British (-ise) and American English word lists in `apps/studio/web/dict/`. Licences: `apps/studio/licenses/dictionary-en-GB-LICENSE.txt`, `dictionary-en-US-LICENSE.txt`.
- **Courier Prime** (SIL Open Font License 1.1, Quote-Unquote Apps): the Writing Studio's comic script typeface. `apps/studio/fonts/courier-prime/` (licence: `OFL.txt` there).
- **Noto Sans Symbols 2, Noto Sans Symbols and Noto Emoji** (SIL Open Font License 1.1, Google): fallback fonts the Writing Studio's typesetter uses for symbols the book font doesn't have. `apps/studio/fonts/symbols/` (licence: `OFL.txt` there).
- **WordNet 3.1** (Princeton University, WordNet licence; files from wordnet-db 3.1.14, MIT packaging): the Writing Studio's thesaurus, boiled down by `tools/build-thesaurus.mjs` into `apps/studio/web/dict/thesaurus.json`. WordNet 3.0 Copyright 2006 by Princeton University. All rights reserved. Licence: `apps/studio/licenses/WordNet.txt`.
- **miniz** (MIT): reading EPUB (ZIP) files. `libs/third_party/miniz/LICENSE`.
- **PDFium** (BSD-3-Clause / Apache-2.0) and the libraries it bundles: PDF import. Windows build from bblanchon/pdfium-binaries. `apps/studio/licenses/PDFium/`.
- **Microsoft Edge WebView2** loader (BSD-style licence from Microsoft): hosts the editor, as in CJ Radio. `apps/studio/licenses/WebView2-LICENSE.txt`.

## Writing Studio typesetting (0.9.0)

- **HarfBuzz** 10.1.0 (Old MIT licence): text shaping. `libs/third_party/harfbuzz/COPYING`, shipped as `licenses/HarfBuzz/`.
- **FreeType** 2.13.3 (FreeType Licence, BSD-style with credit): font outlines and the preview's rasteriser. Portions of this software are copyright © The FreeType Project (www.freetype.org). All rights reserved. `libs/third_party/freetype/FTL.TXT`, shipped as `licenses/FreeType/`.
- **stb_image** 2.32 and **stb_image_write** 1.18 by Sean Barrett (public domain / MIT): decoding pictures for typeset pages and PDFs, and writing the cropped pictures in EPUBs. `libs/third_party/stb/LICENSE`.
- **Hyphenation patterns** from hyph-utf8: British English (Dominik Wujastyk, Graham Toal; MIT) and US English (Gerard D.C. Kuiken; free to copy with notice). `apps/studio/hyphenation/*.LICENCE.txt`. The hyphenation algorithm (Liang's) is our own code.
- **Fonts** (SIL Open Font License 1.1): Crimson Text (The Crimson Text Project Authors) and Gentium Book Plus (SIL International; reserved font names "Gentium" and "SIL"). Licences in `apps/studio/fonts/*/OFL.txt`. The Open Font License allows embedding these fonts in the PDFs the Studio exports.
