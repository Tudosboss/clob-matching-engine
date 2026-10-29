cmake_minimum_required(VERSION 3.20)
project(clob_matching_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(CLOB_SANITIZE "Build tests with AddressSanitizer and UBSan (GCC/Clang)" OFF)

add_library(clob INTERFACE)
target_include_directories(clob INTERFACE include)

if(MSVC)
    set(WARNINGS /W4 /wd4324)   # 4324: padding added because of alignas, which is intended
else()
    set(WARNINGS -Wall -Wextra -Wpedantic)
endif()

add_executable(tests tests/tests.cpp)
target_link_libraries(tests PRIVATE clob)
target_compile_options(tests PRIVATE ${WARNINGS})
if(CLOB_SANITIZE AND NOT MSVC)
    target_compile_options(tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(tests PRIVATE -fsanitize=address,undefined)
endif()

add_executable(benchmark bench/benchmark.cpp)
target_link_libraries(benchmark PRIVATE clob)
target_compile_options(benchmark PRIVATE ${WARNINGS})

enable_testing()
add_test(NAME tests COMMAND tests)
