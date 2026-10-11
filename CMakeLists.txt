cmake_minimum_required(VERSION 3.25)
project(tsengine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TSENGINE_SANITIZE "Build with ASan + UBSan" OFF)
option(TSENGINE_BUILD_TESTS "Build unit tests" ON)

# Warning and sanitizer flags for our own targets only (not third-party code).
add_library(tsengine_options INTERFACE)
target_compile_options(tsengine_options INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)
if(TSENGINE_SANITIZE)
  target_compile_options(tsengine_options INTERFACE
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  target_link_options(tsengine_options INTERFACE -fsanitize=address,undefined)
endif()

add_library(tsengine
  src/crc32c.cpp
  src/file_util.cpp
  src/line_protocol.cpp
  src/version.cpp
  src/wal.cpp
)
target_include_directories(tsengine PUBLIC include)
target_link_libraries(tsengine PRIVATE tsengine_options)

if(TSENGINE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
