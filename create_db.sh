#!/bin/bash

if [ $# != 1 ]; then
    echo "usage: $0 database.sqlite" >&2
    exit 1
fi

sqlite3 "$1" <<EOF
create table data (key primary key, value);
EOF
