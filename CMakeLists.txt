cmake_minimum_required(VERSION 3.5...4.0)
project(PHAST)

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release")
endif ()

MESSAGE(STATUS "Build type: " ${CMAKE_BUILD_TYPE})

if (NOT TARGET PHAST)
  add_library(PHAST INTERFACE)
  target_include_directories(PHAST INTERFACE include)

  target_compile_features(PHAST INTERFACE cxx_std_17)

  MESSAGE(STATUS "Compiling for processor: " ${CMAKE_HOST_SYSTEM_PROCESSOR})

  if (UNIX AND (CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "x86_64"))
    MESSAGE(STATUS "Compiling with flags: -march=native -mbmi2 -msse4.2")
    target_compile_options(PHAST INTERFACE -march=native)
    target_compile_options(PHAST INTERFACE -mbmi2 -msse4.2) # for hardware popcount and pdep
  endif()

  if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    MESSAGE(STATUS "Compiling with flags: -O0")
    target_compile_options(PHAST INTERFACE -O0)
  endif()

  find_package(Threads REQUIRED)
  target_link_libraries(PHAST INTERFACE Threads::Threads)

  if (PHAST_USE_SANITIZERS)
    MESSAGE(STATUS "Using sanitizers. Compiling with flags: -fsanitize=address -fno-omit-frame-pointer")
    target_compile_options(PHAST INTERFACE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(PHAST INTERFACE -fsanitize=address)
  endif()

  # Source the `bits` library. Two locations are supported:
  #   1. external/bits          -- standalone mode (recommended): bits is a git submodule.
  #   2. ../                    -- development mode: phast/ lives inside the bits repo.
  # `bits` itself pulls in `essentials`. If `external/essentials` is also present
  # (e.g. as a sibling submodule), it must be added BEFORE `external/bits` so that
  # both libraries see the same ESSENTIALS target.
  if (EXISTS ${CMAKE_CURRENT_LIST_DIR}/external/essentials/CMakeLists.txt)
    if (NOT TARGET ESSENTIALS)
      MESSAGE(STATUS "Configuring the `essentials` library:")
      add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/external/essentials
                       ${CMAKE_BINARY_DIR}/essentials-build)
    endif()
  endif()

  if (EXISTS ${CMAKE_CURRENT_LIST_DIR}/external/bits/CMakeLists.txt)
    MESSAGE(STATUS "Configuring the `bits` library: external/bits")
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/external/bits
                     ${CMAKE_BINARY_DIR}/bits-build)
  elseif (EXISTS ${CMAKE_CURRENT_LIST_DIR}/../CMakeLists.txt AND
          EXISTS ${CMAKE_CURRENT_LIST_DIR}/../include/elias_fano.hpp)
    MESSAGE(STATUS "Configuring the `bits` library: parent directory (development mode)")
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/.. ${CMAKE_BINARY_DIR}/bits-build)
  else()
    MESSAGE(FATAL_ERROR
      "Could not find the `bits` library. Initialize the git submodules with:\n"
      "    git submodule update --init --recursive")
  endif()
  target_link_libraries(PHAST INTERFACE BITS)
endif()

# Only add tests, examples, and benchmarks when compiling PHast itself,
# not when added as a dependency.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  target_compile_options(PHAST INTERFACE -Wall -Wextra -Wno-missing-braces -Wno-unknown-attributes -Wno-unused-function -ggdb)

  enable_testing()
  file(GLOB TEST_SOURCES test/test_*.cpp)
  foreach(TEST_SRC ${TEST_SOURCES})
    get_filename_component(TEST_SRC_NAME ${TEST_SRC} NAME_WE)
    add_executable(${TEST_SRC_NAME} ${TEST_SRC})
    add_test(${TEST_SRC_NAME} ${TEST_SRC_NAME})
    target_link_libraries(${TEST_SRC_NAME} PRIVATE PHAST)
  endforeach(TEST_SRC)

  if (EXISTS ${CMAKE_CURRENT_LIST_DIR}/src/example.cpp)
    add_executable(example src/example.cpp)
    target_link_libraries(example PRIVATE PHAST)
  endif()

  file(GLOB BENCH_SOURCES benchmarks/perf_*.cpp)
  foreach(BENCH_SRC ${BENCH_SOURCES})
    get_filename_component(BENCH_SRC_NAME ${BENCH_SRC} NAME_WE)
    add_executable(${BENCH_SRC_NAME} ${BENCH_SRC})
    target_link_libraries(${BENCH_SRC_NAME} PRIVATE PHAST)
  endforeach(BENCH_SRC)
endif()
