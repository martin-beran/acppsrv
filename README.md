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

- `etc/` - configuration files
- `src/` - source code
- `test/` - automatic tests

## Build

    cmake -S . -B build
    cmake --build build

This will create executable `build/src/acppsrv`.

## Run

1. Create a testing database by script `create_db.sh`.
1. Start the server by command `build/src/acppsrv etc/default_cfg.json`. It
   will write log to its standard error.

1. Send a request by command, for example,

        curl  --data-binary @- -H 'Content-Type: application/json'-v http://localhost:8080/db

    The request body in JSON format should be written to stdandard input of
    `curl`. Request URI paths and protobuf/JSON request and response formats
    have not been documented yet. See source code for available request types.

1. Stop the server by sending it signal `SIGTERM` or `SIGINT` (by `kill` or
   `Ctrl-C`.
