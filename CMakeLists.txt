cmake_minimum_required(VERSION 3.16)
project(Flux VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

# Core library: everything except the CLI entry point, so tests can link it.
add_library(flux_core
    src/tokenizer.cpp
    src/scanner.cpp
    src/indexer.cpp
    src/worker_pool.cpp
    src/options.cpp
)
target_include_directories(flux_core PUBLIC include)
target_link_libraries(flux_core PUBLIC Threads::Threads)
if(MSVC)
    target_compile_options(flux_core PUBLIC /W4 /permissive-)
else()
    target_compile_options(flux_core PUBLIC -Wall -Wextra -Wpedantic)
endif()

add_executable(flux src/main.cpp)
target_link_libraries(flux PRIVATE flux_core)

enable_testing()
foreach(test_name
        test_tokenizer test_scanner test_indexer
        test_work_queue test_worker_pool test_options)
    add_executable(${test_name} tests/${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE flux_core)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
