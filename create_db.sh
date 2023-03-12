#!/bin/bash

if [ $# != 1 ]; then
    echo "usage: $0 database.sqlite" >&2
    exit 1
fi

sqlite3 "$1" <<EOF
pragma page_size = 4096;
pragma journal_mode = WAL;
create table data (key primary key, value);
EOF
