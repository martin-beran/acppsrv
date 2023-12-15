#include "finally.hpp"
#include "log.hpp"
#include "sqlite3.hpp"
#include "worker.hpp"

#include <boost/asio.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/beast.hpp>

#include <openssl/sha.h>

#include <sys/mman.h>

#include <charconv>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>
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
// The maximum number of outstanding database requests
constexpr unsigned long request_queue_max = 1'000;
// A duration message will be logged each time this number of new recods is created
constexpr unsigned long create_log_interval = 100'000;
constexpr unsigned transaction_sz = 100;

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
        " [-l LOG_LEVEL] COMMAND [ARGS]\n" << R"(
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

requests PATH KEYS PARTITIONS TIME INSERTS UPDATES VALUE_SZ CACHE_KIB [THREADS]

    Execute database get/insert/update operations.

    PATH
        The path and the initial part of filename for database files
    KEYS
        The path to the file containing concatenated binary values of partition
        index and key hash for every record in the database. It is used to
        quickly select existing records.
    PARTITIONS
        Number of database partitions
    TIME
        How long to generate requests, in seconds
    INSERTS
        The fraction of insert operations in QUERIES
    UPDATES
        The fraction of update operations in QUERIES
    VALUE_SZ
        The size of the variable-size part of each record
    CACHE_KIB
        The approximate amount of memory (in KiB) to use as the database cache.
        It will be split among caches for individual database files.
    THREADS
        The number of worker threads. If unset then the number of threads will
        be equal to the number of CPUs.
)" << std::endl;
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

