cmake_minimum_required(VERSION 3.22.1)

project(ACppSrv LANGUAGES CXX)

include(CTest)

# Known build types
set(
    BUILD_TYPES
    Debug
    Release
    RelWithDebInfo
    MinSizeRel
)

# Known sanitizers
set(
    SANITIZERS
    OFF
    address
    leak
    thread
    undefined
)

### BEGIN Build configuration #################################################
option(USE_LIBC++ "Use libc++ instead of libstdc++" OFF)

set(
    BUILD_TYPE "RelWithDebInfo" CACHE STRING
    "Select a build type (${BUILD_TYPES})"
)

set(SANITIZER "OFF" CACHE STRING "Select a sanitizer (${SANITIZERS})")

### END Build configuration ###################################################

# Check build configuration
set_property(CACHE BUILD_TYPE PROPERTY STRINGS ${BUILD_TYPES})
if (NOT BUILD_TYPE IN_LIST BUILD_TYPES)
    message(FATAL_ERROR "Unknown value of BUILD_TYPE: ${BUILD_TYPE}")
endif()
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE ${BUILD_TYPE})
endif()

set_property(CACHE SANITIZER PROPERTY STRINGS ${SANITIZERS})
if (NOT SANITIZER IN_LIST SANITIZERS)
    message(FATAL_ERROR "Unknown value of SANITIZER: ${SANITIZER}")
endif()

# Version from Git
message(CHECK_START "Getting version by Git")
execute_process(
    COMMAND git describe --always
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_DESCRIBE OUTPUT_STRIP_TRAILING_WHITESPACE
)
if (GIT_DESCRIBE)
    message(CHECK_PASS "OK")
else()
    message(CHECK_FAIL "FAIL")
    set(GIT_DESCRIBE "unknown")
endif()

# C++ options
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(
    -Wall -Wextra -pedantic -Werror
    -Wconversion -Wsign-conversion
    -Wswitch-default -Wswitch-enum
    -Wno-mismatched-new-delete
)
if (USE_LIBC++)
    add_compile_options(-stdlib=libc++)
    add_link_options(-stdlib=libc++)
endif()
if (CMAKE_CXX_COMPILER_ID STREQUAL Clang)
    add_compile_options(-Wimplicit-fallthrough)
endif()
if (CMAKE_CXX_COMPILER_ID STREQUAL GNU)
    add_compile_options(-Wimplicit-fallthrough=5)
endif()
if (ENABLE_TEMPORARY_DEBUG)
    add_compile_definitions(ENABLE_TEMPORARY_DEBUG)
endif()
if (NOT SANITIZER STREQUAL "OFF")
    add_compile_options(
        -fsanitize=${SANITIZER}
    )
    add_link_options(
        -fsanitize=${SANITIZER}
    )
endif()

# We really want asserts in all builds
add_compile_options(-UNDEBUG)

# Used packages
find_package(Boost 1.81 REQUIRED)
find_package(Protobuf 3.12.4 REQUIRED)
find_package(SQLite3 3.37 REQUIRED)
find_package(Threads REQUIRED)

include_directories(
    SYSTEM
    ${Boost_INCLUDE_DIRS}
)
link_directories(
    ${Boost_LIBRARY_DIRS}
)

# Optional Markdown utility for generating the top-level documentation stub
find_program(
    MARKDOWN
    NAMES markdown markdown_py
    DOC "Program for processing Markdown to HTML"
)

# Subdirectories with CMake configurations
add_subdirectory(src)

# Generate documentation, optional (Markdown)
if (MARKDOWN)
    add_custom_command(
        OUTPUT README.html
        COMMAND ${MARKDOWN} ${CMAKE_CURRENT_SOURCE_DIR}/README.md > README.html
        MAIN_DEPENDENCY ${CMAKE_CURRENT_SOURCE_DIR}/README.md
    )
    add_custom_command(
        OUTPUT LICENSE.html
        COMMAND
            ${MARKDOWN} ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.md > LICENSE.html
        MAIN_DEPENDENCY ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.md
    )
    add_custom_target(doc ALL DEPENDS README.html LICENSE.html)
endif()

# Clang-tidy
set(CLEAN_TIDY_CMD "")
set(CLANG_TIDY_CMD "")
set(CLANG_TIDY_VERSION "")
if (CLANG_TIDY)
    set(
        CLEAN_TIDY_CMD
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} -t clean-tidy
    )
    set(
        CLANG_TIDY_CMD
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} -t clang-tidy
            -j `nproc`
    )
    execute_process(
        COMMAND ${CLANG_TIDY} --version
        COMMAND head -n 1
        OUTPUT_VARIABLE CLANG_TIDY_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()

# Clean and build everything
add_custom_target(
    full
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} -t clean
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} -t all -j `nproc`
    ${CLEAN_TIDY_CMD}
    ${CLANG_TIDY_CMD}
)

# Report CMake configuration
message(STATUS "=== Build configuration ===")
message(STATUS "Version:      ${GIT_DESCRIBE}")
message(STATUS "Build type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "Markdown:     ${MARKDOWN}")
message(
    STATUS
    "C++ compiler: ${CMAKE_CXX_COMPILER} ${CMAKE_CXX_COMPILER_ID} "
    ${CMAKE_CXX_COMPILER_VERSION}
)
message(STATUS "Use libc++:   ${USE_LIBC++}")
message(STATUS "Clang-tidy:   ${CLANG_TIDY} ${CLANG_TIDY_VERSION}")
message(STATUS "DEBUG:        ${ENABLE_TEMPORARY_DEBUG}")
message(STATUS "Sanitizer:    ${SANITIZER}")
