#include "log.hpp"
#include "sqlite3.hpp"

#include <charconv>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

using namespace acppsrv;
using namespace std::string_literals;
using namespace std::string_view_literals;

namespace {

constexpr unsigned partitions_min = 1;
constexpr unsigned partitions_max = 256;
constexpr size_t value_sz_min = 1;
constexpr size_t value_sz_max = 4096;

// We expect key to be SHA256, that is, essentially 32 random bytes, so that we
// can take independent subspans of it as the hash functions for assigning
// a key to a partition and for indexing records.
[[maybe_unused]] unsigned partition_key(std::span<char> key)
{
    if (key.size() > 0)
        return static_cast<unsigned char>(key[0]);
    else
        return 0U;
}

[[maybe_unused]] int32_t hash_key(std::span<char> key)
{
    int32_t hash = 0U;
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

struct partition_args {
    std::string path;
    unsigned partitions;
    unsigned long records;
    size_t value_sz;
    unsigned cache_kib;
    unsigned threads;
};

class partition {
public:
    class exists: public std::runtime_error {
    public:
        exists(std::string_view path):
            std::runtime_error(std::string("Database file \"").append(path).append(" already exists")) {}
    };
    partition(const partition_args& args, unsigned idx, bool create);
    ~partition();
    std::string path_data() const {
        return path + "_data.sqlite";
    }
    std::string path_idx() const {
        return path + "_idx.sqlite";
    }
private:
    std::string path;
    std::optional<sqlite::connection> db;
};

partition::partition(const partition_args& args, unsigned idx, bool create)
{
    path = (std::ostringstream{} << args.path << std::setw(3) << std::setfill('0') << idx).str();
    if (create) {
        if (std::filesystem::exists(path_data()))
            throw exists(path_data());
        sqlite::connection data(path_data(), true);
        if (std::filesystem::exists(path_idx()))
            throw exists(path_idx());
        sqlite::connection idx(path_idx(), true);
    }
    db.emplace(path_data());
    sqlite::query(*db, R"(attach database ?1 as idx)").start().bind(0, path_idx()).next_row();
    if (create) {
        // speed up populating the database by turning off journaling
        sqlite::query(*db, R"(pragma journal_mode = off)").start().next_row();
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
        sqlite::query(*db, R"(create table main.data (key blob not null, counter int not null default 0, data blob))").
            start().next_row();
        sqlite::query(*db,
            R"(create table idx.idx (hash int not null, id int not null, primary key (hash, id)) without rowid)").
            start().next_row();
    }
    // Configure page caches for data and index database files. Cache size is
    // internally stored as the number of pages, therefore we must set it after
    // setting the page size. It is not remembered, therefore we must set it
    // every time the database is opened.
    double cache_sz = double(args.cache_kib) / args.partitions;
    double data_record = 8.0 /* id+overhead */ + 32.0 /* key */ + 8 /* counter */ +
        double(args.value_sz) /* data */;
    double idx_record = 8.0 /* overhead */ + 4.0 /* hash of key */ + 4 /* record id */;
    sqlite::query(*db, R"(pragma main.cache_size = )" +
                  std::to_string(int64_t(-cache_sz * data_record / (data_record + idx_record)))).start().next_row();
    sqlite::query(*db, R"(pragma idx.cache_size = )" +
                  std::to_string(int64_t(-cache_sz * idx_record / (data_record + idx_record)))).start().next_row();
}

partition::~partition()
{
    // enable WAL for regular database operation
    if (db)
        sqlite::query(*db, R"(pragma journal_mode = wal)").start().next_row();
}

using partitions = std::vector<std::unique_ptr<partition>>;

} // namespace create_impl

int create(std::string_view argv0, [[maybe_unused]] std::string_view cmd, std::span<const std::string_view> args)
{
    using namespace create_impl;
    // Process command line arguments
    if (args.size() < 5 || args.size() > 6)
        return usage(argv0);
    partition_args cargs{};
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
    partitions db{};
    db.reserve(cargs.partitions);
    for (decltype(cargs.partitions) part = 0; part < cargs.partitions; ++part) {
        log_msg(log_level::info) << "Creating partition " << part;
        try {
            db.push_back(std::make_unique<partition>(cargs, part, true));
        } catch (const partition::exists& e) {
            log_msg(log_level::crit) << e.what();
            return EXIT_FAILURE;
        }
    }
    // Generate data
    // TODO
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
