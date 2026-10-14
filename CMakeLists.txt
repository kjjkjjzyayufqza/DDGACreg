cmake_minimum_required(VERSION 3.24)
project(DDGACreg LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "DDGACreg builds for x64 only.")
endif()

add_subdirectory(third_party/minhook)
add_subdirectory(dll)
add_subdirectory(server)
