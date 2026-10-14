cmake_minimum_required(VERSION 3.24)

# Local content server. SQLite is the public-domain amalgamation in
# third_party/sqlite (3.54.0). No system SQLite package is required.
add_library(ddgac_sqlite STATIC
    third_party/sqlite/sqlite3.c)
target_include_directories(ddgac_sqlite PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite")
target_compile_definitions(ddgac_sqlite PUBLIC SQLITE_OMIT_LOAD_EXTENSION)
if(MSVC)
    target_compile_options(ddgac_sqlite PRIVATE /W0)
    set_target_properties(ddgac_sqlite PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(ddgac-content
    src/main.cpp
    src/content_db.cpp)
target_include_directories(ddgac-content PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(ddgac-content PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(ddgac-content PRIVATE ddgac_sqlite ws2_32)
if(MSVC)
    target_compile_options(ddgac-content PRIVATE /W4 /EHsc /utf-8)
    set_target_properties(ddgac-content PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
