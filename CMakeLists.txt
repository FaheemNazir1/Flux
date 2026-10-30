cmake_minimum_required(VERSION 3.16)
# THE version of Flux. Everything else is derived from this line: the generated header below (used by
# `flux --version` and the app's About box), the Windows version resources of both executables, and the
# names of the package and installer made by packaging/package.ps1. Pre-1.0 numbering: MAJOR.MINOR.PATCH.
project(Flux VERSION 0.9.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

if(WIN32)
    enable_language(RC)  # version resources for both executables, and the GUI manifest
endif()

# Generated header: build/generated/flux_version.h (included by C++ code and by the .rc files).
configure_file(packaging/version.h.in ${CMAKE_BINARY_DIR}/generated/flux_version.h @ONLY)

# Vendored SQLite amalgamation (see third_party/sqlite/README.md). Built without
# Flux's warning flags, which are meant for our own code.
add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/sqlite)
target_compile_definitions(sqlite3 PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION)
target_link_libraries(sqlite3 PUBLIC Threads::Threads)

# Core library: everything except the CLI entry point, so tests can link it.
add_library(flux_core
    src/tokenizer.cpp
    src/scanner.cpp
    src/indexer.cpp
    src/worker_pool.cpp
    src/options.cpp
    src/database.cpp
    src/index_writer.cpp
    src/search.cpp
    src/drives.cpp
    src/path_utils.cpp
    src/sync.cpp
    src/roots.cpp
    src/app_paths.cpp
    src/cli.cpp
    src/job_runner.cpp
    src/gui_settings.cpp
    src/service.cpp
    src/extract.cpp
    src/content_stats.cpp
    src/extract_inflate.cpp
    src/extract_text.cpp
    src/extract_office.cpp
    src/extract_pdf.cpp
)
target_include_directories(flux_core PUBLIC include ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(flux_core PUBLIC Threads::Threads sqlite3)
if(WIN32)
    target_link_libraries(flux_core PUBLIC shell32)  # CommandLineToArgvW: UTF-8 command-line arguments
endif()
if(MSVC)
    target_compile_options(flux_core PUBLIC /W4 /permissive-)
else()
    target_compile_options(flux_core PUBLIC -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    add_executable(flux src/main.cpp src/flux.rc)
    set_source_files_properties(src/flux.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_BINARY_DIR}/generated/flux_version.h)
else()
    add_executable(flux src/main.cpp)
endif()
target_link_libraries(flux PRIVATE flux_core)

# A distributable program must not need the compiler's runtime DLLs (libstdc++-6.dll, libgcc_s_seh-1.dll,
# libwinpthread-1.dll): they exist on a development machine only because its PATH points at MSYS2.
# Linking them in makes flux.exe and flux-gui.exe depend on Windows system DLLs alone.
if(MINGW)
    option(FLUX_STATIC_RUNTIME "Link the GCC/C++/winpthread runtime into flux.exe and flux-gui.exe (no MinGW DLLs needed)" ON)
endif()
if(MINGW AND FLUX_STATIC_RUNTIME)
    target_link_options(flux PRIVATE -static)
endif()

enable_testing()
foreach(test_name
        test_tokenizer test_scanner test_indexer
        test_work_queue test_worker_pool test_options
        test_database test_index_writer test_search
        test_path_utils test_sync test_roots test_app_paths test_cli
        test_job_runner test_gui_settings test_service test_drives
        test_extract test_content)
    add_executable(${test_name} tests/${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE flux_core)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
# The content tests read small documents with unique known words (tests/fixtures; see make_fixtures.py there).
foreach(test_name test_extract test_content test_cli)
    target_compile_definitions(${test_name} PRIVATE FLUX_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
endforeach()
if(WIN32)
    target_link_libraries(test_content PRIVATE psapi)  # peak memory of reading large documents
endif()

# Per-format extraction speed on generated documents (not a test; run it from a Release build).
add_executable(extract_bench benchmarks/extract_bench.cpp)
target_link_libraries(extract_bench PRIVATE flux_core)
if(WIN32)
    target_link_libraries(extract_bench PRIVATE psapi)
endif()

# The Windows desktop app. A thin native Win32 front end over flux_core (no new dependencies); the
# command-line tool does not depend on it, and -DFLUX_BUILD_GUI=OFF leaves it out.
if(WIN32)
    option(FLUX_BUILD_GUI "Build the Windows desktop app (flux-gui.exe)" ON)
endif()
if(WIN32 AND FLUX_BUILD_GUI)
    add_executable(flux-gui WIN32
        gui/main.cpp gui/main_window.cpp gui/settings_dialog.cpp gui/smoke_test.cpp gui/flux-gui.rc)
    set_source_files_properties(gui/flux-gui.rc PROPERTIES OBJECT_DEPENDS
        "${CMAKE_SOURCE_DIR}/gui/flux-gui.manifest;${CMAKE_BINARY_DIR}/generated/flux_version.h")
    if(MINGW AND FLUX_STATIC_RUNTIME)
        target_link_options(flux-gui PRIVATE -static)
    endif()
    target_include_directories(flux-gui PRIVATE gui)
    target_compile_definitions(flux-gui PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    target_link_libraries(flux-gui PRIVATE flux_core comctl32 uxtheme shlwapi shell32 ole32 uuid user32 gdi32 comdlg32)

    find_program(FLUX_POWERSHELL_GUI powershell
                 HINTS "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0")
    if(FLUX_POWERSHELL_GUI)
        add_test(NAME test_gui_smoke
                 COMMAND ${FLUX_POWERSHELL_GUI} -NoProfile -ExecutionPolicy Bypass
                         -File ${CMAKE_SOURCE_DIR}/tests/test_gui_smoke.ps1
                         -GuiExe $<TARGET_FILE:flux-gui>)
        set_tests_properties(test_gui_smoke PROPERTIES TIMEOUT 600)
    endif()
endif()

# The Windows launcher and the command line as real processes (never touches the user's real database).
if(WIN32)
    find_program(FLUX_POWERSHELL powershell
                 HINTS "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0")
    if(NOT FLUX_POWERSHELL)
        message(WARNING "powershell.exe not found: test_windows_launcher will not be registered")
    endif()
    if(FLUX_POWERSHELL)
        set(FLUX_LAUNCHER_TEST_ARGS -Launcher ${CMAKE_SOURCE_DIR}/run-flux.bat -FluxExe $<TARGET_FILE:flux>)
        if(TARGET flux-gui)
            list(APPEND FLUX_LAUNCHER_TEST_ARGS -GuiExe $<TARGET_FILE:flux-gui>)
        endif()
        add_test(NAME test_windows_launcher
                 COMMAND ${FLUX_POWERSHELL} -NoProfile -ExecutionPolicy Bypass
                         -File ${CMAKE_SOURCE_DIR}/tests/test_windows_launcher.ps1
                         ${FLUX_LAUNCHER_TEST_ARGS})
        # Stage, zip and verify the distribution from the freshly built programs, in the build tree.
        # (The real, Release-built package is made with packaging/package.ps1; no installer is compiled here.)
        if(TARGET flux-gui)
            add_test(NAME test_package
                     COMMAND ${FLUX_POWERSHELL} -NoProfile -ExecutionPolicy Bypass
                             -File ${CMAKE_SOURCE_DIR}/packaging/package.ps1
                             -SkipBuild -FluxExe $<TARGET_FILE:flux> -GuiExe $<TARGET_FILE:flux-gui>
                             -OutputDir ${CMAKE_BINARY_DIR}/package-test -SkipInstaller)
            set_tests_properties(test_package PROPERTIES TIMEOUT 600)
        endif()
    endif()
endif()

# Benchmark tooling (see benchmarks/README.md). The benchmarks themselves are never run by
# CTest; only the fast unit tests of their support code are.
option(FLUX_BUILD_BENCHMARKS "Build the flux_bench tool and its unit tests" ON)
if(FLUX_BUILD_BENCHMARKS)
    add_library(flux_bench_support STATIC benchmarks/dataset_generator.cpp)
    target_include_directories(flux_bench_support PUBLIC benchmarks)
    target_link_libraries(flux_bench_support PUBLIC flux_core)

    add_executable(flux_bench benchmarks/flux_bench.cpp)
    target_link_libraries(flux_bench PRIVATE flux_bench_support)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" FLUX_BUILD_TYPE_UPPER)
    target_compile_definitions(flux_bench PRIVATE
        FLUX_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
        FLUX_CXX_FLAGS="${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${FLUX_BUILD_TYPE_UPPER}}")

    foreach(test_name test_bench_metrics test_dataset_generator)
        add_executable(${test_name} tests/${test_name}.cpp)
        target_link_libraries(${test_name} PRIVATE flux_bench_support)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()
