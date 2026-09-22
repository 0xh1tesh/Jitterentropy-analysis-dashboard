cmake_minimum_required(VERSION 3.20)

project(jitterentropy_app LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(JITTERENTROPY_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../jitterentropy-library"
    CACHE PATH "Path to the Jitterentropy source tree containing jitterentropy.h")

set(JITTERENTROPY_LIB_DIR
    "${JITTERENTROPY_ROOT}/build"
    CACHE PATH "Directory containing libjitterentropy.a")

set(WEBVIEW2_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../deps/webview2/build/native"
    CACHE PATH "Path to WebView2 SDK native folder containing include/ and x64/")

find_library(JITTERENTROPY_LIBRARY
    NAMES jitterentropy libjitterentropy.a
    PATHS "${JITTERENTROPY_LIB_DIR}"
    NO_DEFAULT_PATH)

if(NOT JITTERENTROPY_LIBRARY)
    message(FATAL_ERROR
        "Could not find libjitterentropy.a in JITTERENTROPY_LIB_DIR="
        "${JITTERENTROPY_LIB_DIR}. Reconfigure with "
        "-DJITTERENTROPY_LIB_DIR=<directory-containing-libjitterentropy.a>.")
endif()

if(NOT EXISTS "${JITTERENTROPY_ROOT}/jitterentropy.h")
    message(FATAL_ERROR
        "Could not find jitterentropy.h in JITTERENTROPY_ROOT="
        "${JITTERENTROPY_ROOT}. Reconfigure with "
        "-DJITTERENTROPY_ROOT=<jitterentropy-source-directory>.")
endif()

add_library(jitterentropy STATIC IMPORTED)
set_target_properties(jitterentropy PROPERTIES
    IMPORTED_LOCATION "${JITTERENTROPY_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${JITTERENTROPY_ROOT}")

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# CLI target (unchanged)
# ---------------------------------------------------------------------------
add_executable(jitterentropy_app
    main.c
    rng_wrapper.c
    health_monitor.c
    benchmark.c
    entropy_stats.c)

target_include_directories(jitterentropy_app PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}")

target_link_libraries(jitterentropy_app PRIVATE
    jitterentropy
    Threads::Threads)

if(WIN32)
    # MinGW static archives do not auto-link Windows system libraries. Even
    # though this app does not call jent_status() or public jent_uuid(),
    # jent_entropy_collector_alloc() calls the library's internal
    # jent_uuid_generate(), whose Windows backend uses BCryptGenRandom().
    target_link_libraries(jitterentropy_app PRIVATE bcrypt)
endif()

if(MINGW)
    target_compile_options(jitterentropy_app PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# GUI target (WebView2 + Win32)
# ---------------------------------------------------------------------------
if(WIN32)
    set(WEBVIEW2_INCLUDE_DIR "${WEBVIEW2_ROOT}/include")
    set(WEBVIEW2_LIB_FILE "${WEBVIEW2_ROOT}/x64/WebView2Loader.dll.lib")
    set(WEBVIEW2_DLL_FILE "${WEBVIEW2_ROOT}/x64/WebView2Loader.dll")
    file(GLOB_RECURSE GUI_ASSETS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/gui/*")

    if(NOT EXISTS "${WEBVIEW2_INCLUDE_DIR}/WebView2.h")
        message(FATAL_ERROR "Could not find WebView2.h in WEBVIEW2_ROOT=${WEBVIEW2_ROOT}")
    endif()

    if(NOT EXISTS "${WEBVIEW2_LIB_FILE}")
        message(FATAL_ERROR "Could not find WebView2Loader.dll.lib at ${WEBVIEW2_LIB_FILE}")
    endif()

    add_executable(jitterentropy_gui
        main_gui.cpp
        rng_wrapper.c
        health_monitor.c
        benchmark.c
        entropy_stats.c)

    target_include_directories(jitterentropy_gui PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}"
        "${WEBVIEW2_INCLUDE_DIR}")

    target_link_libraries(jitterentropy_gui PRIVATE
        jitterentropy
        Threads::Threads
        bcrypt
        ole32
        uuid
        user32
        gdi32
        "${WEBVIEW2_LIB_FILE}")

    if(MINGW)
        target_compile_options(jitterentropy_gui PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_custom_command(TARGET jitterentropy_gui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WEBVIEW2_DLL_FILE}"
            "$<TARGET_FILE_DIR:jitterentropy_gui>"
        COMMENT "Copying WebView2Loader.dll next to jitterentropy_gui.exe")

    add_custom_command(TARGET jitterentropy_gui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_LIST_DIR}/gui"
            "$<TARGET_FILE_DIR:jitterentropy_gui>/gui"
        COMMENT "Copying app/gui directory next to jitterentropy_gui.exe")

    # Frontend files are runtime inputs. Make asset changes force a relink so
    # the POST_BUILD copy above refreshes the executable's adjacent gui folder.
    set_property(TARGET jitterentropy_gui APPEND PROPERTY
        LINK_DEPENDS "${GUI_ASSETS}")
endif()

# Unit tests (no WebView2 or jitterentropy needed).
enable_testing()
add_executable(test_bridge_json tests/test_bridge_json.cpp)
target_include_directories(test_bridge_json PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME bridge_json COMMAND test_bridge_json)

add_executable(test_entropy_stats tests/test_entropy_stats.c entropy_stats.c)
target_include_directories(test_entropy_stats PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME entropy_stats COMMAND test_entropy_stats)
