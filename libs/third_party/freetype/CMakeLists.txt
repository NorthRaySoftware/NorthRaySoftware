# FreeType 2.13.3, trimmed to what the Studio's typesetter uses. FreeType
# Licence (BSD-style with credit), see FTL.TXT. Module list: include/freetype/config/ftmodule.h.
add_library(freetype STATIC
  src/base/ftsystem.c src/base/ftinit.c src/base/ftdebug.c src/base/ftbase.c src/base/ftbitmap.c
  src/base/ftglyph.c src/base/ftmm.c
  src/truetype/truetype.c src/sfnt/sfnt.c src/smooth/smooth.c src/psnames/psnames.c)
target_include_directories(freetype PUBLIC include)
target_compile_definitions(freetype PRIVATE FT2_BUILD_LIBRARY)
if(NOT MSVC)
  target_compile_options(freetype PRIVATE -w)
endif()
