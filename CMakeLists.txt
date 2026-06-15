cmake_minimum_required(VERSION 3.16)

project(linux_http_server_lab
  VERSION 0.1.0
  DESCRIPTION "HTTP/1.1 server comparing thread-per-connection, thread-pool and epoll designs"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Platform and source-tree guards
# ---------------------------------------------------------------------------
# The project targets Linux only (epoll, eventfd, accept4 arrive in later
# milestones). Failing early gives a clear message instead of a confusing
# compile error later.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "This project supports Linux only (detected: ${CMAKE_SYSTEM_NAME}).")
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not supported. Use: cmake -S . -B build")
endif()

# ---------------------------------------------------------------------------
# Language standard and build type
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # -std=c++17, not -std=gnu++17

set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # for clangd / clang-tidy

# Single-config generators (Makefiles, Ninja) get Release when nothing is given.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release)

# ---------------------------------------------------------------------------
# Project options
# ---------------------------------------------------------------------------
option(SRV_BUILD_TESTS "Build the unit and smoke tests" ON)
option(SRV_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(SRV_USE_SYSTEM_GTEST "Use an installed GoogleTest instead of fetching a pinned copy" OFF)

set(SRV_SANITIZER "none" CACHE STRING
  "Sanitizer configuration: none, asan-ubsan or tsan")
set_property(CACHE SRV_SANITIZER PROPERTY STRINGS none asan-ubsan tsan)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ProjectOptions) # defines srv_project_options (warnings)
include(Sanitizers)     # applies SRV_SANITIZER globally

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
add_subdirectory(src)

if(SRV_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
