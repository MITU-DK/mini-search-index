cmake_minimum_required(VERSION 3.28)
project(mini_search_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)  # we use headers, not C++20 modules

# "" (none), "address" (ASan + UBSan) or "thread" (TSan). Set by CMakePresets.json.
set(MSE_SANITIZER "" CACHE STRING "Sanitizer to build with")

if(MSE_SANITIZER STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined
                      -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
elseif(MSE_SANITIZER STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
elseif(NOT MSE_SANITIZER STREQUAL "")
  message(FATAL_ERROR "Unknown MSE_SANITIZER: ${MSE_SANITIZER}")
endif()

# Warnings apply only to our own targets, never to third-party code.
set(MSE_WARNINGS -Wall -Wextra -Wpedantic -Werror)

# The HTTP server's two dependencies, pinned to a release and checked by hash. SYSTEM keeps their
# headers out of our warning flags. cpp-httplib is used header-only and without TLS or compression,
# so the build does not depend on which libraries the machine happens to have.
include(FetchContent)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  httplib
  URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.3.tar.gz
  URL_HASH SHA256=a0567bcd6c3fe5cef1b329b96245119047f876b49e06cc129a36a7a8dffe173e
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  SYSTEM)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  SYSTEM)
FetchContent_MakeAvailable(httplib nlohmann_json)

file(GLOB MSE_CORE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(FILTER MSE_CORE_SOURCES EXCLUDE REGEX "/server\\.cpp$")  # the server is its own library
add_library(mse_core STATIC ${MSE_CORE_SOURCES})
target_include_directories(mse_core PUBLIC src)
target_compile_options(mse_core PRIVATE ${MSE_WARNINGS})

add_library(mse_server STATIC src/server.cpp)
target_link_libraries(mse_server PUBLIC mse_core nlohmann_json::nlohmann_json
                      PRIVATE httplib::httplib)
target_compile_options(mse_server PRIVATE ${MSE_WARNINGS})

# The command-line code lives in a library so that the tests can call it too.
add_library(mse_app STATIC app/cli.cpp)
target_include_directories(mse_app PUBLIC app)
target_link_libraries(mse_app PUBLIC mse_core mse_server)
target_compile_options(mse_app PRIVATE ${MSE_WARNINGS})

add_executable(msearch app/main.cpp)
target_link_libraries(msearch PRIVATE mse_app)
target_compile_options(msearch PRIVATE ${MSE_WARNINGS})

# The benchmark runner. Only numbers from a release build mean anything; it refuses to run otherwise.
add_executable(msearch-bench bench/bench_main.cpp)
target_link_libraries(msearch-bench PRIVATE mse_core)
target_compile_options(msearch-bench PRIVATE ${MSE_WARNINGS})

enable_testing()
add_subdirectory(tests)