namespace cmd {

namespace requests_impl {
class worker;
} // namespace requests_impl

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

class partition {
public:
    // Data file in SQLite3 format
    static constexpr std::string_view path_suffix_data = "_data.sqlite"sv;
    // Index file in SQLite3 format
    static constexpr std::string_view path_suffix_idx = "_idx.sqlite"sv;
    // List of keys, concatenated partition indices and hashes of keys, 1+4 B each, used to obtain
    // records for update operations
    static constexpr std::string_view path_suffix_keys = "_keys.bin"sv;
    class exists: public std::runtime_error {
    public:
        explicit exists(std::string_view path):
            std::runtime_error(std::string("Database file \"").append(path).append(" already exists")) {}
    };
    class keys_write: public std::system_error {
    public:
        explicit keys_write(std::string_view path):
            std::system_error(std::error_code(errno, std::generic_category()),
                              std::string("Cannot write keys file \"").append(path).append("\"")) {}
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
    void handle_get(requests_impl::worker& wrk, size_t hash);
    void handle_insert(requests_impl::worker& wrk, db_record rec);
    void handle_update(requests_impl::worker& wrk, size_t hash);
    asio::io_context::strand strand;
    const create_args& args;
    const unsigned idx;
protected:
    std::string path;
    std::optional<sqlite::connection> db;
    std::optional<sqlite::query> insert_data;
    std::optional<sqlite::query> insert_idx;
    std::optional<sqlite::query> begin_transaction;
    std::optional<sqlite::query> commit_transaction;
    std::optional<sqlite::query> request_get;
    std::optional<sqlite::query> request_update;
    unsigned cur_tx = 0;
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
        std::pair<bool, clock::duration> test() const {
            auto tnow = clock::now();
            auto d = tnow - w.t0;
            auto t = w.tt.exchange(clock::time_point{});
            if (t == clock::time_point{})
                return {false, d};
            if (tnow - t >= std::chrono::seconds{1}) {
                w.tt = tnow;
                return {true, d};
            } else {
                w.tt = t;
                return {false, d};
            }
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
protected:
    std::atomic<unsigned long> records = 0;
    partitions& parts;
    clock::time_point t0{};
    clock::time_point t1{};
    std::atomic<clock::time_point> tt{};
};

/*** db_record ***/

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

/*** partition ***/

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
    if (create) {
        sqlite::query(*db, R"(attach database ?1 as idx)").start().bind(0, path_idx()).next_row();
        // speed up populating the database by turning off journaling and filesystem syncing
        sqlite::query(*db, R"(pragma main.journal_mode = off)").start().next_row();
        sqlite::query(*db, R"(pragma idx.journal_mode = off)").start().next_row();
        sqlite::query(*db, R"(pragma main.synchronous = off)").start().next_row();
        sqlite::query(*db, R"(pragma idx.synchronous = off)").start().next_row();
        sqlite::query(*db, R"(pragma main.locking_mode = exclusive)").start().next_row();
        sqlite::query(*db, R"(pragma idx.locking_mode = exclusive)").start().next_row();
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
    // run faster by tolerating lost transactions on system crash
    sqlite::query(*db, R"(pragma main.synchronous = normal)").start().next_row();
    //sqlite::query(*db, R"(pragma idx.synchronous = normal)").start().next_row();
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
    //sqlite::query(*db, R"(pragma idx.cache_size = )" +
    //              std::to_string(int64_t(-cache_sz * idx_record / (data_record + idx_record)))).start().next_row();
    if (keys_file = fopen(path_keys().c_str(), "ab"); !keys_file)
        throw keys_write(path_keys());
    // Prepare queries for inserting data
    insert_data.emplace(*db, R"(insert into main.data values (?1, ?2, ?3, ?4))");
    //insert_idx.emplace(*db, R"(insert into idx.idx values (?1, ?2))");
    begin_transaction.emplace(*db, R"(begin)");
    commit_transaction.emplace(*db, R"(commit)");
    // Prepare queries for requests into existing database
    request_get.emplace(*db,  R"(select hex(key), counter, hex(value) from data where hash = ?1 limit 1)");
    request_update.emplace(*db, R"(update data set counter = counter + 1 where hash = ?1 returning hex(key), counter)");
}

partition::~partition()
{
    // enable WAL for regular database operation
    if (db) {
        sqlite::query(*db, R"(pragma main.journal_mode = wal)").start().next_row();
        //sqlite::query(*db, R"(pragma idx.journal_mode = wal)").start().next_row();
    }
    if (keys_file)
        (void)fclose(keys_file);
}

void partition::handle_create(worker& wrk, db_record rec)
{
    assert(insert_data);
    //assert(insert_idx);
    assert(begin_transaction);
    assert(commit_transaction);
    if (cur_tx == 0) {
        log_msg(log_level::debug) << "Begin transaction";
        assert(begin_transaction->start().next_row() == sqlite::query::status::done);
    }
    log_msg(log_level::debug) << "Add record for partition " << rec.partition << " by partition handler " << idx;
    assert(insert_data->start().bind_blob(0, rec.key).bind(1, int64_t(rec.hash)).bind(2, rec.counter).
           bind_blob(3, rec.value).next_row() == sqlite::query::status::done);
    auto part = static_cast<unsigned char>(rec.partition);
    if (fwrite(&part, sizeof(part), 1, keys_file) != 1 ||
        fwrite(&rec.hash, sizeof(rec.hash), 1, keys_file) != 1)
    {
        throw keys_write(path_keys());
    }
    if (!wrk.is_done()) {
        if (++cur_tx == transaction_sz) {
            log_msg(log_level::debug) << "Commit transaction";
            assert(commit_transaction->start().next_row() == sqlite::query::status::done);
            cur_tx = 0;
        }
        wrk.create_record();
    } else {
        log_msg(log_level::debug) << "Commit transaction";
        assert(commit_transaction->start().next_row() == sqlite::query::status::done);
    }
}

std::ostream& operator<<(std::ostream& os, const worker::time_ref& t)
{
    auto [d0, d1] = t.duration();
    using fsec = std::chrono::duration<double, std::chrono::seconds::period>;
    os << std::fixed << std::setprecision(6) <<
        std::chrono::duration_cast<fsec>(d0).count() << '/' << std::chrono::duration_cast<fsec>(d1).count();
    return os;
}

/*** worker ***/

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
    tt = t0;
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
    // Process command line arguments. See a more defensive method of iterating
    // over arguments in cmd::requests().
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
        r.ec != std::errc{} || cargs.value_sz < value_sz_min)
    {
        log_msg(log_level::crit) << "Value size (VALUE_SZ) must at least " << value_sz_min;
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
    auto requests = std::min(request_queue_max, cargs.records);
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

struct requests_args: create_impl::create_args {
    std::string keys;
    std::chrono::steady_clock::duration time;
    double inserts;
    double updates;
};

class known_key_generator {
public:
    struct key {
        unsigned partition;
        size_t hash;
    };
    class keys_mmap: public std::system_error {
    public:
        explicit keys_mmap(std::string_view path):
            std::system_error(std::error_code(errno, std::generic_category()),
                              std::string("Cannot mmap keys file \"").append(path).append("\"")) {}
    };
    explicit known_key_generator(const std::filesystem::path& path);
    ~known_key_generator();
    key get_key();
    void add(const key& k) {
        std::lock_guard lck(mtx);
        added.push_back(k);
    }
private:
    static constexpr size_t key_sz = sizeof(unsigned char) + sizeof(uint32_t);
    const unsigned char* data;
    size_t data_sz;
    size_t sz;
    std::vector<key> added;
    std::mt19937_64 rnd;
    std::mutex mtx;
};

class worker: public create_impl::worker {
public:
    enum class request {
        get,
        update,
        insert,
    };
    explicit worker(create_impl::partitions& parts):
        create_impl::worker(parts), known_keys(static_cast<const requests_args&>(parts[0]->args).keys),
        rnd{std::random_device{}()}
    {}
    void init(unsigned long requests);
    void create_request();
    void request_get();
    void request_update();
    void request_insert();
    bool is_done(request req);
    void log_end() {
        log_msg(log_level::info) << "End requests=" << records <<
            " get=" << gets << " update=" << updates << " insert=" << inserts << " time=" << time();
    }
private:
    double get_random();
    known_key_generator known_keys;
    std::mt19937_64 rnd;
    std::uniform_real_distribution<> rnd_dist;
    std::mutex mtx;
    std::atomic<unsigned long> records1 = 0;
    std::atomic<unsigned long> gets = 0;
    std::atomic<unsigned long> gets1 = 0;
    std::atomic<unsigned long> updates = 0;
    std::atomic<unsigned long> updates1 = 0;
    std::atomic<unsigned long> inserts = 0;
    std::atomic<unsigned long> inserts1 = 0;
};

/*** known_key_generator ***/

known_key_generator::known_key_generator(const std::filesystem::path& path): rnd{std::random_device{}()}
{
    int fd = -1;
    util::finally fd_close([&fd]() {
        if (fd >= 0)
            close(fd);
    });
    if (fd = open(path.c_str(), O_RDONLY); fd < 0)
        throw keys_mmap(path.native());
    data_sz = std::filesystem::file_size(path.c_str());
    if (data = reinterpret_cast<unsigned char* >(mmap(nullptr, data_sz, PROT_READ, MAP_SHARED, fd, 0));
        data == MAP_FAILED)
    {
        throw keys_mmap(path.native());
    }
    sz = data_sz / key_sz;
    log_msg(log_level::notice) << "Mapped file \"" << path.native() << "\" size=" << data_sz <<
        " containing " << sz << " keys at " << static_cast<const void*>(data);
}

known_key_generator::~known_key_generator()
{
    if (data)
        munmap(const_cast<void*>(static_cast<const void*>(data)), data_sz);
}

known_key_generator::key known_key_generator::get_key()
{
    std::lock_guard lck(mtx);
    size_t i = rnd() % (sz + added.size());
    if (i >= sz)
        return added[i - sz];
    else {
        key result;
        result.partition = data[i * key_sz];
        uint32_t hash;
        std::memcpy(&hash, data + i * key_sz + 1, sizeof(hash));
        result.hash = hash;
        return result;
    }
}

/*** worker ***/

void worker::create_request()
{
    auto& args = static_cast<const requests_args&>(parts[0]->args);
    auto r = get_random();
    if (r < args.inserts)
        request_insert();
    else if (r <  args.inserts + args.updates)
        request_update();
    else
        request_get();
}

double worker::get_random()
{
    std::lock_guard lck(mtx);
    return rnd_dist(rnd);
}

void worker::init(unsigned long requests)
{
    for (unsigned long r = 0; r < requests; ++r)
        create_request();
    t0 = std::chrono::steady_clock::now();
    t1 = t0;
    tt = t0;
}

bool worker::is_done(request req)
{
    ++records;
    ++records1;
    switch (req) {
    case request::get:
        ++gets;
        ++gets1;
        break;
    case request::update:
        ++updates;
        ++updates1;
        break;
    case request::insert:
        ++inserts;
        ++inserts1;
        break;
    default:
        break;
    }
    auto [test, duration] = time_ref{*this}.test();
    if (test) {
        log_msg(log_level::info) << "Processed " << records1 << " requests " <<
            " get=" << gets1 << " update=" << updates1 << " insert=" << inserts1 << " time=" << time();
        records1 = 0;
        gets1 = 0;
        updates1 = 0;
        inserts1 = 0;
    }
    return duration >= static_cast<const requests_args&>(parts[0]->args).time;
}

void worker::request_get()
{
    auto [partition, hash] = known_keys.get_key();
    asio::post(asio::bind_executor(parts[partition]->strand,
        [this, &p = *parts[partition], hash = hash]() {
            p.handle_get(*this, hash);
        }));
}

void worker::request_insert()
{
    static thread_local create_impl::db_record rec{create_impl::db_record::thread_init{}};
    rec.update(parts.size(), parts[0]->args.value_sz);
    asio::post(asio::bind_executor(parts[rec.partition]->strand,
        [this, &p = *parts[rec.partition], rec = rec]() mutable {
            p.handle_insert(*this, std::move(rec));
        }));
}

void worker::request_update()
{
    auto [partition, hash] = known_keys.get_key();
    asio::post(asio::bind_executor(parts[partition]->strand,
        [this, &p = *parts[partition], hash = hash]() {
            p.handle_update(*this, hash);
        }));
}

} // namespace requests_impl

namespace create_impl {

/*** partition ***/

void partition::handle_get(requests_impl::worker& wrk, size_t hash)
{
    assert(request_get);
    auto s = request_get->start().bind(0, int64_t(hash)).next_row();
    switch (s) {
    case sqlite::query::status::row:
        assert(request_get->column_count() == 3);
        log_msg(log_level::debug) << "SELECT partition=" << idx << " hash=" << hash << " returned key=" <<
            std::get<3>(request_get->get_column(0)) << " counter=" << std::get<int64_t>(request_get->get_column(1)) <<
            " value=" << std::get<3>(request_get->get_column(2)).substr(0, 32) << "...";
        assert(request_get->next_row() == sqlite::query::status::done);
        break;
    case sqlite::query::status::done:
        log_msg(log_level::debug) << "SELECT partition=" << idx << " hash=" << hash << " no row";
        break;
    case sqlite::query::status::locked:
    default:
        assert(false);
    }
    if (!wrk.is_done(requests_impl::worker::request::get))
        wrk.create_request();
}

void partition::handle_insert(requests_impl::worker& wrk, db_record rec)
{
    assert(insert_data);
    log_msg(log_level::debug) << "INSERT partition=" << idx << " hash=" << rec.hash;
    assert(insert_data->start().bind_blob(0, rec.key).bind(1, int64_t(rec.hash)).bind(2, rec.counter).
           bind_blob(3, rec.value).next_row() == sqlite::query::status::done);
    if (!wrk.is_done(requests_impl::worker::request::insert))
        wrk.create_request();
}

void partition::handle_update(requests_impl::worker& wrk, size_t hash)
{
    assert(request_update);
    auto s = request_update->start().bind(0, int64_t(hash)).next_row();
    switch (s) {
    case sqlite::query::status::row:
        assert(request_update->column_count() == 2);
        {
            int n = 1;
            auto key = std::get<3>(request_update->get_column(0));
            auto counter = std::get<int64_t>(request_update->get_column(1));
            while (request_update->next_row() == sqlite::query::status::row)
                ++n;
        log_msg(log_level::debug) << "UPDATE partition=" << idx << " hash=" << hash << " rows=" << n <<
            " key=" << key << " counter=" << counter;
        }
        break;
    case sqlite::query::status::done:
        log_msg(log_level::debug) << "UPDATE partition=" << idx << " hash=" << hash << " no row";
        break;
    case sqlite::query::status::locked:
    default:
        assert(false);
    }
    if (!wrk.is_done(requests_impl::worker::request::update))
        wrk.create_request();
}

} // namespace create_impl

int requests(std::string_view argv0, [[maybe_unused]] std::string_view cmd,
             std::span<const std::string_view> args)
{
    using namespace create_impl;
    using namespace requests_impl;
    // Process command line arguments. This is a more defensive method than in cmd::create()
    auto it = args.end();
    struct args_count {};
    auto next_arg = [&it, &args] {
        if (it == args.end())
            it = args.begin();
        else
            ++it;
        if (it == args.end())
            throw args_count{};
        return it;
    };
    requests_args rargs{};
    try {
        rargs.path = *next_arg();
        rargs.keys = *next_arg();
        next_arg();
        if (auto r = std::from_chars(it->begin(), it->end(), rargs.partitions);
            r.ec != std::errc{} || rargs.partitions < partitions_min || rargs.partitions > partitions_max)
        {
            log_msg(log_level::crit) << "Number of partitions (PARTITIONS) must be between " << partitions_min <<
                " and " << partitions_max;
            return usage(argv0);
        }
        next_arg();
        if (unsigned v; std::from_chars(it->begin(), it->end(), v).ec != std::errc{} || v < 1) {
            log_msg(log_level::crit) << "Invalid number of seconds for test duration";
            return usage(argv0);
        } else
            rargs.time = std::chrono::duration_cast<decltype(rargs.time)>(std::chrono::seconds(v));
        next_arg();
        if (auto r = std::from_chars(it->begin(), it->end(), rargs.inserts);
            r.ec != std::errc{} || rargs.inserts < 0.0 || rargs.inserts > 1.0)
        {
            log_msg(log_level::crit) << "Fraction of INSERT requests must be between 0.0 and 1.0";
            return usage(argv0);
        }
        next_arg();
        if (auto r = std::from_chars(it->begin(), it->end(), rargs.updates);
            r.ec != std::errc{} || rargs.updates < 0.0 || rargs.updates > 1.0)
        {
            log_msg(log_level::crit) << "Fraction of UPDATE requests must be between 0.0 and 1.0";
            return usage(argv0);
        }
        if (rargs.inserts + rargs.updates > 1.0) {
            log_msg(log_level::crit) << "Sum of INSERT and UPDATE must be at most 1.0";
            return usage(argv0);
        }
        next_arg();
        if (auto r = std::from_chars(it->begin(), it->end(), rargs.value_sz);
            r.ec != std::errc{} || rargs.value_sz < value_sz_min)
        {
            log_msg(log_level::crit) << "Value size (VALUE_SZ) must be at least " << value_sz_min;
            return usage(argv0);
        }
        next_arg();
        if (auto r = std::from_chars(it->begin(), it->end(), rargs.cache_kib);
            r.ec != std::errc{})
        {
            log_msg(log_level::crit) << "Invalid number of KiB of memory to use as the page cache";
            return usage(argv0);
        }
        if (++it != args.end()) {
            if (auto r = std::from_chars(it->begin(), it->end(), rargs.threads);
                r.ec != std::errc{} || rargs.threads < 1)
            {
                log_msg(log_level::crit) << "Number of threads (THREADS) must be at least 1";
                return usage(argv0);
            }
            ++it;
        } else {
            rargs.threads = std::thread::hardware_concurrency();
            assert(rargs.threads > 0);
        }
        if (it != args.end())
            throw args_count{};
    } catch (const args_count&) {
        return usage(argv0);
    }
    // Open database files
    thread_pool threads(int(rargs.threads), "main");
    partitions db{};
    db.reserve(rargs.partitions);
    for (decltype(rargs.partitions) part = 0; part < rargs.partitions; ++part) {
        log_msg(log_level::info) << "Opening partition " << part;
        db.push_back(std::make_unique<partition>(threads.ctx, rargs, part, false));
    }
    // Run requests
    requests_impl::worker wrk(db);
    wrk.init(request_queue_max);
    log_msg(log_level::info) << "Begin with " << request_queue_max << " parallel requests time=" << wrk.time();
    threads.run(false);
    threads.wait();
    wrk.log_end();
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
