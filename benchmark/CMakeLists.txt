# The main benchmark executable
add_executable(
    benchmark
    benchmark.cpp
)
target_include_directories(benchmark PRIVATE ${CMAKE_BINARY_DIR}/src)
target_link_libraries(
    benchmark
    acpplib
    OpenSSL::Crypto
    protobuf::libprotobuf
    SQLite::SQLite3
)
