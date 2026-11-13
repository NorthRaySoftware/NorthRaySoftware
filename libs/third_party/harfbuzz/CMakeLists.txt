# HarfBuzz 10.1.0 (Old MIT licence, see COPYING), built as one translation
# unit with no optional dependencies: the Studio gives it font data directly.
add_library(harfbuzz STATIC src/harfbuzz-subset.cc)   # all of HarfBuzz, plus subsetting (smaller PDFs)
target_include_directories(harfbuzz PUBLIC src)
target_compile_definitions(harfbuzz PRIVATE HB_NO_MT)
if(NOT MSVC)
  target_compile_options(harfbuzz PRIVATE -w -fno-exceptions -fno-rtti)
endif()
