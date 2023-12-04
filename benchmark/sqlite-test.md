# Benchmarking an HTTP server with SQLite database

## Requirements/sizing

- DB-A
    - records 49.3e9
    - size 3.55 TB
    - 256 shards
        - select shard by data key hash
    - each shard
        - records 193e6
        - data
            - record
                - key 32 B
                - value 32 B
            - 12 GB
        - index
            - key 4 B (hash of data key)
            - value 4 B (index of data record)
            - 2 files, 1.5 GB + 500 kB

- DB-C
    - records 10.6e9
    - size 6.91 TB
    - 256 shards
        - select shard by data key hash
    - each shard
        - records 41e6
        - data
            - record
                - key 32 B
                - value 570 B
            - 25 GB
        - index
            - key 4 B (hash of data key)
            - value 4 B (index of data record)
            - 2 files, 319 MB + 350 kB
