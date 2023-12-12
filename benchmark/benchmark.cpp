#include "log.hpp"
#include "sqlite3.hpp"
#include "worker.hpp"

#include <boost/asio.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/beast.hpp>

#include <openssl/sha.h>

#include <charconv>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

using namespace acppsrv;
using namespace std::string_literals;
using namespace std::string_view_literals;

namespace {

namespace asio = boost::asio;

constexpr unsigned partitions_min = 1;
constexpr unsigned partitions_max = 256;
constexpr size_t key_sz = 32; // do not change this value
constexpr size_t value_sz_min = 1;
constexpr size_t value_sz_max = 4096;
// The maximum number of outstanding requests for creation a database record
constexpr unsigned long create_queue_max = 1'000;
// A duration message will be logged each time this number of new recods is created
constexpr unsigned long create_log_interval = 100'000;

// We expect a key to be SHA256, that is, essentially 32 random bytes, so that
// we can take independent subspans of it as the hash functions for assigning a
// key to a partition and for indexing records. The first byte is used to
// select a partition and the next 4 bytes are used as the key hash value.

[[maybe_unused]] unsigned partition_key(std::span<char> key)
{
    if (!key.empty())
        return static_cast<unsigned char>(key[0]);
    else
        return 0U;
}

[[maybe_unused]] uint32_t hash_key(std::span<char> key)
{
    uint32_t hash = 0U;
    if (key.size() >= 5) {
        std::memcpy(&hash, &key[1], 4);
    }
    return hash;
}

int final_exception(std::string_view prefix, std::string_view msg)
{
    try {
        log_msg(log_level::emerg) << prefix << msg;
    } catch (std::exception& e) {
    } catch (...) {
    }
    std::cout << prefix << msg << std::endl;
    return EXIT_FAILURE;
}

int usage(const std::filesystem::path& argv0, bool ok = false)
{
    if (!ok)
        log_msg(log_level::crit) << "Invalid command line arguments";
    std::cout << "usage: " << argv0.filename().native() <<
        "[-l LOG_LEVEL] COMMAND [ARGS]\n" << R"(
-l LOG_LEVEL
    Set log level, one of -|off, X|emerg|emergency, A|alert, C|crit|critical,
    E|err|error, W|warn|warning, N|notice, I|info (default), D|debug.

Commands:

create PATH PARTITIONS RECORDS VALUE_SZ CACHE_KIB [THREADS]

    Create databases for benchmarking.

    PATH
        The path and the initial part of filename for database files
    PARTITIONS
        Split the database into this number of partitions.
    RECORDS
        The total number of records, together in all partitions
    VALUE_SZ
        The size of the variable-size part of each record
    CACHE_KIB
        The approximate amount of memory (in KiB) to use as the database cache.
        It will be split among caches for individual database files.
    THREADS
        The number of worker threads. If unset then the number of threads will
        be equal to the number of CPUs.

help

    Display this help message and exit.

requests URI REQUESTS INSERTS UPDATES VALUE_SZ [THREADS]

    Execute database get/insert/update operations.

    URI
        The base URI of the server where requests will be sent.
    REQUESTS
        The number of requests to execute
    INSERTS
        The fraction of insert operations in QUERIES.
    UPDATES
        The fraction of update operations in QUERIES.
    VALUE_SZ
        The size of the variable-size part of each record
    THREADS
        The number of worker threads. If unset then the number of threads will
        be equal to the number of CPUs.
)" << std::endl;
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

namespace cmd {

/*** create ******************************************************************/

namespace create_impl {

struct create_args {
    std::string path;
    unsigned partitions;
    unsigned long records;
    size_t value_sz;
    unsigned cache_kib;
    unsigned threads;
};

class worker;

struct db_record {
    struct thread_init {};
    db_record() = default;
    explicit db_record(thread_init) {
        pid_t pid = getpid();
        pthread_t tid = pthread_self();
        key.resize(key_sz);
        static_assert(key_sz >= sizeof(pid) + sizeof(tid));
        memmove(key.data(), &pid, sizeof(pid));
        memmove(key.data() + sizeof(pid), &tid, sizeof(tid));
    }
    void update(size_t partitions, size_t value_sz);
    unsigned partition = 0;
    uint32_t hash = 0;
    int64_t counter = 0;
    std::string key = {};
    std::string value = {};
};

void db_record::update(size_t partitions, size_t value_sz)
{
    static_assert(key_sz == SHA256_DIGEST_LENGTH);
    char buf[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<unsigned char*>(key.data()), key.size(), reinterpret_cast<unsigned char*>(buf));
    key.resize(key_sz);
    memcpy(key.data(), buf, key_sz);
    partition = partition_key(key) % partitions;
    hash = hash_key(key);
    value.resize(value_sz);
    for (size_t i = 0; i < value.size() / key_sz; ++i)
        memcpy(value.data() + i * key_sz, key.data(), key_sz);
}

class partition {
public:
    // Data file in SQLite3 format
    static constexpr std::string_view path_suffix_data = "_data.sqlite"sv;
    // Index file in SQLite3 format
    static constexpr std::string_view path_suffix_idx = "_idx.sqlite"sv;
    // List of keys, concatenated partition indices and binary keys, 1+4 B each, used to obtain
    // records for update operations
    static constexpr std::string_view path_suffix_keys = "_keys.bin"sv;
    class exists: public std::runtime_error {
    public:
        explicit exists(std::string_view path):
            std::runtime_error(std::string("Database file \"").append(path).append(" already exists")) {}
    };
    class keys_write: public std::runtime_error {
    public:
        explicit keys_write(std::string_view path):
            std::runtime_error(std::string("Cannot writel keys file \"").append(path).append("\"")) {}
    };
    partition(asio::io_context& ctx, const create_args& args, unsigned idx, bool create);
    partition(const partition&) = delete;
    partition(partition&&) = delete;
    ~partition();
    partition& operator=(const partition&) = delete;
    partition& operator=(partition&&) = delete;
    static std::string path_idx(std::string_view path, unsigned idx) {
        return (std::ostringstream{} << path << std::setw(3) << std::setfill('0') << idx).str();
    }
    std::string path_data() const {
        return std::string{path}.append(path_suffix_data);
    }
    std::string path_idx() const {
        return std::string{path}.append(path_suffix_idx);
    }
    std::string path_keys() const {
        return std::string{path}.append(path_suffix_keys);
    }
    void handle_create(worker& wrk, db_record rec);
    asio::io_context::strand strand;
    const create_args& args;
    const unsigned idx;
private:
    std::string path;
    std::optional<sqlite::connection> db;
    std::optional<sqlite::query> insert_data;
    std::optional<sqlite::query> insert_idx;
    FILE* keys_file = nullptr;
};

using partitions = std::vector<std::unique_ptr<partition>>;

class worker {
public:
    using clock = std::chrono::steady_clock;
    class time_ref {
    public:
        explicit time_ref(worker& w): w(w) {}
        std::pair<clock::duration, clock::duration> duration() const {
            if (w.t0 == clock::time_point{})
                return {clock::duration{}, clock::duration{}};
            auto t = clock::now();
            auto d0 = t - w.t0;
            auto d1 = t - w.t1;
            w.t1 = t;
            return {d1, d0};
        }
    private:
        worker& w;
    };
    explicit worker(partitions& parts): parts(parts) {}
    void init(unsigned long requests);
    void create_record();
    time_ref time() {
        return time_ref(*this);
    }
    bool is_done();
    void log_end() {
        log_msg(log_level::info) << "End records=" << records << " time=" << time();
    }
private:
    std::atomic<unsigned long> records = 0;
    partitions& parts;
    clock::time_point t0{};
    clock::time_point t1{};
};

partition::partition(asio::io_context& ctx, const create_args& args, unsigned idx, bool create):
    strand{ctx}, args{args}, idx{idx}, path{path_idx(args.path, idx)}
{
    if (create) {
        if (std::filesystem::exists(path_data()))
            throw exists(path_data());
        sqlite::connection data(path_data(), true);
        if (std::filesystem::exists(path_idx()))
            throw exists(path_idx());
        if (std::filesystem::exists(path_keys()))
            throw exists(path_keys());
        sqlite::connection idx(path_idx(), true);
    }
    db.emplace(path_data());
    sqlite::query(*db, R"(attach database ?1 as idx)").start().bind(0, path_idx()).next_row();
    if (create) {
        // speed up populating the database by turning off journaling and filesystem syncing
        sqlite::query(*db, R"(pragma main.journal_mode = off)").start().next_row();
        sqlite::query(*db, R"(pragma idx.journal_mode = off)").start().next_row();
        sqlite::query(*db, R"(pragma main.synchronous = off)").start().next_row();
        sqlite::query(*db, R"(pragma idx.synchronous = off)").start().next_row();
        // configure page sizes for data and index database files
        int64_t page_size = 4096;
        if (args.value_sz < 3900)
            page_size = 2048;
        if (args.value_sz < 1900)
            page_size = 1024;
        if (args.value_sz < 400)
            page_size = 512;
        sqlite::query(*db, R"(pragma main.page_size = )" + std::to_string(page_size)).start().next_row();
        sqlite::query(*db, R"(pragma idx.page_size = 512)").start().next_row();
        // create tables
        sqlite::query(*db, R"(
            create table main.data
                (key blob not null, hash int not null, counter int not null default 0, value blob not null default '')
            )").start().next_row();
        sqlite::query(*db,
            R"(create table idx.idx (hash int not null, id int not null, primary key (hash, id)) without rowid)").
            start().next_row();
    }
    // Configure page caches for data and index database files. Cache size is
    // internally stored as the number of pages, therefore we must set it after
    // setting the page size. It is not remembered, therefore we must set it
    // every time the database is opened.
    double cache_sz = double(args.cache_kib) / args.partitions;
    double data_record = 8.0 /* id+overhead */ + key_sz /* key */ + 4 /* hash of key */ + 8 /* counter */ +
        double(args.value_sz) /* data */;
    double idx_record = 8.0 /* overhead */ + 4.0 /* hash of key */ + 4 /* record id */;
    sqlite::query(*db, R"(pragma main.cache_size = )" +
                  std::to_string(int64_t(-cache_sz * data_record / (data_record + idx_record)))).start().next_row();
    sqlite::query(*db, R"(pragma idx.cache_size = )" +
                  std::to_string(int64_t(-cache_sz * idx_record / (data_record + idx_record)))).start().next_row();
    if (keys_file = fopen(path_keys().c_str(), "ab"); !keys_file)
        throw keys_write(path_keys());
    // Prepare queries for inserting data
    insert_data.emplace(*db, R"(insert into main.data values (?1, ?2, ?3, ?4) returning oid)");
    insert_idx.emplace(*db, R"(insert into idx.idx values (?1, ?2))");
}

partition::~partition()
{
    // enable WAL for regular database operation
    if (db) {
        sqlite::query(*db, R"(pragma main.journal_mode = wal)").start().next_row();
        sqlite::query(*db, R"(pragma idx.journal_mode = wal)").start().next_row();
    }
    if (keys_file)
        (void)fclose(keys_file);
}

void partition::handle_create(worker& wrk, db_record rec)
{
    log_msg(log_level::debug) << "Add record for partition " << rec.partition << " by partition handler " << idx;
    assert(insert_data);
    assert(insert_idx);
    assert(insert_data->start().bind_blob(0, rec.key).bind(1, int64_t(rec.hash)).bind(2, rec.counter).
           bind_blob(3, rec.value).next_row() == sqlite::query::status::row);
    assert(insert_data->column_count() == 1);
    insert_idx->start().bind(0, int64_t(rec.hash)).bind(1, std::get<int64_t>(insert_data->get_column(0)));
    assert(insert_data->next_row() == sqlite::query::status::done);
    assert(insert_idx->next_row() == sqlite::query::status::done);
    auto part = static_cast<unsigned char>(rec.partition);
    if (fwrite(&part, sizeof(part), 1, keys_file) != 1 ||
        fwrite(&rec.hash, sizeof(rec.hash), 1, keys_file) != 1)
    {
        throw keys_write(path_keys());
    }
    if (!wrk.is_done()) 
        wrk.create_record();
}

std::ostream& operator<<(std::ostream& os, const worker::time_ref& t)
{
    auto [d0, d1] = t.duration();
    using fsec = std::chrono::duration<double, std::chrono::seconds::period>;
    os << std::fixed << std::setprecision(6) <<
        std::chrono::duration_cast<fsec>(d0).count() << '/' << std::chrono::duration_cast<fsec>(d1).count();
    return os;
}

void worker::create_record()
{
    static thread_local db_record rec{db_record::thread_init{}};
    rec.update(parts.size(), parts[0]->args.value_sz);
    asio::post(asio::bind_executor(parts[rec.partition]->strand,
        [this, &p = *parts[rec.partition], rec = rec]() mutable {
            p.handle_create(*this, std::move(rec));
        }));
}

void worker::init(unsigned long requests)
{
    for (unsigned long r = 0; r < requests; ++r)
        create_record();
    t0 = std::chrono::steady_clock::now();
    t1 = t0;
}

bool worker::is_done()
{
    auto r = ++records;
    if (r % create_log_interval == 0)
        log_msg(log_level::info) << "Created " << r << " records time=" << time();
    return r >= parts[0]->args.records;
}

} // namespace create_impl

int create(std::string_view argv0, [[maybe_unused]] std::string_view cmd, std::span<const std::string_view> args)
{
    using namespace create_impl;
    // Process command line arguments
    if (args.size() < 5 || args.size() > 6)
        return usage(argv0);
    create_args cargs{};
    cargs.path = args[0];
    if (auto r = std::from_chars(args[1].begin(), args[1].end(), cargs.partitions);
        r.ec != std::errc{} || cargs.partitions < partitions_min || cargs.partitions > partitions_max)
    {
        log_msg(log_level::crit) << "Number of partitions (PARTITIONS) must be between " << partitions_min <<
            " and " << partitions_max;
        return usage(argv0);
    }
    if (auto r = std::from_chars(args[2].begin(), args[2].end(), cargs.records);
        r.ec != std::errc{} || cargs.records < 1)
    {
        log_msg(log_level::crit) << "Number of records (RECORDS) must be at least 1";
        return usage(argv0);
    }
    if (auto r = std::from_chars(args[3].begin(), args[3].end(), cargs.value_sz);
        r.ec != std::errc{} || cargs.value_sz < value_sz_min || cargs.value_sz > value_sz_max)
    {
        log_msg(log_level::crit) << "Value size (VALUE_SZ) must be between " << value_sz_min << " and " <<
            value_sz_max;
        return usage(argv0);
    }
    if (auto r = std::from_chars(args[4].begin(), args[4].end(), cargs.cache_kib);
        r.ec != std::errc{})
    {
        log_msg(log_level::crit) << "Invalid number of KiB of memory to use as the page cache";
        return usage(argv0);
    }
    if (args.size() >= 6) {
        if (auto r = std::from_chars(args[5].begin(), args[5].end(), cargs.threads);
            r.ec != std::errc{} || cargs.threads < 1)
        {
            log_msg(log_level::crit) << "Number of threads (THREADS) must be at least 1";
            return usage(argv0);
        }
    } else {
        cargs.threads = std::thread::hardware_concurrency();
        assert(cargs.threads > 0);
    }
    log_msg(log_level::info) << "Creating database with partitions=" << cargs.partitions <<
        " records=" << cargs.records << " value_sz=" << cargs.value_sz <<
        " cache_kib=" << cargs.cache_kib <<" threads=" << cargs.threads;
    // Create database files
    thread_pool threads(int(cargs.threads), "main");
    partitions db{};
    db.reserve(cargs.partitions);
    for (decltype(cargs.partitions) part = 0; part < cargs.partitions; ++part) {
        log_msg(log_level::info) << "Creating partition " << part;
        try {
            db.push_back(std::make_unique<partition>(threads.ctx, cargs, part, true));
        } catch (const partition::exists& e) {
            log_msg(log_level::crit) << e.what();
            return EXIT_FAILURE;
        }
    }
    // Generate data
    worker wrk(db);
    auto requests = std::min(create_queue_max, cargs.records);
    wrk.init(requests);
    log_msg(log_level::info) << "Begin with " << requests << " parallel requests time=" << wrk.time();
    threads.run(false);
    threads.wait();
    wrk.log_end();
    return EXIT_SUCCESS;
}

/*** help ********************************************************************/

int help(std::string_view argv0, [[maybe_unused]] std::string_view cmd,
         [[maybe_unused]] std::span<const std::string_view> args)
{
    return usage(argv0, true);
}

/*** requests ****************************************************************/

namespace requests_impl {

struct requests_args {
    std::string uri;
    unsigned long requests;
    double inserts;
    double updates;
    size_t value_sz;
    unsigned threads;
};

} // namespace requests_impl

int requests(std::string_view argv0, [[maybe_unused]] std::string_view cmd,
             std::span<const std::string_view> args)
{
    using namespace requests_impl;
    // Process command line arguments
    if (args.size() < 5 || args.size() > 6)
        return usage(argv0);
    requests_args rargs{};
    rargs.uri = args[0];
    if (auto r = std::from_chars(args[1].begin(), args[1].end(), rargs.requests); r.ec != std::errc{}) {
        log_msg(log_level::crit) << "Invalid number of requests to be executed";
        return usage(argv0);
    }
    if (auto r = std::from_chars(args[2].begin(), args[2].end(), rargs.inserts);
        r.ec != std::errc{} || rargs.inserts < 0.0 || rargs.inserts > 1.0)
    {
        log_msg(log_level::crit) << "Fraction of INSERT requests must be between 0.0 and 1.0";
        return usage(argv0);
    }
    if (auto r = std::from_chars(args[3].begin(), args[3].end(), rargs.updates);
        r.ec != std::errc{} || rargs.updates < 0.0 || rargs.updates > 1.0)
    {
        log_msg(log_level::crit) << "Fraction of UPDATE requests must be between 0.0 and 1.0";
        return usage(argv0);
    }
    if (rargs.inserts + rargs.updates > 1.0) {
        log_msg(log_level::crit) << "Sum of INSERT and UPDATE must be at most 1.0";
        return usage(argv0);
    }
    if (auto r = std::from_chars(args[4].begin(), args[4].end(), rargs.value_sz);
        r.ec != std::errc{} || rargs.value_sz < value_sz_min || rargs.value_sz > value_sz_max)
    {
        log_msg(log_level::crit) << "Value size (VALUE_SZ) must be between " << value_sz_min << " and " <<
            value_sz_max;
        return usage(argv0);
    }
    if (args.size() >= 6) {
        if (auto r = std::from_chars(args[5].begin(), args[5].end(), rargs.threads);
            r.ec != std::errc{} || rargs.threads < 1)
        {
            log_msg(log_level::crit) << "Number of threads (THREADS) must be at least 1";
            return usage(argv0);
        }
    } else {
        rargs.threads = std::thread::hardware_concurrency();
        assert(rargs.threads > 0);
    }
    // Prepare threaded HTTP client
    // Run requests
    // TODO
    return EXIT_SUCCESS;
}

} // namespace cmd

/*** main ********************************************************************/

int run(std::span<const std::string_view> argv)
{
    auto argv0 = argv[0];
    argv = argv.subspan(1);
    if (argv.size() >= 1 && argv[0] == "-l") {
        if (argv.size() < 2) {
            log_msg(log_level::crit) << "A value expected for option -l";
            return usage(argv0);
        }
        if (auto l = from_string(argv[1]))
            logger::global().level(*l);
        else {
            log_msg(log_level::crit) << "Unknown log level \"" << argv[1] << "\"";
            return usage(argv0);
        }
        argv = argv.subspan(2);
    }
    if (argv.size() < 1)
        return usage(argv0);
    auto cmd = argv[0];
    auto cmd_args = argv.subspan(1);
    log_msg(log_level::notice) << "Running command \"" << cmd <<"\"";
    if (argv[0] == "create")
        return cmd::create(argv0, cmd, cmd_args);
    if (argv[0] == "help")
        return cmd::help(argv0, cmd, cmd_args);
    if (argv[0] == "requests")
        return cmd::requests(argv0, cmd, cmd_args);
    log_msg(log_level::crit) << "Unknown command \"" << cmd << "\"";
    return usage(argv0);
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        int result = run(std::vector<std::string_view>(argv, argv + argc));
        if (result == EXIT_SUCCESS)
            log_msg(log_level::notice) << "Terminating successfully";
        else
            log_msg(log_level::crit) << "Terminating after error";
        return result;
    } catch (std::exception& e) {
        return final_exception("Terminated by unhandled exception: ", e.what());
    } catch (...) {
        return final_exception("Terminated by unhandled unknown exception", "");
    }
}
