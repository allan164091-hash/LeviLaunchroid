cmake_minimum_required(VERSION 3.22)

project(framegen_mod LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(LL_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(PRELOADER_ANDROID_ROOT
    "${LL_REPO_ROOT}/app/src/main/cpp/preloader"
    CACHE PATH "Path to LiteLDev/preloader-android")

if(NOT EXISTS "${PRELOADER_ANDROID_ROOT}/include/pl/Config.hpp")
    message(FATAL_ERROR
        "PRELOADER_ANDROID_ROOT must point to a preloader-android checkout. "
        "Missing: ${PRELOADER_ANDROID_ROOT}/include/pl/Config.hpp")
endif()

include(FetchContent)

set(FG_DEPS)
if(NOT TARGET nlohmann_json::nlohmann_json)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3)
    list(APPEND FG_DEPS nlohmann_json)
endif()

if(NOT TARGET Boost::pfr)
    FetchContent_Declare(
        boost_pfr
        GIT_REPOSITORY https://github.com/boostorg/pfr.git
        GIT_TAG boost-1.88.0)
    list(APPEND FG_DEPS boost_pfr)
endif()

if(NOT TARGET magic_enum::magic_enum)
    FetchContent_Declare(
        magic_enum
        GIT_REPOSITORY https://github.com/Neargye/magic_enum.git
        GIT_TAG v0.9.7)
    list(APPEND FG_DEPS magic_enum)
endif()

if(NOT TARGET fmt::fmt)
    FetchContent_Declare(
        fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG 11.2.0)
    set(FMT_DOC OFF CACHE BOOL "Disable fmt docs" FORCE)
    set(FMT_TEST OFF CACHE BOOL "Disable fmt tests" FORCE)
    set(FMT_INSTALL OFF CACHE BOOL "Disable fmt install rules" FORCE)
    list(APPEND FG_DEPS fmt)
endif()

if(FG_DEPS)
    FetchContent_MakeAvailable(${FG_DEPS})
endif()

if(ANDROID)
    if(NOT TARGET preloader)
        add_subdirectory("${PRELOADER_ANDROID_ROOT}"
                         "${CMAKE_BINARY_DIR}/_preloader_android")
    endif()

    if(ANDROID_ABI)
        set(FG_OUTPUT_ABI "${ANDROID_ABI}")
    else()
        set(FG_OUTPUT_ABI "android")
    endif()
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY
        "${CMAKE_BINARY_DIR}/out/${FG_OUTPUT_ABI}")

    find_library(FG_LOG_LIB log)

    add_library(framegen_mod SHARED src/FramegenMod.cpp)
    set_target_properties(framegen_mod PROPERTIES OUTPUT_NAME "framegen_mod")

    target_include_directories(framegen_mod PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/src"
        "${PRELOADER_ANDROID_ROOT}/include")
    target_link_libraries(framegen_mod PRIVATE
        nlohmann_json::nlohmann_json
        Boost::pfr
        magic_enum::magic_enum
        fmt::fmt
        preloader)
    target_compile_options(framegen_mod PRIVATE
        $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wall -Wextra -Wpedantic>
        $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-fvisibility=hidden -fvisibility-inlines-hidden>)

    if(FG_LOG_LIB)
        target_link_libraries(framegen_mod PRIVATE "${FG_LOG_LIB}")
    endif()
endif()
