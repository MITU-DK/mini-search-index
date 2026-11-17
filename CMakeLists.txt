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

file(GLOB MSE_CORE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(mse_core STATIC ${MSE_CORE_SOURCES})
target_include_directories(mse_core PUBLIC src)
target_compile_options(mse_core PRIVATE ${MSE_WARNINGS})

enable_testing()
add_subdirectory(tests)
