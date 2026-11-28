cmake_minimum_required(VERSION 3.20)

project(search_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)

find_package(Threads REQUIRED)

include(FetchContent)

# cpp-httplib v0.67.0 auto-detects OpenSSL, zlib, Brotli and zstd (all ON by
# default). Disable them so a native macOS build (e.g. with Homebrew OpenSSL)
# and the Docker build produce the same binary features. Must be set before
# FetchContent_MakeAvailable.
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.67.0
  GIT_SHALLOW TRUE)
# The release tarball is pinned to the v3.12.0 tag and avoids a full clone of
# the nlohmann/json repository on every Docker build.
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
FetchContent_MakeAvailable(httplib nlohmann_json)

# Options shared by every target. The WebSocket read timeout macro must have
# the same value in every translation unit that includes httplib.h, otherwise
# different .cc files would violate the ODR. Setting it here, once, avoids a
# #define in any source file.
add_library(search_build_options INTERFACE)
target_compile_definitions(search_build_options INTERFACE
  CPPHTTPLIB_WEBSOCKET_SERVER_READ_TIMEOUT_SECOND=60)
target_link_libraries(search_build_options INTERFACE Threads::Threads)

if(ENABLE_TSAN)
  target_compile_options(search_build_options INTERFACE
    -fsanitize=thread -g -O1)
  target_link_options(search_build_options INTERFACE -fsanitize=thread)
endif()

add_library(search_core STATIC
  src/config.cc src/logger.cc src/tokenizer.cc
  src/striped_hash_table.cc src/document_table.cc src/inverted_index.cc
  src/snippet.cc src/ranker.cc src/ollama_client.cc)
target_include_directories(search_core PUBLIC src)
target_link_libraries(search_core PUBLIC
  search_build_options httplib::httplib nlohmann_json::nlohmann_json)

add_executable(search_server src/main.cc)
target_link_libraries(search_server PRIVATE search_core)

add_executable(index_stress_test tests/index_stress_test.cc)
target_link_libraries(index_stress_test PRIVATE search_core)
