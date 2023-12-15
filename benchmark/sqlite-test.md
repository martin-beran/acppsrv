# Benchmarking an HTTP server with SQLite database

## Requirements/sizing

- DB-A
    - records 49.3e9
    - size 3.55 TB
    - 256 partitions
        - select partition by data key hash
    - each partition
        - records 193e6
        - data
            - record
                - key 32 B
                - update counter 8 B
                - value 32 B
            - 12 GB
        - index
            - key 4 B (hash of data key)
            - value 4 B (index of data record)
            - 2 files, 1.5 GB + 500 kB
    - preliminary results
        CPU > 30 % idle in all tests (waiting for disk)
        10 partitions, 12 threads, USB SSD ZFS compressratio 1.58
        SQLite page_size 512, partition db file size 19 GB
            1 query/transaction
            pragma synchronous = normal
        SELECT 1500 queries/s 4800 iops 405 MB_rd/s 0 MB_wr/s
        UPDATE 950 queries/s 4800 iops 300 MB_rd/s 100 MB_wr/s
            rd/wr wildly fluctuating
            WAL ~ 500 kB
        INSERT 930 queries/s 3500 iops 100 MB_rd/s 190 MB_wr/s
            WAL ~ 500 kB
        10 % INSERT + 10 % UPDATE
            1200 queries/s 4600 iops 350 MB_rd/s 10 MB_wr/s
            rd/wr wildly fluctuating
            WAL ~ 500 kB
        INSERT without index 73000 queries/s 1100 iops 0 MB_rd/s 100 MB_wr/s
            WAL ~ 500 kB

- DB-C
    - records 10.6e9
    - size 6.91 TB
    - 256 partitions
        - select partition by data key hash
    - each partition
        - records 41e6
        - data
            - record
                - key 32 B
                - update counter 8 B
                - value 570 B
            - 25 GB
        - index
            - key 4 B (hash of data key)
            - value 4 B (index of data record)
            - 2 files, 319 MB + 350 kB
    - preliminary results
        CPU > 30 % idle in all tests (waiting for disk)
        10 partitions, 12 threads, USB SSD ZFS compressratio 11.93 (because
            a value is concatenated multiple copies of key)
        SQLite page_size 1024, partition db file size 41 GB
            1 query/transaction
            pragma synchronous = normal
        SELECT 16000 queries/s 18000 iops 160 MB_rd/s 0 MB_wr/s
        UPDATE 4400 queries/s 10000 iops 100 MB_rd/s 200 MB_wr/s
            rd/wr wildly fluctuating
            WAL ~ 1 MB
        INSERT 2300 queries/s 2700 iops 100 kB_rd/s 250 MB_wr/s
            WAL ~ 1.1 MB
        10 % INSERT + 10 % UPDATE
            8000 queries/s 0 iops 80 MB_rd/s 130 MB_wr/s
            rd/wr wildly fluctuating
            WAL ~ 1.1 MB
        INSERT without index 22900 queries/s 1500 iops 100 kB_rd/s 130 MB_wr/s
            WAL ~ 1.1 MB
