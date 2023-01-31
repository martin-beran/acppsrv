# ACppSrv (Application C++ Server)

An experimental multithreading application server with HTTP API, implemented in
C++20 using Boost.Asio (with coroutines), Boost.Beast, and SQLite database.

## Author

Martin Beran

<martin@mber.cz>

## License

This software is available under the terms of BSD 2-Clause License, see
file [LICENSE.md](LICENSE.html).

## Repository structure

- `src/` - source code
- `test/` - automatic tests

## Build

    cmake -S . -B build
    cmake --build build

This will create executable `build/src/acppsrv`.
