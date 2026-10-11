cmake_minimum_required(VERSION 3.25)
project(tsengine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TSENGINE_SANITIZE "Build with ASan + UBSan" OFF)
option(TSENGINE_BUILD_TESTS "Build unit tests" ON)
option(TSENGINE_BUILD_SERVER "Build the HTTP server" ON)

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
  src/api.cpp
  src/block_file.cpp
  src/codec.cpp
  src/crc32c.cpp
  src/engine.cpp
  src/file_util.cpp
  src/line_protocol.cpp
  src/manifest.cpp
  src/memtable.cpp
  src/query.cpp
  src/series_index.cpp
  src/version.cpp
  src/wal.cpp
)
target_include_directories(tsengine PUBLIC include)
target_link_libraries(tsengine PRIVATE tsengine_options)

if(TSENGINE_BUILD_SERVER)
  # cpp-httplib is a single-header HTTP library. It is only used by the server front end;
  # the storage engine itself has no dependencies.
  include(FetchContent)
  set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
  set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.18.3
    SYSTEM
  )
  FetchContent_MakeAvailable(httplib)

  add_library(tsengine_http
    src/server.cpp
  )
  target_link_libraries(tsengine_http PUBLIC tsengine PRIVATE tsengine_options httplib::httplib)

  add_executable(tsengine_server
    src/main.cpp
  )
  target_link_libraries(tsengine_server PRIVATE tsengine_http tsengine_options)
endif()

if(TSENGINE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
