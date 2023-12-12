# Protobuf files
protobuf_generate_cpp(
    PROTO_CPP PROTO_HPP
    configuration.proto
    http_hnd_db.proto
    http_hnd_stat.proto
)
message("Protobuf generated headers: ${PROTO_HPP}")
message("Protobuf generated sources: ${PROTO_CPP}")

add_library(proto_obj OBJECT ${PROTO_CPP})
target_compile_options(proto_obj PRIVATE -Wno-sign-conversion)

# Library containing functionality shared by executables acppsrv and benchmark 
add_library(
    acpplib STATIC
    log.cpp
    sqlite3.cpp
    worker.cpp
    $<TARGET_OBJECTS:proto_obj>
)
target_include_directories(
    acpplib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

# The main ACppSrv executable
add_executable(
    acppsrv
    acppsrv.cpp
    application.cpp
    configuration.cpp
    db_server.cpp
    http_hnd_db.cpp
    http_hnd_echo.cpp
    http_hnd_stat.cpp
    http_server.cpp
)
target_link_libraries(
    acppsrv
    acpplib
    protobuf::libprotobuf
    SQLite::SQLite3
)

# Check sources by clang-tidy
find_program(CLANG_TIDY NAMES clang-tidy)
if (CLANG_TIDY)
    file(
        GLOB_RECURSE CLANG_TIDY_SRC
        LIST_DIRECTORIES false
        RELATIVE ${CMAKE_SOURCE_DIR}
        *.cpp
    )
    list(TRANSFORM CLANG_TIDY_SRC APPEND .tidy OUTPUT_VARIABLE CLANG_TIDY_OUT)
    list(TRANSFORM CLANG_TIDY_SRC PREPEND ${CMAKE_SOURCE_DIR}/)
    list(TRANSFORM CLANG_TIDY_OUT PREPEND ${CMAKE_BINARY_DIR}/)
    add_custom_target(
        clang-tidy
        COMMAND test `cat ${CLANG_TIDY_OUT} | wc -c` = 0
        DEPENDS ${CLANG_TIDY_OUT}
    )
    foreach (S O IN ZIP_LISTS CLANG_TIDY_SRC CLANG_TIDY_OUT)
        add_custom_command(
            OUTPUT ${O}
            # Do not remove output file on error
            # Run clang-tidy in "best effort" mode - if clang-tidy crashes
            # (which happens, unfortunately), do not treat it as failure
            COMMAND ${CLANG_TIDY} ${S} > ${O} | true
            DEPENDS ${S}
        )
    endforeach()
    add_custom_target(clean-tidy COMMAND rm -f ${CLANG_TIDY_OUT})
endif()

