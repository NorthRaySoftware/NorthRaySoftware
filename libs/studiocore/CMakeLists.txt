# Writing & Publishing Studio core: portable, no Windows UI code.
enable_language(C)

add_library(sqlite3 STATIC ${CMAKE_SOURCE_DIR}/libs/third_party/sqlite3/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${CMAKE_SOURCE_DIR}/libs/third_party/sqlite3)
# Safe defaults for a desktop app: no extension loading, foreign keys on by default.
target_compile_definitions(sqlite3 PRIVATE SQLITE_OMIT_LOAD_EXTENSION SQLITE_DEFAULT_FOREIGN_KEYS=1
                                            SQLITE_DQS=0 SQLITE_THREADSAFE=1 SQLITE_DEFAULT_WAL_SYNCHRONOUS=2)
if(NOT MSVC)
  target_compile_options(sqlite3 PRIVATE -w)
endif()
if(UNIX)
  find_package(Threads REQUIRED)
  target_link_libraries(sqlite3 PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

add_library(miniz STATIC ${CMAKE_SOURCE_DIR}/libs/third_party/miniz/miniz.c ${CMAKE_SOURCE_DIR}/libs/third_party/miniz/miniz_tdef.c
                         ${CMAKE_SOURCE_DIR}/libs/third_party/miniz/miniz_tinfl.c ${CMAKE_SOURCE_DIR}/libs/third_party/miniz/miniz_zip.c)
target_include_directories(miniz PUBLIC ${CMAKE_SOURCE_DIR}/libs/third_party/miniz)
# Reading only: no file I/O inside miniz (we pass it memory), no writing archives.
target_compile_definitions(miniz PUBLIC MINIZ_NO_STDIO MINIZ_NO_ARCHIVE_WRITING_APIS MINIZ_NO_TIME)
if(NOT MSVC)
  target_compile_options(miniz PRIVATE -w)
endif()

# Typesetting: HarfBuzz (shaping) and FreeType (outlines, rasterising), both permissively licensed.
add_subdirectory(${CMAKE_SOURCE_DIR}/libs/third_party/freetype ${CMAKE_BINARY_DIR}/third_party/freetype)
add_subdirectory(${CMAKE_SOURCE_DIR}/libs/third_party/harfbuzz ${CMAKE_BINARY_DIR}/third_party/harfbuzz)

add_library(studiocore STATIC src/library.cpp src/manuscript.cpp src/document.cpp src/markup.cpp src/import.cpp
                              src/text.cpp src/pdf.cpp src/docx.cpp src/artwork.cpp
                              src/typeset.cpp src/typeset_fonts.cpp src/typeset_hyphen.cpp src/typeset_output.cpp src/ebook.cpp
                              src/publish.cpp src/editions.cpp src/entries.cpp src/review.cpp src/comments.cpp)
target_include_directories(studiocore PUBLIC include PRIVATE ${CMAKE_SOURCE_DIR}/libs/third_party)
target_link_libraries(studiocore PUBLIC sqlite3 PRIVATE miniz harfbuzz freetype)
if(MSVC)
  target_compile_options(studiocore PRIVATE /W4 /utf-8)
else()
  target_compile_options(studiocore PRIVATE -Wall -Wextra)
endif()
