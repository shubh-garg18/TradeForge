cmake_minimum_required(VERSION 3.16)

project(SystemDev VERSION 1.0 LANGUAGES CXX)

# ============================
# Global Configuration
# ============================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# ============================
# Compiler Warnings
# ============================
set(WARNING_FLAGS
    -Wall
    -Wextra
    -Wshadow
    -Wconversion
    -Wpedantic
)

# ============================
# Core Library
# ============================
add_library(core
    src/core/MatchingEngine.cpp
    src/core/OrderBook.cpp
    src/core/EventQueue.cpp
)

target_include_directories(core PUBLIC include)
target_compile_options(core PRIVATE ${WARNING_FLAGS})

# ============================
# Utils Library
# ============================
add_library(utils
    src/utils/TimeUtils.cpp
)

target_include_directories(utils PUBLIC include)
target_compile_options(utils PRIVATE ${WARNING_FLAGS})

# ============================
# IO Library
# ============================
add_library(io
    src/io/PrintL2Snapshot.cpp
    src/io/PrintBBO.cpp
)

target_include_directories(io PUBLIC include)
target_compile_options(io PRIVATE ${WARNING_FLAGS})

# ============================
# Fee Calculator Library
# ============================
add_library(fee_calculator
    src/FeeCalculator/FeeCalculator.cpp
)

target_include_directories(fee_calculator PUBLIC include)
target_compile_options(fee_calculator PRIVATE ${WARNING_FLAGS})

# ============================
# Link Dependencies
# ============================
target_link_libraries(io PUBLIC core utils)
target_link_libraries(fee_calculator PUBLIC core)
target_link_libraries(core PUBLIC utils)

# ============================
# Main Engine Executable
# ============================
add_executable(engine
    src/main.cpp
    src/tests/test_orderbook.cpp
)

target_link_libraries(engine PRIVATE core io fee_calculator utils)

# ============================
# Benchmark Executable
# ============================
add_executable(bench
    bench/Benchmark.cpp
)

target_link_libraries(bench PRIVATE core fee_calculator utils)
target_compile_options(bench PRIVATE -O3 -march=native)

# ============================
# Build Type Flags
# ============================
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(engine PRIVATE -g)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(engine PRIVATE -O3 -march=native)
endif()

# ============================
# Runtime Directories
# ============================
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/logs)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/trade_logs)

# ========================
# Build Info
# ========================
message(STATUS "")
message(STATUS "========== Build Configuration ==========")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "========================================")
message(STATUS "")