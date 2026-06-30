cmake_minimum_required(VERSION 3.22)

project(etb_bhop VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BHOP_BUILD_TESTS "Build the deterministic movement tests" ON)
option(BHOP_BUILD_UE4SS_MOD "Build the native UE4SS adapter" OFF)
set(UE4SS_ROOT "" CACHE PATH "Path to the ETBCommunity/UE4SS 1.3.0 source tree")

add_library(bhop_core STATIC
    src/config.cpp
    src/movement.cpp
)
target_include_directories(bhop_core PUBLIC src)
target_compile_features(bhop_core PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(bhop_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(bhop_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BHOP_BUILD_TESTS)
    enable_testing()
    add_executable(bhop_tests tests/movement_tests.cpp)
    target_link_libraries(bhop_tests PRIVATE bhop_core)
    add_test(NAME bhop_tests COMMAND bhop_tests)
endif()

if(BHOP_BUILD_UE4SS_MOD)
    if(NOT UE4SS_ROOT OR NOT EXISTS "${UE4SS_ROOT}/CMakeLists.txt")
        message(FATAL_ERROR
            "BHOP_BUILD_UE4SS_MOD requires UE4SS_ROOT to point at the "
            "ETBCommunity/UE4SS 1.3.0 source tree with its submodules initialized.")
    endif()

    set(BHOP_UE4SS_1_3_0_COMMIT "0a3cf5ebaf28c9385523b4a651542be6746ed1e9")
    execute_process(
        COMMAND git -C "${UE4SS_ROOT}" rev-parse HEAD
        OUTPUT_VARIABLE BHOP_UE4SS_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE BHOP_UE4SS_GIT_RESULT
    )
    if(NOT BHOP_UE4SS_GIT_RESULT EQUAL 0
       OR NOT BHOP_UE4SS_COMMIT STREQUAL BHOP_UE4SS_1_3_0_COMMIT)
        message(FATAL_ERROR
            "UE4SS_ROOT must be ETBCommunity/UE4SS tag 1.3.0 "
            "(${BHOP_UE4SS_1_3_0_COMMIT}); found '${BHOP_UE4SS_COMMIT}'.")
    endif()

    include(FetchContent)
    FetchContent_Declare(
        minhook
        GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
        GIT_TAG v1.3.4
    )
    FetchContent_MakeAvailable(minhook)

    add_subdirectory("${UE4SS_ROOT}" "${CMAKE_BINARY_DIR}/ue4ss")

    add_library(etb_bhop SHARED
        src/hooks.cpp
        src/hooks.h
        src/input.cpp
        src/ladder.cpp
        src/mod.cpp
        src/mod.h
        src/player.cpp
        src/runtime.cpp
        src/util/process.cpp
        src/util/process.h
        src/util/scanner.cpp
        src/util/scanner.h
        src/water.cpp
    )
    target_include_directories(etb_bhop PRIVATE src)
    target_link_libraries(etb_bhop PRIVATE bhop_core UE4SS minhook Zydis)
    set_target_properties(etb_bhop PROPERTIES OUTPUT_NAME main)
    add_custom_command(
        TARGET etb_bhop
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_SOURCE_DIR}/package/bhop/dlls"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:etb_bhop>"
                "${CMAKE_SOURCE_DIR}/package/bhop/dlls/main.dll"
        COMMENT "Staging native bhop package"
    )

    if(MSVC)
        target_compile_options(etb_bhop PRIVATE /W4 /permissive-)
    endif()
endif()
