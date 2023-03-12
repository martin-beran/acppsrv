#!/bin/bash

if [ $# != 2 ]; then
    echo "usage: $0 database.sqlite value_count" >&2
    exit 1
fi

value_count="$2"

sqlite3 "$1" <<EOF
delete from data;
with recursive
    v(i) as (
        select 0 union all
        select i + 1 from v where i < $value_count - 1
    )
insert into data select i, 'val[' || i || ']' from v;
EOF
