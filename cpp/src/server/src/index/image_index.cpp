#include "index/image_index.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <deque>
#include <limits>
#include <mutex>
#include <print>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <absl/strings/ascii.h>
#include <openssl/evp.h>
#include <sqlite3.h>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>
#include <sung/basic/os_detect.hpp>
#include <sung/basic/time.hpp>

#include "sung/image/img_info.hpp"

#include "gallery_snapshot.hpp"
#include "image_query.hpp"
#include "scan_journal.hpp"
#include "snapshot_store.hpp"
#include "snapshot_writer.hpp"
#include "tag_analysis.hpp"
#include "tag_queue.hpp"
#include "tagger_client.hpp"

#if defined(SUNG_OS_WINDOWS)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <sys/stat.h>
#endif

#if defined(SUNG_OS_LINUX)
    #include <fcntl.h>
    #include <linux/stat.h>
    #include <sys/syscall.h>
    #include <unistd.h>
#endif


namespace sung::detail {

    int64_t select_image_sort_time(
        const int64_t creation_time_ns, const int64_t modified_time_ns
    ) {
        if (creation_time_ns > 0)
            return creation_time_ns;
        return std::max<int64_t>(modified_time_ns, 0);
    }

    std::string logical_image_key(const Path& physical_path) {
        std::error_code ec;
        auto normalized = fs::absolute(physical_path, ec);
        if (ec)
            normalized = physical_path;
        normalized = normalized.lexically_normal();
        const auto canonical = fs::weakly_canonical(normalized, ec);
        if (!ec)
            normalized = canonical;
        if (const auto source = sprintboard_proxy_source_path(normalized))
            normalized = source->lexically_normal();

        auto output = tostr(normalized);
#if defined(SUNG_OS_WINDOWS)
        absl::AsciiStrToLower(&output);
#endif
        return output;
    }

}  // namespace sung::detail


namespace {

    constexpr int DATABASE_SCHEMA_VERSION = 9;
    constexpr int64_t NANOSECONDS_PER_SECOND = 1'000'000'000;

    std::optional<std::chrono::year_month_day> parse_date_folder(
        const std::string_view name
    ) {
        if (name.size() != 10 || name[4] != '-' || name[7] != '-')
            return std::nullopt;
        for (size_t i = 0; i < name.size(); ++i) {
            if (i != 4 && i != 7 && (name[i] < '0' || name[i] > '9'))
                return std::nullopt;
        }
        const auto number = [&](size_t offset, size_t count) {
            unsigned value = 0;
            for (size_t i = offset; i < offset + count; ++i)
                value = value * 10 + (name[i] - '0');
            return value;
        };
        const auto date = std::chrono::year_month_day{
            std::chrono::year{ static_cast<int>(number(0, 4)) },
            std::chrono::month{ number(5, 2) },
            std::chrono::day{ number(8, 2) }
        };
        if (!date.ok())
            return std::nullopt;
        return date;
    }

    enum class DateScope { container, selected, excluded };

    struct DateRange {
        std::string first_;
        std::string last_;

        bool empty() const { return first_.empty(); }
    };

    DateRange date_range(const std::string_view date, const int days) {
        const auto parsed = parse_date_folder(date);
        if (!parsed)
            throw std::invalid_argument("Image scan date must be YYYY-MM-DD");
        const auto first = std::chrono::sys_days{ *parsed } -
                           std::chrono::days{ days - 1 };
        return { std::format("{:%F}", first), std::string{ date } };
    }

    std::string local_date_folder() {
        const auto now = std::time(nullptr);
        std::tm local{};
#if defined(SUNG_OS_WINDOWS)
        if (localtime_s(&local, &now) != 0)
#else
        if (!localtime_r(&now, &local))
#endif
            throw std::runtime_error("Cannot determine local image scan date");
        return std::format(
            "{:04}-{:02}-{:02}",
            local.tm_year + 1900,
            local.tm_mon + 1,
            local.tm_mday
        );
    }

    // Purely lexical: preserving old entries must not stat archived images.
    DateScope date_scope(
        const sung::Path& directory,
        const sung::Path& root,
        const DateRange& date
    ) {
#if defined(SUNG_OS_WINDOWS)
        const auto relative =
            sung::fromstr(absl::AsciiStrToLower(sung::tostr(directory)))
                .lexically_relative(
                    sung::fromstr(absl::AsciiStrToLower(sung::tostr(root)))
                );
#else
        const auto relative = directory.lexically_relative(root);
#endif
        if (relative.empty() || *relative.begin() == "..")
            return DateScope::excluded;
        const auto parts = root.filename() / relative;
        for (const auto& part : parts) {
            const auto name = sung::tostr(part);
            if (parse_date_folder(name))
                return name >= date.first_ && name <= date.last_
                           ? DateScope::selected
                           : DateScope::excluded;
        }
        return DateScope::container;
    }


    int64_t make_timestamp_ns(
        const int64_t seconds, const int64_t nanoseconds
    ) {
        if (seconds <= 0)
            return 0;
        const auto clamped_nanoseconds = std::clamp<int64_t>(
            nanoseconds, 0, NANOSECONDS_PER_SECOND - 1
        );
        const auto max_seconds = std::numeric_limits<int64_t>::max() /
                                 NANOSECONDS_PER_SECOND;
        const auto remaining_nanoseconds = std::numeric_limits<int64_t>::max() %
                                           NANOSECONDS_PER_SECOND;
        if (seconds > max_seconds ||
            (seconds == max_seconds &&
             clamped_nanoseconds > remaining_nanoseconds)) {
            return std::numeric_limits<int64_t>::max();
        }
        return seconds * NANOSECONDS_PER_SECOND + clamped_nanoseconds;
    }

    int64_t get_image_sort_time(const sung::Path& path) {
#if defined(SUNG_OS_WINDOWS)
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (!GetFileAttributesExW(
                path.c_str(), GetFileExInfoStandard, &attributes
            )) {
            return 0;
        }

        const auto to_unix_ns = [](const FILETIME& value) {
            constexpr uint64_t WINDOWS_TO_UNIX_EPOCH = 116'444'736'000'000'000;
            const auto ticks = (static_cast<uint64_t>(value.dwHighDateTime)
                                << 32) |
                               value.dwLowDateTime;
            if (ticks <= WINDOWS_TO_UNIX_EPOCH)
                return int64_t{ 0 };
            const auto unix_ticks = ticks - WINDOWS_TO_UNIX_EPOCH;
            if (unix_ticks > static_cast<uint64_t>(
                                 std::numeric_limits<int64_t>::max() / 100
                             )) {
                return std::numeric_limits<int64_t>::max();
            }
            return static_cast<int64_t>(unix_ticks * 100);
        };

        return sung::detail::select_image_sort_time(
            to_unix_ns(attributes.ftCreationTime),
            to_unix_ns(attributes.ftLastWriteTime)
        );
#elif defined(SUNG_OS_MACOS)
        struct stat attributes{};
        if (::stat(path.c_str(), &attributes) != 0)
            return 0;
        return sung::detail::select_image_sort_time(
            make_timestamp_ns(
                attributes.st_birthtimespec.tv_sec,
                attributes.st_birthtimespec.tv_nsec
            ),
            make_timestamp_ns(
                attributes.st_mtimespec.tv_sec, attributes.st_mtimespec.tv_nsec
            )
        );
#elif defined(SUNG_OS_LINUX)
    #if defined(SYS_statx) && defined(STATX_BTIME)
        struct statx attributes{};
        if (::syscall(
                SYS_statx,
                AT_FDCWD,
                path.c_str(),
                AT_STATX_SYNC_AS_STAT,
                STATX_BTIME | STATX_MTIME,
                &attributes
            ) == 0) {
            const auto creation_time = (attributes.stx_mask & STATX_BTIME) != 0
                                           ? make_timestamp_ns(
                                                 attributes.stx_btime.tv_sec,
                                                 attributes.stx_btime.tv_nsec
                                             )
                                           : 0;
            const auto modified_time = (attributes.stx_mask & STATX_MTIME) != 0
                                           ? make_timestamp_ns(
                                                 attributes.stx_mtime.tv_sec,
                                                 attributes.stx_mtime.tv_nsec
                                             )
                                           : 0;
            return sung::detail::select_image_sort_time(
                creation_time, modified_time
            );
        }
    #endif

        struct stat fallback_attributes{};
        if (::stat(path.c_str(), &fallback_attributes) != 0)
            return 0;
        return make_timestamp_ns(
            fallback_attributes.st_mtim.tv_sec,
            fallback_attributes.st_mtim.tv_nsec
        );
#else
        struct stat attributes{};
        if (::stat(path.c_str(), &attributes) != 0)
            return 0;
        return make_timestamp_ns(attributes.st_mtime, 0);
#endif
    }


    struct CachedMetadata {
        int extraction_version_ = 0;
        std::string physical_path_;
        int64_t file_size_ = 0;
        int64_t modified_time_ = 0;
        std::string sha256_;
        int64_t sort_time_ns_ = 0;
        bool eligible_ = false;
        int width_ = 0;
        int height_ = 0;
        std::string model_;
        std::vector<std::string> prompts_;
    };

    using CachedTagAnalysis = sung::TagAnalysisRecord;

    using sung::gallery_cache::IndexedFile;
    using sung::gallery_cache::IndexedFolder;
    using sung::gallery_cache::IndexSnapshot;

    bool file_before(const IndexedFile& a, const IndexedFile& b) {
        return sung::ImageListResponse::file_before(a.info_, b.info_);
    }

    std::string make_root_key(
        const std::string& namespace_name, const sung::Path& root
    ) {
        return namespace_name + '\n' + sung::tostr(root);
    }

    bool is_descendant_or_child(
        const std::string& parent, const std::string& candidate_parent
    ) {
        if (candidate_parent == parent)
            return true;
        if (parent.empty())
            return false;
        return candidate_parent.starts_with(parent + "/");
    }

    std::optional<sung::FileFingerprint> validate_fingerprint(
        const sung::Path& path,
        const int64_t expected_size,
        const int64_t expected_modified_time,
        const std::string& expected_sha256
    ) {
        const auto metadata = sung::fingerprint_file(path);
        if (!metadata || metadata->size_ != expected_size)
            return std::nullopt;
        if (metadata->modified_time_ == expected_modified_time) {
            auto output = *metadata;
            output.sha256_ = expected_sha256;
            return output;
        }
        const auto fingerprint = sung::fingerprint_file_with_sha256(path);
        if (!fingerprint || fingerprint->sha256_ != expected_sha256)
            return std::nullopt;
        return *fingerprint;
    }

    std::string make_path_key(const sung::Path& path) {
        auto output = sung::tostr(path);
#if defined(SUNG_OS_WINDOWS)
        absl::AsciiStrToLower(&output);
#endif
        return output;
    }

    CachedMetadata inspect_file(
        const sung::Path& path,
        const int64_t size,
        const int64_t modified,
        const int64_t sort_time_ns
    ) {
        CachedMetadata output;
        output.extraction_version_ = sung::GENERATION_EXTRACTION_VERSION;
        output.physical_path_ = sung::tostr(path);
        output.file_size_ = size;
        output.modified_time_ = modified;
        output.sort_time_ns_ = sort_time_ns;

        sung::ImageInfo info{ path };
        if (!info.load_simple_info())
            return output;

        output.eligible_ = true;
        output.width_ = static_cast<int>(info.width());
        output.height_ = static_cast<int>(info.height());

        if (info.load_img_metadata() && info.extract_generation_info()) {
            output.model_ = info.sd().model_name_;
            output.prompts_ = info.sd().prompt_;
        }

        return output;
    }

    // Concurrency this workload wants to run at while scanning: each unit of
    // work is dominated by round-trip latency to the filesystem (which may
    // sit behind something slow, e.g. an encrypted vault driver) rather than
    // CPU, so we deliberately run far more of these in flight at once than
    // there are cores, to overlap that latency instead of serializing it.
    constexpr int SCAN_CONCURRENCY = 32;

    struct FileProbe {
        bool shadowed_ = false;
        bool stat_failed_ = false;
        bool reused_ = false;
        bool needs_persist_ = false;
        CachedMetadata metadata_;
    };

    // Pure/side-effect-free so it is safe to call concurrently across files:
    // reads `existing` (a snapshot, not a live map reference) and touches
    // only the filesystem and its own return value.
    FileProbe probe_file(
        const sung::Path& physical_path,
        const bool shadowed_by_proxy,
        const sung::Path* sort_time_source,
        const CachedMetadata* existing
    ) {
        FileProbe probe;

        if (shadowed_by_proxy) {
            probe.shadowed_ = true;
            return probe;
        }

        const auto fingerprint = sung::fingerprint_file(physical_path);
        if (!fingerprint) {
            probe.stat_failed_ = true;
            return probe;
        }
        const auto size = fingerprint->size_;
        const auto modified = fingerprint->modified_time_;

        // Sync clients can change timestamp precision without changing bytes.
        // Only hash when the portable size/time fast path cannot establish
        // reuse.
        std::optional<sung::FileFingerprint> hashed;
        const bool current_extraction =
            existing && existing->extraction_version_ ==
                            sung::GENERATION_EXTRACTION_VERSION;
        bool reusable = current_extraction && existing->file_size_ == size &&
                        existing->modified_time_ == modified;
        if (!reusable && current_extraction && existing->file_size_ == size &&
            !existing->sha256_.empty()) {
            const auto result = sung::fingerprint_file_with_sha256(
                physical_path
            );
            if (result && result->size_ == size &&
                result->modified_time_ == modified) {
                hashed = *result;
                reusable = hashed->sha256_ == existing->sha256_;
            }
        }

        if (reusable) {
            probe.reused_ = true;
            probe.metadata_ = *existing;
            if (probe.metadata_.modified_time_ != modified) {
                probe.metadata_.modified_time_ = modified;
                probe.needs_persist_ = true;
            }
            if (sort_time_source) {
                const auto& source_path = *sort_time_source;
                const auto sort_time_ns = get_image_sort_time(source_path);
                if (sort_time_ns > 0 &&
                    probe.metadata_.sort_time_ns_ != sort_time_ns) {
                    probe.metadata_.sort_time_ns_ = sort_time_ns;
                    probe.needs_persist_ = true;
                } else if (
                    sort_time_ns == 0 && probe.metadata_.sort_time_ns_ == 0
                ) {
                    const auto fallback_sort_time_ns = get_image_sort_time(
                        physical_path
                    );
                    if (fallback_sort_time_ns > 0) {
                        probe.metadata_.sort_time_ns_ = fallback_sort_time_ns;
                        probe.needs_persist_ = true;
                    }
                }
            } else if (probe.metadata_.sort_time_ns_ == 0) {
                const auto fallback_sort_time_ns = get_image_sort_time(
                    physical_path
                );
                if (fallback_sort_time_ns > 0) {
                    probe.metadata_.sort_time_ns_ = fallback_sort_time_ns;
                    probe.needs_persist_ = true;
                }
            }
        } else {
            auto sort_time_ns = get_image_sort_time(
                sort_time_source ? *sort_time_source : physical_path
            );
            if (sort_time_ns == 0 && sort_time_source)
                sort_time_ns = get_image_sort_time(physical_path);
            probe.metadata_ = inspect_file(
                physical_path, size, modified, sort_time_ns
            );
            if (!hashed) {
                const auto result = sung::fingerprint_file_with_sha256(
                    physical_path
                );
                if (result)
                    hashed = *result;
            }
            // Never associate decoded metadata with a fingerprint from a file
            // that changed during inspection; a later refresh can retry it.
            const auto after = sung::fingerprint_file(physical_path);
            if (!after || *after != *fingerprint ||
                (hashed && (hashed->size_ != size ||
                            hashed->modified_time_ != modified))) {
                probe.stat_failed_ = true;
                return probe;
            }
            if (hashed)
                probe.metadata_.sha256_ = hashed->sha256_;
            probe.needs_persist_ = true;
        }

        return probe;
    }

    bool execute_sql(sqlite3* database, const char* sql) {
        char* error = nullptr;
        const auto result = sqlite3_exec(
            database, sql, nullptr, nullptr, &error
        );
        if (result == SQLITE_OK)
            return true;

        std::println(
            "ImageIndex: SQLite error: {}", error ? error : "unknown error"
        );
        sqlite3_free(error);
        return false;
    }

}  // namespace

namespace sung {

    class IndexStorage;

}

// SQLite connections and all storage state are confined to the storage worker.
class sung::IndexStorage {
public:
    IndexStorage(
        std::atomic_bool& loading,
        const std::function<void(std::string_view)>& observer
    )
        : cache_loading_(loading), scan_observer_(observer) {
    }
    struct Store {
        Path root_;
        Path path_;
        Path fallback_;
        sqlite3* database_ = nullptr;
        bool dirty_ = false;
        bool active_ = true;
        bool blocked_ = false;
        bool paused_ = false;
        std::unordered_map<std::string, CachedTagAnalysis> pending_tags_;
        std::unordered_map<std::string, std::string> persisted_tags_;
        std::unordered_map<std::string, std::string> deleted_tags_;
        bool healthy_ = false;
        bool loaded_ = false;
        int last_error_ = SQLITE_OK;
        ~Store() {
            if (database_)
                sqlite3_close(database_);
        }
    };

    using ResolvedPaths = std::unordered_map<std::string, Path>;

    static Path canonical_path(
        const Path& path, ResolvedPaths* resolved = nullptr
    ) {
        const auto key = make_path_key(path);
        if (resolved) {
            const auto found = resolved->find(key);
            if (found != resolved->end())
                return found->second;
        }
        std::error_code ec;
        auto result = fs::weakly_canonical(fs::absolute(path, ec), ec);
        if (ec)
            result = path.lexically_normal();
        while (result.has_relative_path() && result.filename().empty())
            result = result.parent_path();
        if (resolved) {
            resolved->insert_or_assign(key, result);
            resolved->try_emplace(make_path_key(result), result);
        }
        return result;
    }

    static std::optional<std::string> relative_path(
        const Store& store,
        const std::string& path,
        ResolvedPaths* resolved = nullptr
    ) {
        if (path.empty())
            return std::string{};
        const auto absolute = canonical_path(sung::fromstr(path), resolved);
        auto part = absolute.begin();
        for (const auto& root_part : store.root_) {
            if (part == absolute.end() ||
                make_path_key(*part) != make_path_key(root_part))
                return std::nullopt;
            ++part;
        }
        Path relative;
        for (; part != absolute.end(); ++part) {
            if (*part == "..")
                return std::nullopt;
            relative /= *part;
        }
        return sung::tostr(relative);
    }

    static bool restore_path(
        const Store& store,
        std::string& value,
        ResolvedPaths* resolved = nullptr
    ) {
        if (value.empty())
            return true;
        const auto relative = sung::fromstr(value);
        // Reject foreign-platform absolute paths as well as native ones.
        if (relative.has_root_path() || value.find('\\') != std::string::npos ||
            value.find(':') != std::string::npos)
            return false;
        for (const auto& part : relative)
            if (part == "..")
                return false;
        const auto absolute = canonical_path(store.root_ / relative, resolved);
        if (!relative_path(store, sung::tostr(absolute), resolved))
            return false;
        value = sung::tostr(absolute);
        return true;
    }

    Store* owner(
        const std::string& path, ResolvedPaths* resolved = nullptr
    ) const {
        Store* result = nullptr;
        size_t depth = 0;
        for (const auto& [key, store] : stores_) {
            if (store->active_ && key.size() > depth &&
                relative_path(*store, path, resolved)) {
                result = store.get();
                depth = key.size();
            }
        }
        return result;
    }

    static std::string root_hash(const std::string& value) {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int size = 0;
        if (EVP_Digest(
                value.data(), value.size(), digest, &size, EVP_sha256(), nullptr
            ) != 1)
            throw std::runtime_error("Cannot hash cache root");
        std::string result;
        for (unsigned int i = 0; i < size; ++i)
            result += std::format("{:02x}", digest[i]);
        return result;
    }

    static std::string stored_logical_path(
        const Store& store, const CachedTagAnalysis& item
    ) {
        const auto& input = item.input_path_.empty() ? item.attempt_input_path_
                                                     : item.input_path_;
        const auto physical =
            sung::fromstr(input.empty() ? item.logical_path_ : input);
        return relative_path(
                   store,
                   sung::tostr(
                       sung::sprintboard_proxy_source_path(physical).value_or(
                           physical
                       )
                   )
        )
            .value_or("");
    }

    static std::optional<int> database_version(sqlite3* db) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(
                db, "PRAGMA user_version;", -1, &stmt, nullptr
            ) != SQLITE_OK)
            return std::nullopt;
        const auto result = sqlite3_step(stmt);
        const auto version =
            result == SQLITE_ROW
                ? std::optional<int>{sqlite3_column_int(stmt, 0)}
                : std::nullopt;
        sqlite3_finalize(stmt);
        return version;
    }

    bool inspect_database(Store& store, const Path& path) {
        std::error_code ec;
        if (!fs::is_regular_file(path, ec))
            return true;
        sqlite3* db = nullptr;
        if (sqlite3_open_v2(
                sung::tostr(path).c_str(), &db, SQLITE_OPEN_READONLY, nullptr
            ) != SQLITE_OK) {
            if (db)
                sqlite3_close(db);
            return true;  // Permission/unavailability can use the fallback.
        }
        const auto version = database_version(db);
        sqlite3_stmt* check = nullptr;
        bool valid = version.has_value() &&
                     sqlite3_prepare_v2(
                         db, "PRAGMA quick_check;", -1, &check, nullptr
                     ) == SQLITE_OK;
        if (valid)
            valid =
                sqlite3_step(check) == SQLITE_ROW &&
                std::string(
                    reinterpret_cast<const char*>(sqlite3_column_text(check, 0))
                ) == "ok";
        sqlite3_finalize(check);
        if (valid && *version == 0) {
            valid = sqlite3_prepare_v2(
                        db,
                        "SELECT COUNT(*) FROM sqlite_master WHERE name NOT "
                        "LIKE 'sqlite_%';",
                        -1,
                        &check,
                        nullptr
                    ) == SQLITE_OK;
            if (valid)
                valid = sqlite3_step(check) == SQLITE_ROW &&
                        sqlite3_column_int(check, 0) == 0;
            sqlite3_finalize(check);
        } else if (valid)
            valid = *version == 6 || *version == 7 || *version == 8 ||
                    *version == DATABASE_SCHEMA_VERSION;
        sqlite3_close(db);
        if (!valid) {
            store.blocked_ = true;
            std::println(
                "ImageIndex: Preserving incompatible or corrupt database "
                "{}; "
                "tagging is disabled for {} until it is repaired or opened "
                "by "
                "a compatible application.",
                sung::tostr(path),
                sung::tostr(store.root_)
            );
        }
        return valid && !store.blocked_;
    }

    void load_readonly_tags(
        Store& store, const Path& path, ResolvedPaths* resolved
    ) {
        sqlite3* database = nullptr;
        if (sqlite3_open_v2(
                tostr(path).c_str(), &database, SQLITE_OPEN_READONLY, nullptr
            ) == SQLITE_OK)
            load_tag_analyses(
                store, database, path != store.fallback_, false, resolved
            );
        if (database)
            sqlite3_close(database);
    }

    void reconcile_stores(const ServerConfigs& configs) {
        // Resolutions are shared only within this preparation pass. Never
        // retain them across scans: a symlink or mount may have changed.
        ResolvedPaths resolved;
        std::set<std::string> roots;
        std::map<std::string, Path> root_paths;
        binding_roots_.clear();
        for (const auto& [name, binding] : configs.dir_bindings_) {
            const auto root = canonical_path(binding.local_dir_);
            const auto key = make_path_key(root);
            roots.insert(key);
            binding_roots_.emplace(name, key);
            root_paths.try_emplace(key, root);
        }
        for (auto& [key, store] : stores_) store->active_ = roots.contains(key);
        for (const auto& key : roots) {
            if (stores_.contains(key))
                continue;
            auto store = std::make_unique<Store>();
            store->root_ = root_paths.at(key);
            store->path_ =
                store->root_ / ".sprintboard" / "image-index.sqlite3";
            store->fallback_ =
                canonical_path(sung::fromstr(configs.cache_dir_)) /
                root_hash(key) / "image-index.sqlite3";
            stores_.emplace(key, std::move(store));
        }
        for (auto& [key, store] : stores_) {
            if (!store->active_ || store->blocked_ || store->database_)
                continue;

            struct LoadingCache {
                std::atomic_bool& flag_;

                explicit LoadingCache(std::atomic_bool& flag) : flag_(flag) {
                    flag_ = true;
                }

                ~LoadingCache() {
                    flag_ = false;
                }
            } loading{cache_loading_};
            std::error_code ec;
            if (!fs::is_directory(store->root_, ec) || ec)
                continue;
            const bool initial_load = !store->loaded_;
            const auto primary =
                store->root_ / ".sprintboard" / "image-index.sqlite3";
            sung::MonotonicRealtimeTimer load_timer;
            if (initial_load) {
                std::println(
                    "ImageIndex: Checking cached database for {}...",
                    sung::tostr(store->root_)
                );
                if (!inspect_database(*store, primary))
                    continue;
                if (!inspect_database(*store, store->fallback_)) {
                    // A broken fallback must not hide readable primary
                    // tags.
                    load_readonly_tags(*store, primary, &resolved);
                    continue;
                }
            }
            open_database(*store, initial_load);
            if (!store->database_ && !store->blocked_ &&
                store->path_ != store->fallback_) {
                store->path_ = store->fallback_;
                open_database(*store, initial_load);
            }
            store->healthy_ = store->database_ != nullptr;
            std::println(
                "ImageIndex: Root {} database: {}",
                key,
                store->database_ ? sung::tostr(store->path_)
                                 : "unavailable (browsing only)"
            );
            std::println(
                "ImageIndex: Loading cached metadata and tags for {}...",
                tostr(store->root_)
            );
            if (initial_load && store->database_)
                load_metadata(*store, &resolved);
            // Merge primary then fallback exactly once. Readable tags
            // remain available even when neither database can be opened for
            // writing.
            for (const auto& path : {primary, store->fallback_}) {
                if (path == store->path_ && store->database_) {
                    load_tag_analyses(
                        *store,
                        store->database_,
                        path != store->fallback_,
                        true,
                        &resolved
                    );
                } else if (initial_load) {
                    load_readonly_tags(*store, path, &resolved);
                }
            }
            std::println(
                "ImageIndex: Loaded cached metadata and tags for {} in "
                "{:.3f} "
                "seconds.",
                tostr(store->root_),
                load_timer.elapsed()
            );
            if (store->database_) {
                store->loaded_ = true;
                store->dirty_ = true;
                // Reconcile readable local/fallback records into the
                // selected store.
                for (const auto& [path, item] : tag_analyses_)
                    if (owner(path, &resolved) == store.get())
                        store->pending_tags_.try_emplace(path, item);
            }
        }
        if (root_keys_ != roots) {
            for (auto& [key, store] : stores_) {
                if (!store->active_)
                    continue;
                store->dirty_ = true;
                for (const auto& [path, item] : tag_analyses_)
                    if (owner(path, &resolved) == store.get())
                        store->pending_tags_.try_emplace(path, item);
            }
            // Move uncommitted results too; never publish them before
            // destination commit.
            for (auto& [key, store] : stores_) {
                for (auto it = store->pending_tags_.begin();
                     it != store->pending_tags_.end();) {
                    auto* destination = owner(it->first, &resolved);
                    if (destination && destination != store.get()) {
                        destination->pending_tags_.insert_or_assign(
                            it->first, it->second
                        );
                        it = store->pending_tags_.erase(it);
                    } else
                        ++it;
                }
            }
            std::erase_if(tag_analyses_, [&](const auto& entry) {
                return owner(entry.first, &resolved) == nullptr;
            });
            root_keys_ = std::move(roots);
        }
    }

    static void record_failure(Store& store) {
        store.healthy_ = false;
        if (!store.database_)
            return;
        const auto error = store.last_error_ & 0xff;
        if (error == SQLITE_IOERR || error == SQLITE_CANTOPEN ||
            error == SQLITE_NOTADB || error == SQLITE_READONLY) {
            sqlite3_close(store.database_);
            store.database_ = nullptr;
            store.dirty_ = true;
        }
    }

    void open_database(Store& store, bool validated = false) {
        if (!validated && !inspect_database(store, store.path_))
            return;
        auto*& database_ = store.database_;
        const auto& database_path_ = store.path_;
        std::error_code ec;
        const auto parent = database_path_.parent_path();
        if (!parent.empty())
            fs::create_directories(parent, ec);
        if (ec) {
            std::println(
                "ImageIndex: Cannot create cache directory: {}. Using "
                "memory "
                "only.",
                ec.message()
            );
            return;
        }

        const auto path_str = sung::tostr(database_path_);
        if (sqlite3_open_v2(
                path_str.c_str(),
                &database_,
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                nullptr
            ) != SQLITE_OK) {
            std::println(
                "ImageIndex: Cannot open '{}': {}. Using memory only.",
                path_str,
                database_ ? sqlite3_errmsg(database_) : "unknown error"
            );
            if (database_)
                sqlite3_close(database_);
            database_ = nullptr;
            return;
        }

        if (!execute_sql(database_, "PRAGMA journal_mode=WAL;") ||
            !execute_sql(database_, "PRAGMA synchronous=FULL;")) {
            sqlite3_close(database_);
            database_ = nullptr;
            return;
        }

        int schema_version = 0;
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(
                database_, "PRAGMA user_version;", -1, &statement, nullptr
            ) == SQLITE_OK &&
            sqlite3_step(statement) == SQLITE_ROW) {
            schema_version = sqlite3_column_int(statement, 0);
        }
        sqlite3_finalize(statement);

        const char* create_tag_table =
            "CREATE TABLE IF NOT EXISTS image_tag_analysis ("
            "logical_path TEXT PRIMARY KEY,"
            "input_kind TEXT NOT NULL DEFAULT '',"
            "input_path TEXT NOT NULL DEFAULT '',"
            "input_size INTEGER NOT NULL DEFAULT 0,"
            "input_modified_time INTEGER NOT NULL DEFAULT 0,"
            "input_sha256 TEXT NOT NULL DEFAULT '',"
            "analyzer_fingerprint TEXT NOT NULL DEFAULT '',"
            "model_id TEXT NOT NULL DEFAULT '',"
            "general_threshold REAL NOT NULL DEFAULT 0,"
            "character_threshold REAL NOT NULL DEFAULT 0,"
            "analysis_json TEXT NOT NULL DEFAULT '',"
            "analyzed_at INTEGER NOT NULL DEFAULT 0,"
            "attempt_input_path TEXT NOT NULL DEFAULT '',"
            "attempt_input_size INTEGER NOT NULL DEFAULT 0,"
            "attempt_input_modified_time INTEGER NOT NULL DEFAULT 0,"
            "attempt_analyzer_fingerprint TEXT NOT NULL DEFAULT '',"
            "last_attempt_at INTEGER NOT NULL DEFAULT 0,"
            "failure_count INTEGER NOT NULL DEFAULT 0,"
            "last_error TEXT NOT NULL DEFAULT '',"
            "analysis_id TEXT NOT NULL DEFAULT '',"
            "proxy_path TEXT NOT NULL DEFAULT '',"
            "proxy_size INTEGER NOT NULL DEFAULT 0,"
            "proxy_modified_time INTEGER NOT NULL DEFAULT 0,"
            "proxy_sha256 TEXT NOT NULL DEFAULT '',"
            "proxy_materialization_id TEXT NOT NULL DEFAULT ''"
            ");";

        bool success = execute_sql(database_, "BEGIN IMMEDIATE;");
        if (success && schema_version == 6) {
            success =
                execute_sql(
                    database_,
                    "ALTER TABLE image_tag_analysis RENAME TO "
                    "image_tag_analysis_v6;"
                ) &&
                execute_sql(database_, create_tag_table) &&
                execute_sql(
                    database_,
                    "INSERT INTO image_tag_analysis "
                    "(logical_path,input_kind,input_path,input_size,input_"
                    "modified_time,input_sha256,analyzer_fingerprint,model_"
                    "id,"
                    "general_threshold,character_threshold,analysis_json,"
                    "analyzed_at,attempt_input_path,attempt_input_size,"
                    "attempt_"
                    "input_modified_time,attempt_analyzer_fingerprint,last_"
                    "attempt_at,failure_count,last_error,analysis_id,proxy_"
                    "path,proxy_size,proxy_modified_time,proxy_sha256,"
                    "proxy_"
                    "materialization_id) SELECT "
                    "logical_path,input_kind,input_path,input_size,input_"
                    "modified_time,input_sha256,analyzer_fingerprint,model_"
                    "id,"
                    "general_threshold,character_threshold,analysis_json,"
                    "analyzed_at,attempt_input_path,attempt_input_size,"
                    "attempt_"
                    "input_modified_time,attempt_analyzer_fingerprint,last_"
                    "attempt_at,failure_count,last_error,analysis_id,proxy_"
                    "path,proxy_size,proxy_modified_time,proxy_sha256,"
                    "proxy_"
                    "materialization_id FROM image_tag_analysis_v6;"
                ) &&
                execute_sql(database_, "DROP TABLE image_tag_analysis_v6;");
        }
        if (success && (schema_version == 6 || schema_version == 7)) {
            // Legacy metadata used platform-specific file_clock ticks.
            // There is no recorded source platform, so rebuild only this
            // disposable table.
            success = execute_sql(
                database_, "DROP TABLE IF EXISTS image_metadata;"
            );
            if (success)
                std::println(
                    "ImageIndex: Upgrading to portable metadata "
                    "fingerprints; "
                    "rebuilding image metadata once (saved tags are "
                    "preserved)."
                );
        }
        if (success)
            success = execute_sql(
                          database_,
                          "CREATE TABLE IF NOT EXISTS image_metadata "
                          "(physical_path "
                          "TEXT PRIMARY KEY,file_size INTEGER NOT "
                          "NULL,modified_time "
                          "INTEGER NOT NULL,sort_time_ns INTEGER NOT "
                          "NULL,eligible "
                          "INTEGER NOT NULL,width INTEGER NOT NULL,height "
                          "INTEGER "
                          "NOT NULL,model TEXT NOT NULL,prompts_json TEXT "
                          "NOT NULL,"
                          "sha256 TEXT NOT NULL DEFAULT '', extraction_version "
                          "INTEGER NOT NULL DEFAULT 0);"
                      ) &&
                      execute_sql(database_, create_tag_table);
        if (success) {
            sqlite3_stmt* columns = nullptr;
            bool has_version = false;
            success = sqlite3_prepare_v2(
                          database_,
                          "PRAGMA table_info(image_metadata);",
                          -1,
                          &columns,
                          nullptr
                      ) == SQLITE_OK;
            while (success && sqlite3_step(columns) == SQLITE_ROW) {
                const auto name = reinterpret_cast<const char*>(
                    sqlite3_column_text(columns, 1)
                );
                if (name && std::string_view{name} == "extraction_version")
                    has_version = true;
            }
            sqlite3_finalize(columns);
            if (success && !has_version)
                success = execute_sql(
                    database_,
                    "ALTER TABLE image_metadata ADD COLUMN extraction_version "
                    "INTEGER NOT NULL DEFAULT 0;"
                );
        }
        if (success)
            success = execute_sql(database_, "PRAGMA user_version=9;") &&
                      execute_sql(database_, "COMMIT;");
        if (!success) {
            execute_sql(database_, "ROLLBACK;");
            sqlite3_close(database_);
            database_ = nullptr;
        }
    }

    void load_metadata(Store& store, ResolvedPaths* resolved = nullptr) {
        auto* database_ = store.database_;
        if (!database_)
            return;

        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(
                database_,
                "SELECT physical_path, file_size, modified_time, "
                "sort_time_ns, "
                "eligible, width, height, model, prompts_json, sha256, "
                "extraction_version FROM "
                "image_metadata;",
                -1,
                &statement,
                nullptr
            ) != SQLITE_OK) {
            std::println(
                "ImageIndex: Cannot load cache: {}", sqlite3_errmsg(database_)
            );
            return;
        }

        while (sqlite3_step(statement) == SQLITE_ROW) {
            CachedMetadata metadata;
            metadata.extraction_version_ = sqlite3_column_int(statement, 10);
            metadata.physical_path_ = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 0)
            );
            metadata.file_size_ = sqlite3_column_int64(statement, 1);
            metadata.modified_time_ = sqlite3_column_int64(statement, 2);
            metadata.sort_time_ns_ = sqlite3_column_int64(statement, 3);
            metadata.eligible_ = sqlite3_column_int(statement, 4) != 0;
            metadata.width_ = sqlite3_column_int(statement, 5);
            metadata.height_ = sqlite3_column_int(statement, 6);
            metadata.model_ = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 7)
            );
            metadata.sha256_ = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 9)
            );

            try {
                const auto* prompts_text = reinterpret_cast<const char*>(
                    sqlite3_column_text(statement, 8)
                );
                metadata.prompts_ = nlohmann::json::parse(prompts_text)
                                        .get<std::vector<std::string>>();
            } catch (const std::exception&) {
                metadata.prompts_.clear();
            }
            if (metadata.extraction_version_ !=
                sung::GENERATION_EXTRACTION_VERSION) {
                metadata.model_.clear();
                metadata.prompts_.clear();
            }
            if (!metadata.physical_path_.empty() &&
                restore_path(store, metadata.physical_path_, resolved) &&
                owner(metadata.physical_path_, resolved) == &store)
                metadata_.try_emplace(
                    metadata.physical_path_, std::move(metadata)
                );
        }
        sqlite3_finalize(statement);
    }

    void merge_analysis(CachedTagAnalysis incoming, bool local) {
        const auto key = incoming.logical_path_;
        const auto existing = tag_analyses_.find(key);
        if (existing != tag_analyses_.end()) {
            const auto& previous = existing->second;
            const bool incoming_wins =
                !incoming.analysis_.is_null() &&
                (previous.analysis_.is_null() ||
                 incoming.analyzed_at_ > previous.analyzed_at_ ||
                 (incoming.analyzed_at_ == previous.analyzed_at_ && local &&
                  !analysis_local_[key]));
            auto chosen = incoming_wins ? incoming : previous;
            const auto& retry =
                incoming.last_attempt_at_ > previous.last_attempt_at_
                    ? incoming
                    : previous;
            chosen.attempt_input_path_ = retry.attempt_input_path_;
            chosen.attempt_input_size_ = retry.attempt_input_size_;
            chosen.attempt_input_modified_time_ =
                retry.attempt_input_modified_time_;
            chosen.attempt_analyzer_fingerprint_ =
                retry.attempt_analyzer_fingerprint_;
            chosen.last_attempt_at_ = retry.last_attempt_at_;
            chosen.failure_count_ = retry.failure_count_;
            chosen.last_error_ = retry.last_error_;
            const auto& other = incoming_wins ? previous : incoming;
            if (!chosen.analysis_id_.empty() &&
                chosen.analysis_id_ == other.analysis_id_ &&
                !other.proxy_path_.empty() &&
                (chosen.proxy_path_.empty() ||
                 !validate_fingerprint(
                     sung::fromstr(chosen.proxy_path_),
                     chosen.proxy_size_,
                     chosen.proxy_modified_time_,
                     chosen.proxy_sha256_
                 )) &&
                validate_fingerprint(
                    sung::fromstr(other.proxy_path_),
                    other.proxy_size_,
                    other.proxy_modified_time_,
                    other.proxy_sha256_
                )) {
                chosen.proxy_path_ = other.proxy_path_;
                chosen.proxy_size_ = other.proxy_size_;
                chosen.proxy_modified_time_ = other.proxy_modified_time_;
                chosen.proxy_sha256_ = other.proxy_sha256_;
                chosen.proxy_materialization_id_ =
                    other.proxy_materialization_id_;
            }
            if (incoming_wins)
                analysis_local_[key] = local;
            incoming = std::move(chosen);
        } else
            analysis_local_[key] = local;
        if (!incoming.proxy_path_.empty() &&
            !validate_fingerprint(
                sung::fromstr(incoming.proxy_path_),
                incoming.proxy_size_,
                incoming.proxy_modified_time_,
                incoming.proxy_sha256_
            )) {
            incoming.proxy_path_.clear();
            incoming.proxy_size_ = incoming.proxy_modified_time_ = 0;
            incoming.proxy_sha256_.clear();
            incoming.proxy_materialization_id_.clear();
        }
        tag_analyses_.insert_or_assign(key, std::move(incoming));
    }

    void load_tag_analyses(
        Store& store,
        sqlite3* database_,
        bool local,
        bool track,
        ResolvedPaths* resolved = nullptr
    ) {
        if (!database_)
            return;
        if (scan_observer_)
            scan_observer_("cache-tags");

        sqlite3_stmt* statement = nullptr;
        const char* query =
            "SELECT logical_path, input_kind, input_path, input_size, "
            "input_modified_time, input_sha256, analyzer_fingerprint, "
            "model_id, "
            "general_threshold, character_threshold, analysis_json, "
            "analyzed_at, attempt_input_path, attempt_input_size, "
            "attempt_input_modified_time, attempt_analyzer_fingerprint, "
            "last_attempt_at, failure_count, last_error, analysis_id, "
            "proxy_path, proxy_size, proxy_modified_time, "
            "proxy_sha256, proxy_materialization_id "
            "FROM image_tag_analysis;";
        if (sqlite3_prepare_v2(database_, query, -1, &statement, nullptr) !=
            SQLITE_OK) {
            std::println(
                "ImageIndex: Cannot load tag cache: {}",
                sqlite3_errmsg(database_)
            );
            store.blocked_ = true;
            return;
        }

        while (sqlite3_step(statement) == SQLITE_ROW) {
            CachedTagAnalysis analysis;
            analysis.logical_path_ = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 0)
            );
            analysis.input_kind_ = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 1)
            );
            analysis.input_path_ = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 2)
            );
            analysis.input_size_ = sqlite3_column_int64(statement, 3);
            analysis.input_modified_time_ = sqlite3_column_int64(statement, 4);
            analysis.input_sha256_ = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 5)
            );
            analysis.analyzer_fingerprint_ = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 6)
            );
            analysis.model_id_ = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 7)
            );
            analysis.general_threshold_ = sqlite3_column_double(statement, 8);
            analysis.character_threshold_ = sqlite3_column_double(statement, 9);
            const auto* json_text = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 10)
            );
            if (json_text && *json_text) {
                try {
                    analysis.analysis_ = nlohmann::json::parse(json_text);
                    analysis.searchable_tags_ =
                        sung::searchable_tags_from_analysis(analysis.analysis_);
                } catch (const std::exception&) {
                    std::println(
                        "ImageIndex: Preserving an unreadable tag record "
                        "in "
                        "{}.",
                        sung::tostr(store.path_)
                    );
                    continue;
                }
            }
            analysis.analyzed_at_ = sqlite3_column_int64(statement, 11);
            analysis.attempt_input_path_ = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 12)
            );
            analysis.attempt_input_size_ = sqlite3_column_int64(statement, 13);
            analysis.attempt_input_modified_time_ =
                sqlite3_column_int64(statement, 14);
            analysis.attempt_analyzer_fingerprint_ =
                reinterpret_cast<const char*>(
                    sqlite3_column_text(statement, 15)
                );
            analysis.last_attempt_at_ = sqlite3_column_int64(statement, 16);
            analysis.failure_count_ = sqlite3_column_int(statement, 17);
            analysis.last_error_ = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 18)
            );
            analysis.analysis_id_ = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 19)
            );
            analysis.proxy_path_ = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 20)
            );
            analysis.proxy_size_ = sqlite3_column_int64(statement, 21);
            analysis.proxy_modified_time_ = sqlite3_column_int64(statement, 22);
            analysis.proxy_sha256_ = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 23)
            );
            analysis.proxy_materialization_id_ = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 24)
            );
            const auto stored_key = analysis.logical_path_;
            if (analysis.logical_path_.empty())
                continue;
            bool valid = true;
            for (auto* path :
                 {&analysis.logical_path_,
                  &analysis.input_path_,
                  &analysis.attempt_input_path_,
                  &analysis.proxy_path_})
                valid = restore_path(store, *path, resolved) && valid;
            if (valid) {
                // restore_path already canonicalized and checked
                // containment.
                const auto physical = fromstr(analysis.logical_path_);
                analysis.logical_path_ = make_path_key(
                    sprintboard_proxy_source_path(physical).value_or(physical)
                );
            }
            if (valid && !sung::is_sprintboard_temporary_path(
                             sung::fromstr(analysis.logical_path_)
                         )) {
                if (analysis.analysis_.is_object() &&
                    analysis.analysis_.contains("path"))
                    analysis.analysis_["path"] = analysis.input_path_;
                if (track)
                    store.persisted_tags_[analysis.logical_path_] = stored_key;
                merge_analysis(analysis, local);
            } else {
                std::println(
                    "ImageIndex: Ignoring invalid relative tag paths in "
                    "{}; "
                    "preserving the stored row.",
                    sung::tostr(store.path_)
                );
            }
        }
        sqlite3_finalize(statement);
    }

    bool persist_changes(
        Store& store,
        const std::vector<CachedMetadata>& changed,
        const std::vector<std::string>& removed,
        const bool replace_all
    ) {
        auto* database_ = store.database_;
        if (!database_)
            return false;
        if (changed.empty() && removed.empty() && !replace_all)
            return true;
        if (!execute_sql(database_, "BEGIN IMMEDIATE;")) {
            store.last_error_ = sqlite3_errcode(database_);
            return false;
        }

        if (replace_all &&
            !execute_sql(database_, "DELETE FROM image_metadata;")) {
            store.last_error_ = sqlite3_errcode(database_);
            execute_sql(database_, "ROLLBACK;");
            return false;
        }

        sqlite3_stmt* upsert = nullptr;
        sqlite3_stmt* erase = nullptr;
        const auto upsert_sql =
            "INSERT INTO image_metadata "
            "(physical_path, file_size, modified_time, sort_time_ns, "
            "eligible, "
            "width, height, model, prompts_json, sha256, extraction_version) "
            "VALUES (?, ?, ?, "
            "?, "
            "?, ?, ?, "
            "?, ?, ?, ?) "
            "ON CONFLICT(physical_path) DO UPDATE SET "
            "file_size=excluded.file_size, "
            "modified_time=excluded.modified_time, "
            "sort_time_ns=excluded.sort_time_ns, "
            "eligible=excluded.eligible, width=excluded.width, "
            "height=excluded.height, model=excluded.model, "
            "prompts_json=excluded.prompts_json, sha256=excluded.sha256, "
            "extraction_version=excluded.extraction_version;";

        bool success =
            sqlite3_prepare_v2(database_, upsert_sql, -1, &upsert, nullptr) ==
                SQLITE_OK &&
            sqlite3_prepare_v2(
                database_,
                "DELETE FROM image_metadata WHERE physical_path=?;",
                -1,
                &erase,
                nullptr
            ) == SQLITE_OK;

        for (const auto& item : changed) {
            if (!success)
                break;
            const auto prompts = nlohmann::json(item.prompts_).dump();
            const auto path = relative_path(store, item.physical_path_);
            if (!path) {
                success = false;
                break;
            }
            sqlite3_bind_text(upsert, 1, path->c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(upsert, 2, item.file_size_);
            sqlite3_bind_int64(upsert, 3, item.modified_time_);
            sqlite3_bind_int64(upsert, 4, item.sort_time_ns_);
            sqlite3_bind_int(upsert, 5, item.eligible_ ? 1 : 0);
            sqlite3_bind_int(upsert, 6, item.width_);
            sqlite3_bind_int(upsert, 7, item.height_);
            sqlite3_bind_text(
                upsert, 8, item.model_.c_str(), -1, SQLITE_TRANSIENT
            );
            sqlite3_bind_text(upsert, 9, prompts.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(
                upsert, 10, item.sha256_.c_str(), -1, SQLITE_TRANSIENT
            );
            sqlite3_bind_int(upsert, 11, item.extraction_version_);
            success = sqlite3_step(upsert) == SQLITE_DONE;
            sqlite3_reset(upsert);
            sqlite3_clear_bindings(upsert);
        }

        for (const auto& path : removed) {
            if (!success)
                break;
            const auto relative = relative_path(store, path);
            if (!relative) {
                success = false;
                break;
            }
            sqlite3_bind_text(
                erase, 1, relative->c_str(), -1, SQLITE_TRANSIENT
            );
            success = sqlite3_step(erase) == SQLITE_DONE;
            sqlite3_reset(erase);
            sqlite3_clear_bindings(erase);
        }

        if (!success)
            store.last_error_ = sqlite3_errcode(database_);
        sqlite3_finalize(upsert);
        sqlite3_finalize(erase);
        if (success) {
            success = execute_sql(database_, "COMMIT;");
            if (!success)
                store.last_error_ = sqlite3_errcode(database_);
        }
        if (!success)
            execute_sql(database_, "ROLLBACK;");
        return success;
    }

    bool write_tag_analysis(Store& store, const CachedTagAnalysis& original) {
        auto* database_ = store.database_;
        if (!database_)
            return false;
        auto item = original;
        // Logical keys are case-folded on Windows. Persist the source
        // spelling from a physical input so a moved cache also works on
        // case-sensitive filesystems.
        const auto& input = original.input_path_.empty()
                                ? original.attempt_input_path_
                                : original.input_path_;
        if (!input.empty()) {
            const auto physical = sung::fromstr(input);
            item.logical_path_ = sung::tostr(
                sung::sprintboard_proxy_source_path(physical).value_or(physical)
            );
        }
        for (auto* path :
             {&item.logical_path_,
              &item.input_path_,
              &item.attempt_input_path_,
              &item.proxy_path_}) {
            const auto relative = relative_path(store, *path);
            if (!relative)
                return false;
            *path = *relative;
        }
        if (item.analysis_.is_object() && item.analysis_.contains("path"))
            item.analysis_["path"] = item.input_path_;

        const char* sql =
            "INSERT INTO image_tag_analysis ("
            "logical_path, input_kind, input_path, input_size, "
            "input_modified_time, input_sha256, analyzer_fingerprint, "
            "model_id, general_threshold, "
            "character_threshold, analysis_json, analyzed_at, "
            "attempt_input_path, attempt_input_size, "
            "attempt_input_modified_time, attempt_analyzer_fingerprint, "
            "last_attempt_at, failure_count, last_error, analysis_id, "
            "proxy_path, proxy_size, proxy_modified_time, "
            "proxy_sha256, proxy_materialization_id"
            ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
            "?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(logical_path) DO UPDATE SET "
            "input_kind=excluded.input_kind, "
            "input_path=excluded.input_path, "
            "input_size=excluded.input_size, "
            "input_modified_time=excluded.input_modified_time, "
            "input_sha256=excluded.input_sha256, "
            "analyzer_fingerprint=excluded.analyzer_fingerprint, "
            "model_id=excluded.model_id, "
            "general_threshold=excluded.general_threshold, "
            "character_threshold=excluded.character_threshold, "
            "analysis_json=excluded.analysis_json, "
            "analyzed_at=excluded.analyzed_at, "
            "attempt_input_path=excluded.attempt_input_path, "
            "attempt_input_size=excluded.attempt_input_size, "
            "attempt_input_modified_time="
            "excluded.attempt_input_modified_time, "
            "attempt_analyzer_fingerprint="
            "excluded.attempt_analyzer_fingerprint, "
            "last_attempt_at=excluded.last_attempt_at, "
            "failure_count=excluded.failure_count, "
            "last_error=excluded.last_error, "
            "analysis_id=excluded.analysis_id, "
            "proxy_path=excluded.proxy_path, "
            "proxy_size=excluded.proxy_size, "
            "proxy_modified_time=excluded.proxy_modified_time, "
            "proxy_sha256=excluded.proxy_sha256, "
            "proxy_materialization_id=excluded.proxy_materialization_id;";

        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) !=
            SQLITE_OK) {
            store.last_error_ = sqlite3_errcode(database_);
            return false;
        }

        const auto analysis_json = item.analysis_.is_null()
                                       ? std::string{}
                                       : item.analysis_.dump();
        const auto bind_text = [&](const int index, const std::string& value) {
            sqlite3_bind_text(
                statement, index, value.c_str(), -1, SQLITE_TRANSIENT
            );
        };
        bind_text(1, item.logical_path_);
        bind_text(2, item.input_kind_);
        bind_text(3, item.input_path_);
        sqlite3_bind_int64(statement, 4, item.input_size_);
        sqlite3_bind_int64(statement, 5, item.input_modified_time_);
        bind_text(6, item.input_sha256_);
        bind_text(7, item.analyzer_fingerprint_);
        bind_text(8, item.model_id_);
        sqlite3_bind_double(statement, 9, item.general_threshold_);
        sqlite3_bind_double(statement, 10, item.character_threshold_);
        bind_text(11, analysis_json);
        sqlite3_bind_int64(statement, 12, item.analyzed_at_);
        bind_text(13, item.attempt_input_path_);
        sqlite3_bind_int64(statement, 14, item.attempt_input_size_);
        sqlite3_bind_int64(statement, 15, item.attempt_input_modified_time_);
        bind_text(16, item.attempt_analyzer_fingerprint_);
        sqlite3_bind_int64(statement, 17, item.last_attempt_at_);
        sqlite3_bind_int(statement, 18, item.failure_count_);
        bind_text(19, item.last_error_);
        bind_text(20, item.analysis_id_);
        bind_text(21, item.proxy_path_);
        sqlite3_bind_int64(statement, 22, item.proxy_size_);
        sqlite3_bind_int64(statement, 23, item.proxy_modified_time_);
        bind_text(24, item.proxy_sha256_);
        bind_text(25, item.proxy_materialization_id_);

        const auto success = sqlite3_step(statement) == SQLITE_DONE;
        if (!success)
            store.last_error_ = sqlite3_errcode(database_);
        sqlite3_finalize(statement);
        return success;
    }

    bool tag_ready(const std::string& path) const {
        const auto* store = owner(path);
        return store && !store->blocked_ && store->database_ &&
               store->healthy_ && store->pending_tags_.empty() &&
               store->deleted_tags_.empty();
    }

    bool flush_tag_store(Store& store) {
        if (store.blocked_ || !store.database_)
            return false;
        if (store.pending_tags_.empty() && store.deleted_tags_.empty())
            return true;
        auto* db = store.database_;
        bool success = execute_sql(db, "BEGIN IMMEDIATE;");
        const bool began = success;
        if (success) {
            for (const auto& [path, item] : store.pending_tags_) {
                if (!write_tag_analysis(store, item)) {
                    success = false;
                    break;
                }
            }
        }
        sqlite3_stmt* erase = nullptr;
        if (success && !store.deleted_tags_.empty()) {
            success =
                sqlite3_prepare_v2(
                    db,
                    "DELETE FROM image_tag_analysis WHERE logical_path=?;",
                    -1,
                    &erase,
                    nullptr
                ) == SQLITE_OK;
            if (success)
                for (const auto& [path, relative] : store.deleted_tags_) {
                    sqlite3_bind_text(
                        erase, 1, relative.c_str(), -1, SQLITE_TRANSIENT
                    );
                    if (sqlite3_step(erase) != SQLITE_DONE) {
                        success = false;
                        break;
                    }
                    sqlite3_reset(erase);
                    sqlite3_clear_bindings(erase);
                }
        }
        if (!success)
            store.last_error_ = sqlite3_errcode(db);
        sqlite3_finalize(erase);
        if (success) {
            success = execute_sql(db, "COMMIT;");
            if (!success)
                store.last_error_ = sqlite3_errcode(db);
        }
        if (!success) {
            if (began)
                execute_sql(db, "ROLLBACK;");
            record_failure(store);
            return false;
        }
        for (const auto& [path, item] : store.pending_tags_) {
            store.persisted_tags_[path] = stored_logical_path(store, item);
            if (owner(path) == &store) {
                tag_analyses_.insert_or_assign(path, item);
                analysis_local_[path] = store.path_ != store.fallback_;
            }
        }
        for (const auto& [path, relative] : store.deleted_tags_)
            store.persisted_tags_.erase(path);
        store.pending_tags_.clear();
        store.deleted_tags_.clear();
        return true;
    }

    bool persist_tag_analysis(const CachedTagAnalysis& item) {
        auto* store = owner(item.logical_path_);
        if (!store)
            return false;
        store->deleted_tags_.erase(item.logical_path_);
        store->pending_tags_.insert_or_assign(item.logical_path_, item);
        return true;
    }

    bool erase_tag_analysis(const std::string& logical_path) {
        tag_analyses_.erase(logical_path);
        for (auto& [key, store] : stores_) {
            store->pending_tags_.erase(logical_path);
            if (const auto it = store->persisted_tags_.find(logical_path);
                it != store->persisted_tags_.end())
                store->deleted_tags_[logical_path] = it->second;
        }
        return true;
    }

    void flush_stores(
        const std::vector<CachedMetadata>& changed,
        const std::vector<std::string>& removed,
        const bool reconcile_ownership = true
    ) {
        for (auto& [key, store] : stores_) {
            if (store->blocked_)
                continue;
            if (!store->active_) {
                flush_tag_store(*store);
                continue;
            }
            if (!store->database_) {
                store->dirty_ = true;
                continue;
            }
            std::vector<CachedMetadata> items;
            std::vector<std::string> erased;
            if (store->dirty_) {
                for (const auto& [path, item] : metadata_)
                    if (owner(path) == store.get())
                        items.push_back(item);
            } else {
                for (const auto& item : changed)
                    if (owner(item.physical_path_) == store.get())
                        items.push_back(item);
                for (const auto& path : removed)
                    if (owner(path) == store.get())
                        erased.push_back(path);
            }
            const bool metadata_ok =
                persist_changes(*store, items, erased, store->dirty_);
            store->dirty_ = !metadata_ok;
            const bool tags_ok = flush_tag_store(*store);
            store->healthy_ = metadata_ok && tags_ok;
            if (!store->healthy_)
                record_failure(*store);
        }
        // A previous owner is pruned only after the new owner has
        // committed.
        for (auto& [key, store] : stores_) {
            for (const auto& [path, relative] : store->persisted_tags_) {
                if (!reconcile_ownership)
                    break;
                auto* destination = owner(path);
                if (destination && destination != store.get() &&
                    destination->persisted_tags_.contains(path) &&
                    !destination->pending_tags_.contains(path))
                    store->deleted_tags_[path] = relative;
            }
            if (!store->deleted_tags_.empty())
                flush_tag_store(*store);
            if (store->active_) {
                const bool paused = !store->database_ || store->blocked_ ||
                                    !store->healthy_ ||
                                    !store->pending_tags_.empty() ||
                                    !store->deleted_tags_.empty();
                if (paused != store->paused_) {
                    std::println(
                        "ImageTagger: {} tagging for root {}.",
                        paused ? "Pausing" : "Resuming",
                        key
                    );
                    store->paused_ = paused;
                }
            }
        }
        std::erase_if(stores_, [&](const auto& entry) {
            const auto& store = *entry.second;
            if (store.active_ || !store.pending_tags_.empty() ||
                !store.deleted_tags_.empty())
                return false;
            for (const auto& [path, relative] : store.persisted_tags_)
                if (owner(path))
                    return false;
            return true;
        });
    }

    std::map<std::string, std::unique_ptr<Store>> stores_;
    std::set<std::string> root_keys_;
    std::map<std::string, std::string> binding_roots_;
    std::unordered_map<std::string, CachedMetadata> metadata_;
    std::unordered_map<std::string, CachedTagAnalysis> tag_analyses_;
    std::unordered_map<std::string, bool> analysis_local_;
    std::atomic_bool& cache_loading_;
    const std::function<void(std::string_view)>& scan_observer_;
};

class sung::ImageIndex::Impl {
public:
    void set_scan_observer(std::function<void(std::string_view)> observer) {
        scan_observer_ = std::move(observer);
        snapshot_writer_ =
            std::make_unique<gallery_cache::SnapshotWriter>(scan_observer_);
        storage_thread_ = std::thread([this] { storage_loop(); });
    }

    ~Impl() {
        {
            std::lock_guard lock{storage_mutex_};
            storage_stopping_ = true;
        }
        storage_cv_.notify_all();
        if (storage_thread_.joinable())
            storage_thread_.join();
        snapshot_writer_.reset();
    }

    bool loading_cache() const {
        return cache_loading_.load();
    }

    static Path canonical_path(const Path& path) {
        return IndexStorage::canonical_path(path);
    }

    struct RootStatus {
        Path root_;
        bool healthy_ = false;
        bool blocked_ = false;
        bool ready_ = false;
    };

    struct TagWrite {
        uint64_t version_ = 0;
        std::optional<CachedTagAnalysis> value_;
    };

    struct StorageBatch {
        std::shared_ptr<const ServerConfigs> configs_;
        std::string signature_;
        uint64_t epoch_ = 0;
        uint64_t sequence_ = 0;
        bool hydrate_ = false;
        bool reconcile_ = false;
        std::unordered_map<std::string, std::optional<CachedMetadata>>
            metadata_;
        std::unordered_map<std::string, TagWrite> tags_;
    };

    // Called with refresh_mutex_. Queue locks never cover I/O or callbacks.
    uint64_t enqueue_storage(StorageBatch batch) {
        batch.configs_ = snapshot_configs_;
        batch.signature_ = storage_signature_;
        batch.epoch_ = storage_epoch_;
        batch.sequence_ = ++storage_sequence_;
        const auto sequence = batch.sequence_;
        for (auto& [path, write] : batch.tags_) {
            write.version_ = sequence;
            tag_versions_[path] = sequence;
            pending_tags_[path] = write;
        }
        for (const auto& [path, value] : batch.metadata_)
            metadata_versions_[path] = sequence;
        {
            std::lock_guard lock{storage_mutex_};
            if (!storage_batches_.empty() &&
                storage_batches_.back().epoch_ == batch.epoch_) {
                auto& pending = storage_batches_.back();
                pending.sequence_ = sequence;
                pending.hydrate_ |= batch.hydrate_;
                pending.reconcile_ |= batch.reconcile_;
                for (auto& [path, item] : batch.metadata_)
                    pending.metadata_.insert_or_assign(path, std::move(item));
                for (auto& [path, item] : batch.tags_)
                    pending.tags_.insert_or_assign(path, std::move(item));
            } else {
                storage_batches_.push_back(std::move(batch));
            }
        }
        storage_cv_.notify_one();
        return sequence;
    }

    void wait_storage(uint64_t sequence) {
        std::unique_lock lock{storage_mutex_};
        storage_cv_.wait(lock, [&] { return storage_completed_ >= sequence; });
    }

    const RootStatus* owner(const std::string& path) const {
        const auto normalized = fromstr(make_path_key(fromstr(path)));
        const RootStatus* result = nullptr;
        size_t depth = 0;
        for (const auto& [key, status] : root_status_) {
            const auto relative = normalized.lexically_relative(fromstr(key));
            if (!relative.empty() && *relative.begin() != ".." &&
                key.size() > depth) {
                result = &status;
                depth = key.size();
            }
        }
        return result;
    }

    bool tag_ready(const std::string& path) const {
        const auto* root = owner(path);
        if (!root || !root->ready_)
            return false;
        for (const auto& [pending, write] : pending_tags_)
            if (owner(pending) == root)
                return false;
        return true;
    }

    bool persist_tag_analysis(const CachedTagAnalysis& item) {
        const auto* root = owner(item.logical_path_);
        if (!root || root->blocked_)
            return false;
        StorageBatch batch;
        batch.tags_[item.logical_path_].value_ = item;
        enqueue_storage(std::move(batch));
        return true;
    }

    bool erase_tag_analysis(const std::string& path) {
        StorageBatch batch;
        batch.tags_[path] = {};
        enqueue_storage(std::move(batch));
        return true;
    }

    uint64_t flush_stores(
        const std::vector<CachedMetadata>& changed,
        const std::vector<std::string>& removed,
        bool reconcile = true
    ) {
        if (!snapshot_configs_)
            return 0;
        StorageBatch batch;
        batch.reconcile_ = reconcile;
        for (const auto& item : changed)
            batch.metadata_[item.physical_path_] = item;
        for (const auto& path : removed) batch.metadata_[path] = std::nullopt;
        return enqueue_storage(std::move(batch));
    }

    // Resolve bindings before acquiring refresh_mutex_.
    static std::map<std::string, Path> resolved_roots(
        const ServerConfigs& configs
    ) {
        std::map<std::string, Path> roots;
        for (const auto& [name, binding] : configs.dir_bindings_)
            roots[name] = canonical_path(binding.local_dir_);
        return roots;
    }

    void configure_storage(
        const std::shared_ptr<const ServerConfigs>& configs,
        const std::map<std::string, Path>& roots
    ) {
        snapshot_configs_ = configs;
        const auto signature = scan_bindings(*configs);
        if (signature == storage_signature_)
            return;
        storage_signature_ = signature;
        ++storage_epoch_;
        hydration_sequence_ = 0;
        binding_roots_ = roots;
        root_status_.clear();
        for (const auto& [name, root] : roots)
            root_status_[make_path_key(root)].root_ = root;
        std::erase_if(tag_analyses_, [&](const auto& entry) {
            return !owner(entry.first);
        });
    }

    void storage_loop() {
        IndexStorage storage{cache_loading_, scan_observer_};
        std::unordered_map<std::string, uint64_t> versions, metadata_versions;
        std::optional<StorageBatch> retry;
        auto retry_at = std::chrono::steady_clock::time_point::max();
        int failures = 0;
        for (;;) {
            StorageBatch batch;
            bool stopping = false;
            {
                std::unique_lock lock{storage_mutex_};
                storage_cv_.wait_until(lock, retry_at, [&] {
                    return storage_stopping_ || !storage_batches_.empty();
                });
                stopping = storage_stopping_;
                if (retry &&
                    (!retry->metadata_.empty() || !retry->tags_.empty())) {
                    batch = std::move(*retry);
                    retry.reset();
                } else if (!storage_batches_.empty()) {
                    batch = std::move(storage_batches_.front());
                    storage_batches_.pop_front();
                } else if (retry) {
                    batch = *retry;
                } else if (stopping) {
                    break;
                } else {
                    continue;
                }
            }
            bool failed = false;
            bool staged = false;
            try {
                sung::MonotonicRealtimeTimer timer;
                storage.reconcile_stores(*batch.configs_);
                std::vector<CachedMetadata> changed;
                std::vector<std::string> removed;
                for (const auto& [path, value] : batch.metadata_) {
                    if (metadata_versions[path] > batch.sequence_)
                        continue;
                    metadata_versions[path] = batch.sequence_;
                    if (value) {
                        storage.metadata_[path] = *value;
                        changed.push_back(*value);
                    } else {
                        storage.metadata_.erase(path);
                        removed.push_back(path);
                    }
                }
                for (const auto& [path, write] : batch.tags_) {
                    if (versions[path] > write.version_)
                        continue;
                    versions[path] = write.version_;
                    if (write.value_)
                        storage.persist_tag_analysis(*write.value_);
                    else
                        storage.erase_tag_analysis(path);
                }
                staged = true;
                if (scan_observer_)
                    scan_observer_("storage-write");
                storage.flush_stores(changed, removed, batch.reconcile_);
                std::map<std::string, RootStatus> statuses;
                std::unordered_set<std::string> uncommitted;
                for (const auto& [key, store] : storage.stores_) {
                    for (const auto& [path, value] : store->pending_tags_)
                        uncommitted.insert(path);
                    for (const auto& [path, value] : store->deleted_tags_)
                        uncommitted.insert(path);
                    if (!store->active_)
                        continue;
                    statuses[key] = {
                        store->root_,
                        store->healthy_,
                        store->blocked_,
                        store->database_ && store->healthy_ &&
                            !store->blocked_ && store->pending_tags_.empty() &&
                            store->deleted_tags_.empty()
                    };
                    failed |= !statuses[key].ready_;
                }
                {
                    std::lock_guard lock{refresh_mutex_};
                    // Drain UI tombstones before considering commit
                    // completions.
                    flush_pending_removals_locked();
                    if (storage_epoch_ == batch.epoch_) {
                        root_status_ = std::move(statuses);
                        if (batch.hydrate_)
                            for (const auto& [path, item] : storage.metadata_)
                                if (!metadata_versions_.contains(path) &&
                                    owner(path))
                                    metadata_.try_emplace(path, item);
                        bool changed_tags = false;
                        for (const auto& [path, item] : storage.tag_analyses_) {
                            const auto version =
                                versions.contains(path) ? versions.at(path) : 0;
                            if (!owner(path) ||
                                (version != 0 && uncommitted.contains(path)) ||
                                tag_versions_[path] != version)
                                continue;
                            const auto previous = tag_analyses_.find(path);
                            const bool changed =
                                previous == tag_analyses_.end() ||
                                previous->second.analysis_id_ !=
                                    item.analysis_id_ ||
                                previous->second.input_modified_time_ !=
                                    item.input_modified_time_;
                            tag_analyses_[path] = item;
                            if (changed && !item.analysis_.is_null())
                                proxy_queue_.enqueue(
                                    fromstr(path), false, true
                                );
                            changed_tags |= changed;
                        }
                        for (auto it = pending_tags_.begin();
                             it != pending_tags_.end();) {
                            if (versions[it->first] == it->second.version_ &&
                                !uncommitted.contains(it->first))
                                it = pending_tags_.erase(it);
                            else
                                ++it;
                        }
                        if (changed_tags) {
                            auto next = std::make_shared<IndexSnapshot>(
                                *load_snapshot()
                            );
                            next->tag_analyses_ = tag_analyses_;
                            for (auto& file : next->files_) {
                                // Restored browsing entries acquire logical
                                // keys only when a content scan reconciles
                                // them.
                                if (file.logical_path_.empty())
                                    continue;
                                const auto found =
                                    tag_analyses_.find(file.logical_path_);
                                file.tags_ =
                                    found == tag_analyses_.end()
                                        ? std::vector<std::string>{}
                                        : found->second.searchable_tags_;
                            }
                            store_snapshot(std::move(next));
                        }
                    }
                }
                if (timer.elapsed() >= 1)
                    std::println(
                        "ImageIndex: Storage batch {} took {:.3f} seconds.",
                        batch.sequence_,
                        timer.elapsed()
                    );
            } catch (const std::exception& error) {
                failed = true;
                std::println(
                    "ImageIndex: Storage batch failed: {}", error.what()
                );
            }
            {
                std::lock_guard lock{storage_mutex_};
                storage_completed_ =
                    std::max(storage_completed_, batch.sequence_);
            }
            storage_cv_.notify_all();
            if (failed) {
                // Storage retains dirty metadata and uncommitted tag
                // operations. Replaying a metadata delta is unnecessary after a
                // failed flush.
                if (staged) {
                    batch.metadata_.clear();
                    batch.tags_.clear();
                }
                retry = std::move(batch);
                retry_at = std::chrono::steady_clock::now() +
                           std::chrono::seconds{
                               std::min(1 << std::min(failures++, 5), 30)
                           };
            } else {
                retry.reset();
                retry_at = std::chrono::steady_clock::time_point::max();
                if (failures)
                    std::println("ImageIndex: Storage recovered.");
                failures = 0;
            }
            if (stopping) {
                std::lock_guard lock{storage_mutex_};
                if (storage_batches_.empty()) {
                    if (failed)
                        std::println(
                            stderr,
                            "ImageIndex: Pending storage work could not be "
                            "saved at shutdown."
                        );
                    break;
                }
            }
        }
    }

    ImageIndexRefreshStats initialize(
        const std::shared_ptr<const ServerConfigs>& configs
    ) {
        sung::MonotonicRealtimeTimer timer;
        auto cached = gallery_cache::load(*configs);
        std::lock_guard refresh_lock{refresh_mutex_};
        snapshot_configs_ = configs;
        // Namespaces are available immediately even without a saved snapshot,
        // so a cold-start visit can queue its root before discovery finishes.
        for (const auto& [name, binding] : configs->dir_bindings_)
            cached->namespaces_.insert(name);
        ImageIndexRefreshStats stats;
        stats.images_available_ = cached->files_.size();
        stats.folders_available_ = cached->folders_.size();
        stats.elapsed_seconds_ = timer.elapsed();
        snapshots_.store(std::move(cached));
        std::println(
            "ImageIndex: Restored {} images and {} folders in {:.3f} seconds; "
            "reconciliation pending.",
            stats.images_available_,
            stats.folders_available_,
            stats.elapsed_seconds_
        );
        return stats;
    }

    void queue_new_dates(
        const ServerConfigs& configs,
        const IndexSnapshot& previous,
        const IndexSnapshot& next
    ) {
        const auto signature = scan_bindings(configs);
        if (priority_bindings_ != signature) {
            priority_bindings_ = signature;
            priority_folders_.clear();
            priority_pending_.clear();
            priority_retries_.clear();
            ++priority_epoch_;
        }
        std::set<std::pair<std::string, std::string>> known;
        for (const auto& item : previous.folders_)
            known.emplace(item.root_key_, item.path_);
        for (const auto& item : next.folders_)
            if (parse_date_folder(item.name_) &&
                !known.contains({item.root_key_, item.path_}) &&
                priority_pending_.insert(item.path_).second)
                priority_folders_.push_back(item.path_);
    }

    void poll_folders(const std::shared_ptr<const ServerConfigs>& configs) {
        const auto resolved = resolved_roots(*configs);
        const auto input = load_snapshot();
        for (const auto& [name, binding] : configs->dir_bindings_) {
            const auto root = resolved.at(name);
            const auto key = make_root_key(name, root);
            std::set<std::string> parents{name};
            const auto is_container = [&](const Path& relative) {
                for (const auto& part : root.filename() / relative)
                    if (parse_date_folder(tostr(part)))
                        return false;
                return true;
            };
            if (!is_container({}))
                continue;
            for (const auto& item : input->folders_)
                if (item.root_key_ == key &&
                    is_container(
                        fromstr(item.path_).lexically_relative(fromstr(name))
                    ))
                    parents.insert(item.path_);
            for (const auto& parent : parents) {
                sung::MonotonicRealtimeTimer timer;
                detail::ScanJournal::Transaction transaction{
                    scan_journal_, refresh_mutex_, scan_bindings(*configs)
                };
                configure_storage(configs, resolved);
                transaction.begin_observations();
                const auto physical =
                    root / fromstr(parent).lexically_relative(fromstr(name));
                std::error_code error;
                // Directory symlinks are listed, but never followed by
                // discovery.
                if (physical != root && fs::is_symlink(physical, error))
                    continue;
                error.clear();
                std::vector<IndexedFolder> children;
                fs::directory_iterator it{physical, error}, end;
                while (!error && it != end) {
                    const auto& entry = *it;
                    if (entry.path().filename() != ".sprintboard" &&
                        !is_sprintboard_temporary_path(entry.path()) &&
                        !is_sprintboard_tag_sidecar_path(entry.path()) &&
                        entry.is_directory(error) && !error) {
                        const auto path =
                            fromstr(parent) / entry.path().filename();
                        children.push_back(
                            {key,
                             tostr(entry.path().filename()),
                             tostr(path),
                             parent,
                             get_image_sort_time(entry.path())}
                        );
                    }
                    if (!error)
                        it.increment(error);
                }
                if (error)
                    continue;
                if (scan_observer_)
                    scan_observer_("folder-poll");
                transaction.lock();
                if (!transaction.current())
                    continue;
                const auto previous = load_snapshot();
                auto next = std::make_shared<IndexSnapshot>(*previous);
                const auto selected = [&](const IndexedFolder& item) {
                    return item.root_key_ == key &&
                           item.parent_path_ == parent &&
                           !transaction.superseded(
                               root / fromstr(item.path_)
                                          .lexically_relative(fromstr(name)),
                               true
                           );
                };
                std::map<std::string, int64_t> before, after;
                for (const auto& item : next->folders_)
                    if (selected(item))
                        before[item.path_] = item.sort_time_ns_;
                std::erase_if(next->folders_, selected);
                for (const auto& item : children)
                    if (selected(item)) {
                        next->folders_.push_back(item);
                        after[item.path_] = item.sort_time_ns_;
                    }
                transaction.publish([physical](const Path& path, bool folders) {
                    return folders && make_path_key(path.parent_path()) ==
                                          make_path_key(physical);
                });
                if (before == after) {
                    transaction.unlock();
                    if (timer.elapsed() >= 1)
                        std::println(
                            "ImageIndex: Folder poll of {} took {:.3f} "
                            "seconds.",
                            parent,
                            timer.elapsed()
                        );
                    continue;
                }
                next->namespaces_.insert(name);
                queue_new_dates(*configs, *previous, *next);
                {
                    std::lock_guard lock{visits_mutex_};
                    for (const auto& [path, time] : after)
                        if (!before.contains(path))
                            unscanned_folders_.insert(path);
                    for (const auto& [path, time] : before)
                        if (!after.contains(path))
                            unscanned_folders_.erase(path);
                }
                store_snapshot(std::move(next));
                transaction.unlock();
                std::println(
                    "ImageIndex: Folder poll updated {} ({} children) in "
                    "{:.3f} seconds.",
                    parent,
                    after.size(),
                    timer.elapsed()
                );
            }
        }
    }

    void defer_priority_folder(const std::string& folder) {
        auto& [due, failures] = priority_retries_[folder];
        due = std::chrono::steady_clock::now() +
              std::chrono::seconds{std::min(1 << std::min(failures++, 5), 30)};
        priority_folders_.push_back(folder);
    }

    void refresh_new_folder(
        const std::shared_ptr<const ServerConfigs>& configs
    ) {
        std::string folder;
        uint64_t epoch = 0;
        const auto signature = scan_bindings(*configs);
        {
            std::lock_guard lock{refresh_mutex_};
            if (priority_bindings_ != signature || priority_folders_.empty())
                return;
            epoch = priority_epoch_;
            const auto now = std::chrono::steady_clock::now();
            for (size_t remaining = priority_folders_.size(); remaining > 0;
                 --remaining) {
                auto candidate = std::move(priority_folders_.front());
                priority_folders_.pop_front();
                const auto retry = priority_retries_.find(candidate);
                if (retry != priority_retries_.end() &&
                    retry->second.first > now) {
                    priority_folders_.push_back(std::move(candidate));
                    continue;
                }
                folder = std::move(candidate);
                break;
            }
            if (folder.empty())
                return;
        }
        try {
            refresh_folder(configs, fromstr(folder), false, false);
        } catch (...) {
            std::lock_guard lock{refresh_mutex_};
            if (priority_bindings_ == signature && priority_epoch_ == epoch)
                defer_priority_folder(folder);
            throw;
        }
        // Check links outside the publication lock before queuing descendants.
        std::vector<std::string> children;
        for (const auto& item : load_snapshot()->folders_) {
            if (item.parent_path_ != folder)
                continue;
            const auto split = item.root_key_.find('\n');
            const auto physical =
                fromstr(item.root_key_.substr(split + 1)) /
                fromstr(item.path_)
                    .lexically_relative(
                        fromstr(item.root_key_.substr(0, split))
                    );
            std::error_code error;
            if (!fs::is_symlink(physical, error) && !error)
                children.push_back(item.path_);
        }
        std::lock_guard lock{refresh_mutex_};
        if (priority_bindings_ != signature || priority_epoch_ != epoch)
            return;
        std::lock_guard visits_lock{visits_mutex_};
        if (unscanned_folders_.contains(folder)) {
            defer_priority_folder(folder);
            return;
        }
        priority_pending_.erase(folder);
        priority_retries_.erase(folder);
        for (const auto& child : children)
            if (unscanned_folders_.contains(child) &&
                priority_pending_.insert(child).second)
                priority_folders_.push_back(child);
    }

    void discover_folders(
        const std::shared_ptr<const ServerConfigs>& configs,
        bool synchronous = true
    ) {
        const auto resolved = resolved_roots(*configs);
        detail::ScanJournal::Transaction transaction{
            scan_journal_, refresh_mutex_, scan_bindings(*configs)
        };
        configure_storage(configs, resolved);
        flush_pending_removals_locked();
        transaction.begin_observations();

        struct Discovery {
            Path root_;
            std::string key_;
            std::vector<IndexedFolder> folders_;
            int64_t sort_time_ = 0;
            bool available_ = false;
        };

        std::map<std::string, Discovery> results;
        for (const auto& [name, binding] : configs->dir_bindings_) {
            auto& result = results[name];
            const auto root = canonical_path(binding.local_dir_);
            const auto key = make_root_key(name, root);
            result.root_ = root;
            result.key_ = key;
            auto& discovered = result.folders_;
            std::error_code error;
            fs::recursive_directory_iterator it{ root, error }, end;
            while (!error && it != end) {
                const auto& entry = *it;
                if (entry.path().filename() == ".sprintboard" ||
                    is_sprintboard_temporary_path(entry.path()) ||
                    is_sprintboard_tag_sidecar_path(entry.path())) {
                    it.disable_recursion_pending();
                } else if (entry.is_directory(error) && !error) {
                    const auto path = fromstr(name) /
                                      entry.path().lexically_relative(root);
                    discovered.push_back(
                        { key,
                          tostr(entry.path().filename()),
                          tostr(path),
                          tostr(path.parent_path()),
                          get_image_sort_time(entry.path()) }
                    );
                }
                if (!error)
                    it.increment(error);
            }
            if (error) {
                std::println(
                    "ImageIndex: Folder discovery preserving {}: {}",
                    name,
                    error.message()
                );
                continue;
            }
            result.sort_time_ = get_image_sort_time(root);
            result.available_ = true;
        }
        if (scan_observer_)
            scan_observer_("discovery");
        transaction.lock();
        if (!transaction.current())
            return;
        flush_pending_removals_locked();
        const auto previous = load_snapshot();
        auto next = std::make_shared<IndexSnapshot>(*previous);
        next->namespaces_.clear();
        std::set<std::string> roots;
        std::vector<std::string> scanned_roots;
        for (const auto& [name, result] : results) {
            const auto& key = result.key_;
            roots.insert(key);
            next->namespaces_.insert(name);
            if (!result.available_)
                continue;
            const auto physical = [&](const auto& item) {
                return result.root_ /
                       fromstr(item.path_).lexically_relative(fromstr(name));
            };
            std::erase_if(next->folders_, [&](const auto& item) {
                return item.root_key_ == key &&
                       !transaction.superseded(physical(item), true);
            });
            for (const auto& item : result.folders_)
                if (!transaction.superseded(physical(item), true))
                    next->folders_.push_back(item);
            if (!transaction.superseded(result.root_, true))
                next->namespace_sort_times_[name] = result.sort_time_;
            scanned_roots.push_back(make_path_key(result.root_));
        }
        transaction.publish([scanned_roots = std::move(scanned_roots)](
                                const Path& path, bool folders
                            ) {
            if (!folders)
                return false;
            const auto normalized = fromstr(make_path_key(path));
            for (const auto& root : scanned_roots) {
                const auto relative =
                    normalized.lexically_relative(fromstr(root));
                if (!relative.empty() && *relative.begin() != "..")
                    return true;
            }
            return false;
        });
        std::erase_if(next->folders_, [&](const auto& item) {
            return !roots.contains(item.root_key_);
        });
        std::erase_if(next->files_, [&](const auto& item) {
            return !roots.contains(item.root_key_);
        });
        std::erase_if(next->namespace_sort_times_, [&](const auto& item) {
            return !next->namespaces_.contains(item.first);
        });
        std::sort(
            next->folders_.begin(),
            next->folders_.end(),
            [](const auto& a, const auto& b) { return a.path_ < b.path_; }
        );
        {
            std::lock_guard visits_lock{ visits_mutex_ };
            std::set<std::pair<std::string, std::string>> known;
            std::set<std::string> discovered_paths;
            for (const auto& item : previous->folders_)
                known.emplace(item.root_key_, item.path_);
            for (const auto& item : next->folders_) {
                discovered_paths.insert(item.path_);
                if (!known.contains({ item.root_key_, item.path_ }))
                    unscanned_folders_.insert(item.path_);
            }
            std::erase_if(unscanned_folders_, [&](const auto& path) {
                return !discovered_paths.contains(path);
            });
        }
        const auto folder_times = [](const auto& folders) {
            std::map<std::pair<std::string, std::string>, int64_t> times;
            for (const auto& folder : folders)
                times[{ folder.root_key_, folder.path_ }] =
                    folder.sort_time_ns_;
            return times;
        };
        if (next->namespaces_ == previous->namespaces_ &&
            next->namespace_sort_times_ == previous->namespace_sort_times_ &&
            next->files_.size() == previous->files_.size() &&
            folder_times(next->folders_) == folder_times(previous->folders_))
            return;
        queue_new_dates(*configs, *previous, *next);
        ++next->generation_;
        store_snapshot(std::move(next));
        const auto ticket = snapshot_ticket_;
        transaction.unlock();
        if (synchronous)
            snapshot_writer_->wait(ticket);
    }

    ImageIndexRefreshStats refresh_folder(
        const std::shared_ptr<const ServerConfigs>& configs,
        const Path& directory,
        bool recursive = false,
        bool synchronous = true
    ) {
        const auto normalized = directory.lexically_normal();
        for (const auto& part : normalized)
            if (part == ".." || part == ".sprintboard")
                throw std::invalid_argument("Invalid visited directory");
        for (const auto& [name, binding] : configs->dir_bindings_) {
            const auto relative = normalized.lexically_relative(fromstr(name));
            if (relative.empty() || *relative.begin() == "..")
                continue;
            const auto root = canonical_path(binding.local_dir_);
            const auto physical = concat_path_safely(root, relative);
            if (!physical)
                throw std::invalid_argument("Visited directory outside root");
            return refresh(
                configs, {}, canonical_path(*physical), recursive, synchronous
            );
        }
        throw std::invalid_argument("Unknown visited directory");
    }

    void visit_page(
        const Path& directory, const nlohmann::json&, bool recursive
    ) {
        std::lock_guard lock{ visits_mutex_ };
        const auto enqueue = [&](const Path& path) {
            const auto name = tostr(path.lexically_normal());
            if (name.empty() || name == ".")
                return;
            if (const auto it = pending_visits_.find(name);
                it != pending_visits_.end()) {
                it->second |= recursive;
                return;
            }
            if (pending_visits_.size() >= 256)
                return;
            pending_visits_.emplace(name, recursive);
            visits_.push_back(name);
        };
        if (recursive && (directory.empty() || directory == ".")) {
            for (const auto& name : load_snapshot()->namespaces_)
                enqueue(fromstr(name));
        } else
            enqueue(directory);
    }

    size_t unscanned_folders(const Path& directory) const {
        std::lock_guard lock{ visits_mutex_ };
        return std::count_if(
            unscanned_folders_.begin(),
            unscanned_folders_.end(),
            [&](const auto& path) {
                return is_descendant_or_child(tostr(directory), path);
            }
        );
    }

    void refresh_next_visit(
        const std::shared_ptr<const ServerConfigs>& configs
    ) {
        std::string directory;
        bool recursive = false;
        {
            std::lock_guard lock{ visits_mutex_ };
            // Rotate deferred visits once, so a fresh folder cannot block
            // another folder whose scan is already due.
            const auto now = std::chrono::steady_clock::now();
            for (size_t remaining = visits_.size(); remaining > 0;
                 --remaining) {
                auto candidate = std::move(visits_.front());
                visits_.pop_front();
                const auto last = visited_at_.find(candidate);
                if (last != visited_at_.end() &&
                    std::chrono::duration<double>(now - last->second).count() <
                        configs->image_index_visit_refresh_interval_seconds_) {
                    visits_.push_back(std::move(candidate));
                    continue;
                }
                directory = std::move(candidate);
                recursive = pending_visits_.at(directory);
                break;
            }
            if (directory.empty())
                return;
        }
        try {
            refresh_folder(configs, fromstr(directory), recursive, false);
        } catch (const std::exception& error) {
            std::println(
                "ImageIndex: Visit refresh failed for {}: {}",
                directory,
                error.what()
            );
        }
        std::lock_guard lock{ visits_mutex_ };
        visited_at_[directory] = std::chrono::steady_clock::now();
        // An HTTP request may upgrade a shallow visit while it is running.
        if (!recursive && pending_visits_.at(directory))
            visits_.push_back(directory);
        else
            pending_visits_.erase(directory);
    }

    uint64_t generation() const { return load_snapshot()->generation_; }

    ImageIndexRefreshStats refresh(
        const std::shared_ptr<const ServerConfigs>& configs,
        const DateRange& date = {},
        const Path& folder = {},
        bool recursive = false,
        bool synchronous = true
    ) {
        sung::MonotonicRealtimeTimer timer;
        ImageIndexRefreshStats stats;
        const auto within = [](const Path& path, const Path& parent) {
            const auto relative =
                fromstr(make_path_key(path))
                    .lexically_relative(fromstr(make_path_key(parent)));
            return !relative.empty() && *relative.begin() != "..";
        };
        const auto folder_selected = [&](const Path& directory) {
            return recursive
                       ? within(directory, folder)
                       : make_path_key(directory) == make_path_key(folder);
        };
        const auto scope_contents = [&](const Path& directory,
                                        const Path& root) {
            if (!folder.empty())
                return folder_selected(directory);
            return date.empty() ||
                   date_scope(directory, root, date) == DateScope::selected;
        };
        const auto scope_folders = [&](const Path& path, const Path& root) {
            if (!folder.empty())
                return recursive
                           ? within(path, folder)
                           : make_path_key(path) == make_path_key(folder) ||
                                 make_path_key(path.parent_path()) ==
                                     make_path_key(folder);
            return date.empty() ||
                   date_scope(path, root, date) != DateScope::excluded;
        };
        const auto roots = resolved_roots(*configs);
        detail::ScanJournal::Transaction transaction{
            scan_journal_, refresh_mutex_, scan_bindings(*configs)
        };
        configure_storage(configs, roots);
        if (!hydration_sequence_) {
            StorageBatch batch;
            batch.hydrate_ = true;
            hydration_sequence_ = enqueue_storage(std::move(batch));
        }
        const auto hydration = hydration_sequence_;
        transaction.unlock();
        wait_storage(hydration);
        transaction.lock();
        if (!transaction.current()) {
            stats.superseded_ = true;
            return stats;
        }
        flush_pending_removals_locked();

        struct RootScan {
            Path root_;
            bool selected_ = false;
            bool available_ = false;
            int64_t sort_time_ = 0;
            std::vector<Path> physical_files_;
            std::vector<IndexedFolder> folders_;
            std::unordered_map<std::string, Path> proxy_sources_;
            std::vector<FileProbe> probes_;
            std::vector<std::optional<IndexedFile>> entries_;
        };

        std::map<std::string, RootScan> results;
        for (const auto& [name, binding] : configs->dir_bindings_)
            results[name].root_ = roots.at(name);
        std::unordered_map<std::string, CachedMetadata> input_metadata;
        for (const auto& [path, item] : metadata_) {
            const auto directory = fromstr(path).parent_path();
            for (const auto& [name, result] : results)
                if (within(directory, result.root_) &&
                    scope_contents(directory, result.root_)) {
                    input_metadata.emplace(path, item);
                    break;
                }
        }
        std::vector<std::string> tag_paths;
        for (const auto& [path, item] : tag_analyses_)
            tag_paths.push_back(path);
        transaction.begin_observations();
        if (!folder.empty()) {
            std::println(
                "ImageIndex: Scanning visited {} {}...",
                recursive ? "subtree" : "folder",
                tostr(folder)
            );
        } else if (!date.empty()) {
            std::println(
                "ImageIndex: Scanning date folders from {} through {}...",
                date.first_,
                date.last_
            );
        } else {
            std::println(
                "ImageIndex: Reconciling the library in the background..."
            );
        }
        // Separate arenas bound each kind of scan without sharing its budget.
        tbb::task_arena arena{
            !folder.empty() || !date.empty() ? 4 : SCAN_CONCURRENCY
        };
        for (auto& [namespace_name, result] : results) {
            const auto& root = result.root_;
            const auto root_key = make_root_key(namespace_name, root);
            if (!folder.empty() && !within(folder, root) &&
                !(recursive && within(root, folder)))
                continue;
            if (!date.empty() &&
                date_scope(root, root, date) == DateScope::excluded)
                continue;
            result.selected_ = true;
            std::error_code ec;
            if (!fs::is_directory(root, ec) || ec) {
                std::println(
                    "ImageIndex: Root unavailable, preserving previous "
                    "snapshot: {} ({})",
                    tostr(root),
                    ec ? ec.message() : "not a directory"
                );
                continue;
            }
            result.sort_time_ = get_image_sort_time(root);
            auto& physical_files = result.physical_files_;
            auto& root_folders = result.folders_;
            const auto scan_root =
                folder.empty() || within(root, folder) ? root : folder;
            fs::recursive_directory_iterator iterator{
                scan_root,
                date.empty() && folder.empty()
                    ? fs::directory_options::skip_permission_denied
                    : fs::directory_options::none,
                ec
            };
            const bool directory_opened = !ec;
            // A removed visited directory has no remaining contents in scope.
            // Unavailable roots were handled above and must retain their cache.
            if (!folder.empty() && ec == std::errc::no_such_file_or_directory)
                ec.clear();
            bool scan_failed = static_cast<bool>(ec);
            Path scan_path = root;
            const char* scan_operation = "open directory";
            const fs::recursive_directory_iterator end;
            while (!scan_failed && iterator != end) {
                const auto entry = *iterator;
                if (!folder.empty() && !recursive)
                    iterator.disable_recursion_pending();
                scan_path = entry.path();
                scan_operation = "read entry metadata";
                // Exclude reserved names before any status call. In particular,
                // inaccessible legacy artifacts must not abort image discovery.
                if (entry.path().filename() == ".sprintboard" ||
                    sung::is_sprintboard_temporary_path(entry.path()) ||
                    sung::is_sprintboard_tag_sidecar_path(entry.path()) ||
                    (!date.empty() && date_scope(entry.path(), root, date) ==
                                          DateScope::excluded)) {
                    iterator.disable_recursion_pending();
                    scan_operation = "advance directory iterator";
                    iterator.increment(ec);
                    if (ec)
                        scan_failed = true;
                    continue;
                }
                if (entry.is_directory(ec) && !ec) {
                    const auto relative = entry.path().lexically_relative(root);
                    const auto namespace_path = sung::fromstr(namespace_name);
                    const auto browser_path =
                        sung::tostr(namespace_path / relative);
                    root_folders.push_back({
                        root_key,
                        sung::tostr(entry.path().filename()),
                        browser_path,
                        sung::tostr((namespace_path / relative).parent_path()),
                        get_image_sort_time(entry.path()),
                    });
                } else if (
                    !ec &&
                    (date.empty() ||
                     date_scope(entry.path().parent_path(), root, date) ==
                         DateScope::selected) &&
                    entry.is_regular_file(ec) && !ec
                ) {
                    auto path =
                        fs::absolute(entry.path(), ec).lexically_normal();
                    if (!ec)
                        physical_files.push_back(std::move(path));
                }

                if (ec) {
                    scan_failed = true;
                    break;
                }
                scan_operation = "advance directory iterator";
                iterator.increment(ec);
                scan_failed = static_cast<bool>(ec);
            }

            if (scan_failed) {
                std::println(
                    "ImageIndex: Scan failed, preserving previous snapshot "
                    "for: {} ({} at {}: {} [{}:{}])",
                    sung::tostr(root),
                    scan_operation,
                    sung::tostr(scan_path),
                    ec.message(),
                    ec.category().name(),
                    ec.value()
                );
                continue;
            }

            // Recursive preservation drops the selected folder as well as its
            // descendants. Add it back only when the directory still exists.
            if (!folder.empty() && directory_opened && scan_root != root) {
                const auto path = fromstr(namespace_name) /
                                  scan_root.lexically_relative(root);
                root_folders.push_back(
                    {root_key,
                     tostr(scan_root.filename()),
                     tostr(path),
                     tostr(path.parent_path()),
                     get_image_sort_time(scan_root)}
                );
            }
            // A Sprintboard AVIF proxy is the browser-facing derivative
            // of its source. Keep the relationship explicit so date
            // sorting uses the source timestamp even when the encoder
            // could not copy all filesystem timestamps to the proxy.
            std::unordered_map<std::string, Path> sources_by_path;
            for (const auto& path : physical_files) {
                if (sung::is_sprintboard_proxy_path(path))
                    continue;
                sources_by_path.insert_or_assign(make_path_key(path), path);
            }

            auto& proxy_sources = result.proxy_sources_;
            std::unordered_set<std::string> paired_sources;
            std::unordered_set<std::string> stale_proxies;
            for (const auto& path : physical_files) {
                const auto source_path =
                    sung::sprintboard_proxy_source_path(path);
                if (!source_path)
                    continue;
                const auto source =
                    sources_by_path.find(make_path_key(*source_path));
                if (source == sources_by_path.end())
                    continue;

                std::error_code source_time_error;
                std::error_code proxy_time_error;
                const auto source_time =
                    fs::last_write_time(source->second, source_time_error);
                const auto proxy_time =
                    fs::last_write_time(path, proxy_time_error);
                if (source_time_error || proxy_time_error ||
                    source_time != proxy_time) {
                    stale_proxies.insert(make_path_key(path));
                    continue;
                }
                proxy_sources.insert_or_assign(
                    make_path_key(path), source->second
                );
                paired_sources.insert(make_path_key(source->second));
            }

            // Each worker probes an immutable metadata copy.
            // so concurrent lookups across files are safe; each file's
            // filesystem work (stat, and full decode for new/changed
            // files) can therefore overlap instead of running one at a
            // time, which matters a lot when the scan root is behind
            // something with high per-call latency (e.g. an encrypted
            // vault driver).
            auto& probes = result.probes_;
            probes.resize(physical_files.size());
            arena.execute([&] {
                tbb::parallel_for(
                    tbb::blocked_range<size_t>(0, physical_files.size()),
                    [&](const tbb::blocked_range<size_t>& range) {
                        for (auto i = range.begin(); i != range.end(); ++i) {
                            const auto path_str =
                                sung::tostr(physical_files[i]);
                            const auto it = input_metadata.find(path_str);
                            const CachedMetadata* existing =
                                it != input_metadata.end() ? &it->second
                                                           : nullptr;
                            const auto path_key =
                                make_path_key(physical_files[i]);
                            const Path* sort_time_source = nullptr;
                            if (sung::is_sprintboard_proxy_path(
                                    physical_files[i]
                                )) {
                                const auto source =
                                    proxy_sources.find(path_key);
                                if (source != proxy_sources.end())
                                    sort_time_source = &source->second;
                            }
                            probes[i] = probe_file(
                                physical_files[i],
                                paired_sources.contains(path_key) ||
                                    stale_proxies.contains(path_key),
                                sort_time_source,
                                existing
                            );
                        }
                    }
                );
            });

            result.entries_.resize(physical_files.size());
            for (size_t i = 0; i < physical_files.size(); ++i) {
                const auto& physical_path = physical_files[i];
                const auto& probe = probes[i];
                const auto& metadata = probe.metadata_;
                if (probe.shadowed_ || probe.stat_failed_ ||
                    !metadata.eligible_)
                    continue;
                const auto path_str = tostr(physical_path);
                const auto relative = physical_path.lexically_relative(root);
                if (relative.empty() || sung::tostr(relative).starts_with(".."))
                    continue;

                const auto namespace_path = sung::fromstr(namespace_name);
                const auto browser_path =
                    sung::tostr(namespace_path / relative);
                const auto api_path =
                    sung::tostr(Path{"/img"} / namespace_path / relative);

                IndexedFile entry;
                entry.root_key_ = root_key;
                entry.physical_path_ = path_str;
                entry.browser_path_ = browser_path;
                entry.parent_browser_path_ =
                    sung::tostr((namespace_path / relative).parent_path());
                const auto proxy_source =
                    proxy_sources.find(make_path_key(physical_path));
                const auto display_name =
                    proxy_source != proxy_sources.end()
                        ? sung::tostr(proxy_source->second.filename())
                        : sung::tostr(physical_path.filename());
                entry.info_.name_ = display_name;
                entry.info_.path_ = sung::fromstr(api_path);
                entry.info_.width_ = metadata.width_;
                entry.info_.height_ = metadata.height_;
                entry.info_.sort_time_ns_ = metadata.sort_time_ns_;
                entry.model_ = metadata.model_;
                entry.prompts_ = metadata.prompts_;
                entry.logical_path_ =
                    sung::detail::logical_image_key(physical_path);
                const auto tag_input = proxy_source != proxy_sources.end()
                                           ? proxy_source->second
                                           : physical_path;
                entry.tag_input_path_ = sung::tostr(tag_input);
                const auto tag_fingerprint = sung::fingerprint_file(tag_input);
                if (tag_fingerprint) {
                    entry.tag_input_size_ = tag_fingerprint->size_;
                    entry.tag_input_modified_time_ =
                        tag_fingerprint->modified_time_;
                } else {
                    entry.tag_input_size_ = 0;
                    entry.tag_input_modified_time_ = 0;
                }
                result.entries_[i] = std::move(entry);
            }
            result.available_ = true;
        }
        std::unordered_set<std::string> absent_tags;
        for (const auto& path : tag_paths) {
            const auto source = fromstr(path);
            const auto directory = source.parent_path();
            bool selected = false;
            for (const auto& [name, result] : results)
                selected |= result.available_ &&
                            within(directory, result.root_) &&
                            scope_contents(directory, result.root_);
            if (!selected)
                continue;
            std::error_code source_error, proxy_error;
            const bool source_exists = fs::exists(source, source_error);
            const bool proxy_exists =
                fs::exists(make_sprintboard_proxy_path(source), proxy_error);
            if (!source_error && !proxy_error && !source_exists &&
                !proxy_exists)
                absent_tags.insert(path);
        }
        if (scan_observer_)
            scan_observer_(
                folder.empty() ? (date.empty() ? "full" : "recent") : "visit"
            );
        transaction.lock();
        if (!transaction.current()) {
            stats.superseded_ = true;
            return stats;
        }
        flush_pending_removals_locked();
        const auto old_snapshot = load_snapshot();
        const auto initial_refresh = old_snapshot->generation_ == 0;
        auto next = std::make_shared<IndexSnapshot>();
        next->generation_ = old_snapshot->generation_ + 1;

        std::unordered_set<std::string> seen_physical;
        std::unordered_set<std::string> seen_api_paths;
        std::unordered_map<std::string, size_t> seen_folder_paths;
        std::vector<CachedMetadata> changed;
        bool all_roots_accessible = true;
        const auto contents_scanned = [&](const Path& path, const Path& root) {
            return scope_contents(path, root) && !transaction.superseded(path);
        };
        const auto folders_scanned = [&](const Path& path, const Path& root) {
            return scope_folders(path, root) &&
                   !transaction.superseded(path, true);
        };
        std::set<std::pair<std::string, std::string>> known_folders;
        for (const auto& item : old_snapshot->folders_)
            known_folders.emplace(item.root_key_, item.path_);

        const auto add_folder = [&](IndexedFolder folder) {
            const auto [it, inserted] = seen_folder_paths.try_emplace(
                folder.path_, next->folders_.size()
            );
            if (inserted) {
                next->folders_.push_back(std::move(folder));
                return;
            }

            auto& existing = next->folders_[it->second];
            existing.sort_time_ns_ = std::max(
                existing.sort_time_ns_, folder.sort_time_ns_
            );
        };

        const auto preserve_root = [&](const std::string& root_key,
                                       const bool only_unscanned = false) {
            const auto root = sung::fromstr(
                root_key.substr(root_key.find('\n') + 1)
            );
            const auto namespace_name = root_key.substr(0, root_key.find('\n'));
            if (const auto previous =
                    old_snapshot->namespace_sort_times_.find(namespace_name);
                previous != old_snapshot->namespace_sort_times_.end())
                next->namespace_sort_times_[namespace_name] = std::max(
                    next->namespace_sort_times_[namespace_name],
                    previous->second
                );
            const auto preserve_directory = [&](const Path& directory) {
                return !only_unscanned || !contents_scanned(directory, root);
            };
            // Paired PNG sources may be absent from the visible metadata map.
            for (const auto& [key, path] : proxy_queue_.sources()) {
                const auto relative = path.lexically_relative(root);
                if (!relative.empty() && *relative.begin() != ".." &&
                    preserve_directory(path.parent_path()))
                    seen_physical.insert(tostr(path));
            }
            for (const auto& [path, metadata] : metadata_) {
                const auto physical = sung::fromstr(path);
                const auto relative = physical.lexically_relative(root);
                if (!relative.empty() && *relative.begin() != ".." &&
                    preserve_directory(physical.parent_path()))
                    seen_physical.insert(path);
            }
            for (const auto& file : old_snapshot->files_) {
                if (file.root_key_ != root_key ||
                    !preserve_directory(
                        fromstr(file.physical_path_).parent_path()
                    ))
                    continue;
                if (seen_api_paths.insert(sung::tostr(file.info_.path_)).second)
                    next->files_.push_back(file);
                seen_physical.insert(file.physical_path_);
            }
            for (const auto& cached_folder : old_snapshot->folders_) {
                if (cached_folder.root_key_ != root_key)
                    continue;
                const auto relative =
                    fromstr(cached_folder.path_)
                        .lexically_relative(
                            fromstr(root_key.substr(0, root_key.find('\n')))
                        );
                if (only_unscanned && folders_scanned(root / relative, root))
                    continue;
                add_folder(cached_folder);
            }
        };

        for (auto& [namespace_name, result] : results) {
            next->namespaces_.insert(namespace_name);
            const auto& root = result.root_;
            const auto root_key = make_root_key(namespace_name, root);
            if (!result.available_) {
                all_roots_accessible &= !result.selected_;
                preserve_root(root_key);
                continue;
            }
            next->namespace_sort_times_[namespace_name] = result.sort_time_;
            auto& root_folders = result.folders_;
            std::erase_if(root_folders, [&](const auto& item) {
                const auto physical =
                    root / fromstr(item.path_)
                               .lexically_relative(fromstr(namespace_name));
                return !folders_scanned(physical, root);
            });
            const auto& physical_files = result.physical_files_;
            auto& probes = result.probes_;
            {
                std::lock_guard lock{ visits_mutex_ };
                for (const auto& item : root_folders) {
                    if (!known_folders.contains({ item.root_key_, item.path_ }))
                        unscanned_folders_.insert(item.path_);
                }
                std::erase_if(unscanned_folders_, [&](const auto& path) {
                    if (!is_descendant_or_child(namespace_name, path))
                        return false;
                    return contents_scanned(
                        root / fromstr(path).lexically_relative(
                                   fromstr(namespace_name)
                               ),
                        root
                    );
                });
            }

                preserve_root(root_key, true);
            for (const auto& item : root_folders) add_folder(item);

            for (const auto& path : physical_files) {
                if (!contents_scanned(path.parent_path(), root))
                    continue;
                const auto path_str = sung::tostr(path);
                seen_physical.insert(path_str);
                auto extension = tostr(path.extension());
                absl::AsciiStrToLower(&extension);
                if (extension == ".png")
                    proxy_queue_.discover(path);
            }

            for (size_t i = 0; i < physical_files.size(); ++i) {
                const auto& physical_path = physical_files[i];
                if (!contents_scanned(physical_path.parent_path(), root))
                    continue;
                auto& probe = probes[i];
                ++stats.files_scanned_;

                if (probe.shadowed_ || probe.stat_failed_)
                    continue;

                const auto path_str = sung::tostr(physical_path);
                if (probe.reused_) {
                    ++stats.metadata_reused_;
                    if (probe.needs_persist_) {
                        metadata_[path_str] = probe.metadata_;
                        changed.push_back(probe.metadata_);
                    }
                } else {
                    metadata_[path_str] = probe.metadata_;
                    changed.push_back(probe.metadata_);
                    ++stats.metadata_indexed_;
                }

                const auto& metadata = probe.metadata_;
                if (initial_refresh && stats.files_scanned_ % 1000 == 0) {
                    std::println(
                        "ImageIndex: Validated {} files ({} reused, {} "
                        "indexed)...",
                        stats.files_scanned_,
                        stats.metadata_reused_,
                        stats.metadata_indexed_
                    );
                }
                if (!metadata.eligible_)
                    continue;

                if (result.entries_[i] &&
                    seen_api_paths
                        .insert(tostr(result.entries_[i]->info_.path_))
                        .second)
                    next->files_.push_back(std::move(*result.entries_[i]));
            }
        }

        std::vector<std::string> removed;
        for (auto it = metadata_.begin(); it != metadata_.end();) {
            if (seen_physical.contains(it->first)) {
                ++it;
                continue;
            }
            removed.push_back(it->first);
            it = metadata_.erase(it);
        }
        stats.metadata_removed_ = removed.size();
        std::vector<Path> removed_sources;
        for (const auto& [key, path] : proxy_queue_.sources())
            if (!seen_physical.contains(tostr(path)))
                removed_sources.push_back(path);
        for (const auto& path : removed_sources) proxy_queue_.forget(path);

        if (all_roots_accessible) {
            std::unordered_set<std::string> visible_logical_paths;
            visible_logical_paths.reserve(next->files_.size());
            for (const auto& file : next->files_)
                visible_logical_paths.insert(file.logical_path_);
            for (auto it = tag_analyses_.begin(); it != tag_analyses_.end();) {
                if (!absent_tags.contains(it->first) ||
                    transaction.superseded(fromstr(it->first).parent_path())) {
                    ++it;
                    continue;
                }
                if (visible_logical_paths.contains(it->first)) {
                    ++it;
                    continue;
                }
                if (!erase_tag_analysis(it->first)) {
                    std::println(
                        "ImageIndex: Failed to remove stale tag analysis for "
                        "{}",
                        it->first
                    );
                    ++it;
                    continue;
                }
                it = tag_analyses_.erase(it);
            }
        }

        // Content already reconciled by another scan needs no priority rescan.
        std::erase_if(priority_folders_, [&](const auto& queued) {
            for (const auto& [name, result] : results) {
                const auto relative =
                    fromstr(queued).lexically_relative(fromstr(name));
                if (result.available_ && !relative.empty() &&
                    *relative.begin() != ".." &&
                    contents_scanned(result.root_ / relative, result.root_)) {
                    priority_pending_.erase(queued);
                    priority_retries_.erase(queued);
                    return true;
                }
            }
            return false;
        });

        const auto persistence =
            flush_stores(changed, removed, date.empty() && folder.empty());
        stats.persistent_ = persistent();

        next->tag_analyses_ = tag_analyses_;
        std::unordered_set<std::string> tag_inputs;
        for (auto& file : next->files_) {
            // Unavailable roots may still contain browsing-only restored
            // entries.
            if (file.logical_path_.empty())
                continue;
            if (file.tag_input_size_ > 0) {
                tag_inputs.insert(file.logical_path_);
                tag_queue_.observe(
                    { file.logical_path_,
                      sung::fromstr(file.tag_input_path_),
                      file.tag_input_size_,
                      file.tag_input_modified_time_ }
                );
            }
            const auto found = tag_analyses_.find(file.logical_path_);
            file.tags_ = found == tag_analyses_.end()
                             ? std::vector<std::string>{}
                             : found->second.searchable_tags_;
        }
        tag_queue_.retain(tag_inputs);
        std::sort(next->files_.begin(), next->files_.end(), file_before);
        std::sort(
            next->folders_.begin(),
            next->folders_.end(),
            [](const auto& a, const auto& b) {
                if (a.name_ != b.name_)
                    return a.name_ > b.name_;
                return a.path_ > b.path_;
            }
        );

        stats.images_available_ = next->files_.size();
        stats.folders_available_ = next->folders_.size();
        stats.elapsed_seconds_ = timer.elapsed();
        std::vector<Path> scanned_roots;
        for (const auto& [name, result] : results)
            if (result.available_)
                scanned_roots.push_back(result.root_);
        transaction.publish([scanned_roots = std::move(scanned_roots),
                             date,
                             folder,
                             recursive,
                             within](const Path& path, bool folders) {
            for (const auto& root : scanned_roots) {
                if (!within(path, root))
                    continue;
                if (!folder.empty())
                    return recursive
                               ? within(path, folder)
                               : make_path_key(path) == make_path_key(folder) ||
                                     (folders &&
                                      make_path_key(path.parent_path()) ==
                                          make_path_key(folder));
                const auto scope = date_scope(path, root, date);
                if (date.empty() || (folders ? scope != DateScope::excluded
                                             : scope == DateScope::selected))
                    return true;
            }
            return false;
        });
        store_snapshot(std::move(next));
        transaction.unlock();
        if (synchronous)
            wait_storage(persistence);
        transaction.lock();
        stats.persistent_ = persistent();
        const auto snapshot_ticket = snapshot_ticket_;
        transaction.unlock();
        if (synchronous)
            snapshot_writer_->wait(snapshot_ticket);

        std::println(
            "ImageIndex ({}): {} images, {} folders ({} reused, {} indexed, "
            "{} removed) in {:.3f} seconds",
            !folder.empty() ? tostr(folder)
            : date.empty()
                ? "full"
                : std::format("{} through {}", date.first_, date.last_),
            stats.images_available_,
            stats.folders_available_,
            stats.metadata_reused_,
            stats.metadata_indexed_,
            stats.metadata_removed_,
            stats.elapsed_seconds_
        );
        return stats;
    }

    void refresh_tags(const std::shared_ptr<const ServerConfigs>& configs) {
        using Clock = detail::TagQueue::Clock;
        if (!configs->tagger_enabled_) {
            tagger_info_.reset();
            tagger_info_checked_at_.reset();
            return;
        }

        const auto interval = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>{
                std::max(configs->tagger_poll_interval_seconds_, 1.0) }
        );
        const auto endpoint = std::make_pair(
            configs->tagger_host_, configs->tagger_port_
        );
        const TaggerClient client{ endpoint.first, endpoint.second };
        if (!tagger_info_checked_at_ || endpoint != tagger_endpoint_ ||
            Clock::now() - *tagger_info_checked_at_ >= interval) {
            const auto fetched = client.get_info();
            tagger_info_checked_at_ = Clock::now();
            tagger_endpoint_ = endpoint;
            if (!fetched) {
                tagger_info_.reset();
                std::println(
                    "ImageTagger: Service unavailable at {}:{}: {}",
                    endpoint.first,
                    endpoint.second,
                    fetched.error()
                );
                return;
            }
            tagger_info_ = *fetched;
            std::lock_guard refresh_lock{ refresh_mutex_ };
            if (current_analyzer_fingerprint_ != fetched->fingerprint_) {
                current_analyzer_fingerprint_ = fetched->fingerprint_;
                tag_queue_.invalidate();
            }
        }
        if (!tagger_info_)
            return;
        const auto& info = tagger_info_;
        using Candidate = detail::TagInput;
        const auto batch_size = static_cast<size_t>(
            std::max(configs->tagger_batch_size_, 1)
        );
        const auto retry_delay = [](int failures) {
            const auto shift = std::min(std::max(failures - 1, 0), 7);
            return std::min<int64_t>(30LL << shift, 3600);
        };

        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch()
        )
                             .count();
        std::vector<Candidate> candidates;
        for (size_t checked = 0;
             checked < 128 && candidates.size() < batch_size;
             ++checked) {
            std::unique_lock lock{refresh_mutex_};
            const auto queued = tag_queue_.take();
            if (!queued)
                break;
            const auto file = *queued;
            if (!tag_ready(file.logical_path_)) {
                tag_queue_.retry(file, interval);
                continue;
            }
            const auto found = tag_analyses_.find(file.logical_path_);
            const auto existing = found == tag_analyses_.end()
                                      ? std::optional<CachedTagAnalysis>{}
                                      : found->second;
            const auto version = tag_versions_[file.logical_path_];
            lock.unlock();
            bool source_missing = false;
            if (existing && !existing->analysis_.is_null() &&
                existing->input_path_ != tostr(file.input_path_) &&
                is_sprintboard_proxy_path(file.input_path_)) {
                std::error_code error;
                source_missing =
                    !fs::is_regular_file(fromstr(existing->input_path_), error);
            }
            const auto kind = is_sprintboard_proxy_path(file.input_path_)
                                  ? "proxy"
                                  : "source";
            const bool matches =
                existing && !existing->analysis_.is_null() &&
                existing->input_kind_ == kind &&
                existing->input_path_ == tostr(file.input_path_) &&
                existing->analyzer_fingerprint_ == info->fingerprint_;
            const auto validated = matches ? ::validate_fingerprint(
                                                 file.input_path_,
                                                 existing->input_size_,
                                                 existing->input_modified_time_,
                                                 existing->input_sha256_
                                             )
                                           : std::nullopt;
            lock.lock();
            flush_pending_removals_locked();
            if (tag_versions_[file.logical_path_] != version ||
                !tag_queue_.current(file) || !tag_ready(file.logical_path_)) {
                tag_queue_.retry(file, interval);
                continue;
            }
            if (source_missing)
                continue;
            if (validated) {
                if (validated->modified_time_ !=
                    existing->input_modified_time_) {
                    auto updated = *existing;
                    updated.input_size_ = validated->size_;
                    updated.input_modified_time_ = validated->modified_time_;
                    persist_tag_analysis(updated);
                }
                continue;
            }
            if (existing) {
                const auto& attempt = *existing;
                const bool same =
                    !attempt.last_error_.empty() &&
                    attempt.attempt_input_path_ == tostr(file.input_path_) &&
                    attempt.attempt_input_size_ == file.input_size_ &&
                    attempt.attempt_input_modified_time_ ==
                        file.input_modified_time_ &&
                    attempt.attempt_analyzer_fingerprint_ == info->fingerprint_;
                const auto remaining = retry_delay(attempt.failure_count_) -
                                       (now - attempt.last_attempt_at_);
                if (same && remaining > 0) {
                    tag_queue_.retry(file, std::chrono::seconds{remaining});
                    continue;
                }
            }
            candidates.push_back(file);
        }

        for (size_t offset = 0; offset < candidates.size();
             offset += batch_size) {
            const auto end = std::min(offset + batch_size, candidates.size());
            std::vector<Candidate> batch;
            std::vector<uint64_t> batch_versions;
            std::vector<Path> paths;
            {
                std::lock_guard refresh_lock{ refresh_mutex_ };
                for (size_t i = offset; i < end; ++i)
                    if (tag_ready(candidates[i].logical_path_)) {
                        batch.push_back(candidates[i]);
                        batch_versions.push_back(
                            tag_versions_[candidates[i].logical_path_]
                        );
                        paths.push_back(candidates[i].input_path_);
                    } else {
                        tag_queue_.retry(candidates[i], interval);
                    }
            }
            const auto count = batch.size();
            if (count == 0)
                continue;

            const auto results = client.analyze(paths, info->fingerprint_);
            if (!results) {
                std::println(
                    "ImageTagger: Batch request failed: {}", results.error()
                );
                std::lock_guard refresh_lock{ refresh_mutex_ };
                for (const auto& candidate : batch)
                    tag_queue_.retry(candidate, interval);
                // Pause the backlog until the next service-info check too.
                tagger_info_.reset();
                tagger_info_checked_at_ = Clock::now();
                return;
            }

            for (size_t i = 0; i < count; ++i) {
                const auto& candidate = batch[i];
                const auto& result = results->at(i);
                const auto current = sung::fingerprint_file(
                    candidate.input_path_
                );
                if (!current || current->size_ != candidate.input_size_ ||
                    current->modified_time_ != candidate.input_modified_time_) {
                    continue;
                }

                std::optional<sung::FileFingerprint> content_fingerprint;
                if (result.error_.empty()) {
                    const auto hashed = sung::fingerprint_file_with_sha256(
                        candidate.input_path_
                    );
                    if (!hashed || hashed->size_ != candidate.input_size_ ||
                        hashed->modified_time_ !=
                            candidate.input_modified_time_) {
                        std::println(
                            "ImageTagger: Input changed while fingerprinting: "
                            "{}",
                            sung::tostr(candidate.input_path_)
                        );
                        continue;
                    }
                    content_fingerprint = *hashed;
                }

                std::lock_guard lock{refresh_mutex_};
                flush_pending_removals_locked();
                if (!tag_queue_.current(candidate) ||
                    tag_versions_[candidate.logical_path_] != batch_versions[i])
                    continue;
                const auto previous = tag_analyses_.find(
                    candidate.logical_path_
                );
                auto analysis = previous == tag_analyses_.end()
                                    ? CachedTagAnalysis{}
                                    : previous->second;
                analysis.logical_path_ = candidate.logical_path_;
                const bool repeated_failure =
                    analysis.attempt_input_path_ ==
                        sung::tostr(candidate.input_path_) &&
                    analysis.attempt_input_size_ == candidate.input_size_ &&
                    analysis.attempt_input_modified_time_ ==
                        candidate.input_modified_time_ &&
                    analysis.attempt_analyzer_fingerprint_ ==
                        info->fingerprint_;
                analysis.attempt_input_path_ = sung::tostr(
                    candidate.input_path_
                );
                analysis.attempt_input_size_ = candidate.input_size_;
                analysis.attempt_input_modified_time_ =
                    candidate.input_modified_time_;
                analysis.attempt_analyzer_fingerprint_ = info->fingerprint_;
                analysis.last_attempt_at_ = now;

                if (!result.error_.empty()) {
                    analysis.failure_count_ = repeated_failure
                                                  ? analysis.failure_count_ + 1
                                                  : 1;
                    analysis.last_error_ = result.error_;
                    std::println(
                        "ImageTagger: Analysis failed for {}: {}",
                        sung::tostr(candidate.input_path_),
                        result.error_
                    );
                } else {
                    analysis.input_kind_ = sung::is_sprintboard_proxy_path(
                                               candidate.input_path_
                                           )
                                               ? "proxy"
                                               : "source";
                    analysis.input_path_ = sung::tostr(candidate.input_path_);
                    analysis.input_size_ = content_fingerprint->size_;
                    analysis.input_modified_time_ =
                        content_fingerprint->modified_time_;
                    analysis.input_sha256_ = content_fingerprint->sha256_;
                    analysis.analyzer_fingerprint_ = info->fingerprint_;
                    analysis.model_id_ = info->model_id_;
                    analysis.general_threshold_ = info->general_threshold_;
                    analysis.character_threshold_ = info->character_threshold_;
                    analysis.analysis_ = result.analysis_;
                    analysis.searchable_tags_ = result.searchable_tags_;
                    analysis.analyzed_at_ = now;
                    analysis.analysis_id_ = sung::make_analysis_id(analysis);
                    analysis.proxy_path_.clear();
                    analysis.proxy_size_ = 0;
                    analysis.proxy_modified_time_ = 0;
                    analysis.proxy_sha256_.clear();
                    analysis.proxy_materialization_id_.clear();
                    analysis.failure_count_ = 0;
                    analysis.last_error_.clear();
                }

                if (!result.error_.empty()) {
                    tag_queue_.retry(
                        candidate,
                        std::chrono::seconds{
                            retry_delay(analysis.failure_count_) }
                    );
                }
                if (!persist_tag_analysis(analysis)) {
                    std::println(
                        "ImageTagger: Failed to persist analysis state for {}",
                        candidate.logical_path_
                    );
                } else if (result.error_.empty()) {
                    std::println(
                        "ImageTagger: Queued {} tags for storage: {}",
                        analysis.searchable_tags_.size(),
                        candidate.logical_path_
                    );
                }
            }
        }
    }

    std::optional<nlohmann::json> tag_analysis(
        const Path& physical_path
    ) const {
        const auto current = load_snapshot();
        const auto logical_path = sung::detail::logical_image_key(
            physical_path
        );
        const auto found = current->tag_analyses_.find(logical_path);
        if (found == current->tag_analyses_.end() ||
            found->second.analysis_.is_null()) {
            return std::nullopt;
        }

        auto output = found->second.analysis_;
        output.erase("path");
        output["analyzerFingerprint"] = found->second.analyzer_fingerprint_;
        output["modelId"] = found->second.model_id_;
        output["generalThreshold"] = found->second.general_threshold_;
        output["characterThreshold"] = found->second.character_threshold_;
        output["analyzedAt"] = found->second.analyzed_at_;
        output["analysisId"] = found->second.analysis_id_;
        bool source_missing = false;
        if (const auto source =
                sung::sprintboard_proxy_source_path(physical_path)) {
            std::error_code error;
            source_missing = !sung::fs::is_regular_file(*source, error) ||
                             error;
        }
        output["sourceMissing"] = source_missing;
        return output;
    }

    std::optional<TagAnalysisRecord> current_tag_analysis(
        const Path& source_path, const bool require_current_analyzer
    ) {
        const auto logical_path = detail::logical_image_key(source_path);
        std::unique_lock lock{refresh_mutex_};
        if (require_current_analyzer && !tag_ready(logical_path))
            return std::nullopt;
        const auto found = tag_analyses_.find(logical_path);
        if (found == tag_analyses_.end() || found->second.analysis_.is_null())
            return std::nullopt;
        auto output = found->second;
        const auto version = tag_versions_[logical_path];
        lock.unlock();
        if (scan_observer_)
            scan_observer_("tag-validation");
        const auto kind =
            is_sprintboard_proxy_path(source_path) ? "proxy" : "source";
        if (output.input_kind_ != kind ||
            make_path_key(canonical_path(fromstr(output.input_path_))) !=
                make_path_key(canonical_path(source_path)))
            return std::nullopt;
        const auto fingerprint = ::validate_fingerprint(
            source_path,
            output.input_size_,
            output.input_modified_time_,
            output.input_sha256_
        );
        if (!fingerprint)
            return std::nullopt;
        lock.lock();
        flush_pending_removals_locked();
        if (tag_versions_[logical_path] != version ||
            !tag_analyses_.contains(logical_path) ||
            (require_current_analyzer &&
             (!tag_ready(logical_path) ||
              (!current_analyzer_fingerprint_.empty() &&
               output.analyzer_fingerprint_ != current_analyzer_fingerprint_))))
            return std::nullopt;
        if (fingerprint->modified_time_ != output.input_modified_time_) {
            output.input_size_ = fingerprint->size_;
            output.input_modified_time_ = fingerprint->modified_time_;
            if (!pending_tags_.contains(logical_path)) {
                persist_tag_analysis(output);
                const auto sequence = storage_sequence_;
                lock.unlock();
                wait_storage(sequence);
                lock.lock();
                flush_pending_removals_locked();
                if (!tag_analyses_.contains(logical_path) ||
                    tag_versions_[logical_path] != sequence)
                    return std::nullopt;
            }
        }
        return output;
    }

    bool proxy_materialization_current(
        const Path& proxy_path, const std::string_view materialization_id
    ) {
        const auto logical_path = detail::logical_image_key(proxy_path);
        std::unique_lock lock{refresh_mutex_};
        const auto found = tag_analyses_.find(logical_path);
        if (found == tag_analyses_.end() ||
            found->second.proxy_materialization_id_ != materialization_id)
            return false;
        auto output = found->second;
        const auto version = tag_versions_[logical_path];
        lock.unlock();
        if (scan_observer_)
            scan_observer_("proxy-validation");
        if (make_path_key(canonical_path(fromstr(output.proxy_path_))) !=
            make_path_key(canonical_path(proxy_path)))
            return false;
        const auto fingerprint = ::validate_fingerprint(
            proxy_path,
            output.proxy_size_,
            output.proxy_modified_time_,
            output.proxy_sha256_
        );
        if (!fingerprint)
            return false;
        lock.lock();
        flush_pending_removals_locked();
        if (tag_versions_[logical_path] != version ||
            !tag_analyses_.contains(logical_path))
            return false;
        if (fingerprint->modified_time_ != output.proxy_modified_time_ &&
            !pending_tags_.contains(logical_path)) {
            output.proxy_modified_time_ = fingerprint->modified_time_;
            persist_tag_analysis(output);
            const auto sequence = storage_sequence_;
            lock.unlock();
            wait_storage(sequence);
            lock.lock();
            flush_pending_removals_locked();
            if (!tag_analyses_.contains(logical_path) ||
                tag_versions_[logical_path] != sequence)
                return false;
        }
        return true;
    }

    void mark_proxy_materialized(
        const Path& source_path,
        const Path& proxy_path,
        std::string materialization_id
    ) {
        const auto logical_path = detail::logical_image_key(source_path);
        std::unique_lock lock{refresh_mutex_};
        const auto found = tag_analyses_.find(logical_path);
        if (found == tag_analyses_.end() || found->second.analysis_.is_null())
            return;
        auto analysis = found->second;
        const auto version = tag_versions_[logical_path];
        if (const auto pending = pending_tags_.find(logical_path);
            pending != pending_tags_.end()) {
            if (!pending->second.value_ ||
                pending->second.value_->analysis_id_ != analysis.analysis_id_)
                return;
            analysis = *pending->second.value_;
        }
        lock.unlock();
        if (scan_observer_)
            scan_observer_("proxy-hash");
        const auto fingerprint = fingerprint_file_with_sha256(proxy_path);
        if (!fingerprint)
            return;
        analysis.proxy_path_ = tostr(canonical_path(proxy_path));
        analysis.proxy_size_ = fingerprint->size_;
        analysis.proxy_modified_time_ = fingerprint->modified_time_;
        analysis.proxy_sha256_ = fingerprint->sha256_;
        analysis.proxy_materialization_id_ = std::move(materialization_id);
        lock.lock();
        flush_pending_removals_locked();
        if (tag_versions_[logical_path] != version ||
            !tag_analyses_.contains(logical_path))
            return;
        if (persist_tag_analysis(analysis)) {
            const auto sequence = storage_sequence_;
            lock.unlock();
            wait_storage(sequence);
        }
    }

    std::vector<ProxyWorkItem> take_proxy_work(
        const ServerConfigs& configs, size_t limit
    ) {
        const auto roots = resolved_roots(configs);
        std::lock_guard lock{ refresh_mutex_ };
        auto signature = nlohmann::json::array({ configs.tagger_enabled_ });
        for (const auto& [name, binding] : configs.dir_bindings_) {
            const auto options = configs.effective_avif_options(binding);
            signature.push_back(
                { name,
                  tostr(binding.local_dir_),
                  options.gen_,
                  static_cast<int>(options.pix_format_),
                  options.quality_,
                  options.speed_ }
            );
        }
        const auto options_for =
            [](const Path& path,
               const auto& roots) -> std::optional<ServerConfigs::AvifOptions> {
            const auto normalized = fromstr(detail::ProxyQueue::key(path));
            for (const auto& [root, options] : roots) {
                const auto relative = normalized.lexically_relative(root);
                if (!relative.empty() && *relative.begin() != "..")
                    return options;
            }
            return std::nullopt;
        };
        if (signature != proxy_config_signature_) {
            auto previous_roots = std::move(proxy_roots_);
            proxy_roots_.clear();
            for (const auto& [name, binding] : configs.dir_bindings_) {
                proxy_roots_.emplace_back(
                    fromstr(detail::ProxyQueue::key(roots.at(name))),
                    configs.effective_avif_options(binding)
                );
            }
            std::stable_sort(
                proxy_roots_.begin(),
                proxy_roots_.end(),
                [](const auto& a, const auto& b) {
                    return tostr(a.first).size() > tostr(b.first).size();
                }
            );
            proxy_queue_.clear_pending();
            for (const auto& [key, source] : proxy_queue_.sources()) {
                const auto next = options_for(source, proxy_roots_);
                if (!next || !next->gen_)
                    continue;
                const auto previous = options_for(source, previous_roots);
                const bool force = previous &&
                                   (previous->pix_format_ !=
                                        next->pix_format_ ||
                                    previous->quality_ != next->quality_ ||
                                    previous->speed_ != next->speed_);
                proxy_queue_.enqueue(source, force);
            }
            proxy_config_signature_ = std::move(signature);
        }
        std::vector<ProxyWorkItem> result;
        while (result.size() < limit) {
            auto item = proxy_queue_.take();
            if (!item)
                break;
            const auto options = options_for(item->path_, proxy_roots_);
            if (!options || !options->gen_)
                continue;
            item->options_ = *options;
            result.push_back(std::move(*item));
        }
        return result;
    }

    void retry_proxy_work(ProxyWorkItem item) {
        std::lock_guard lock{ refresh_mutex_ };
        proxy_queue_.retry(std::move(item));
    }

    ImageListResponse query(
        const Path& dir_path,
        const std::string& query_text,
        const bool recursive,
        const ImageSortOrder sort_order,
        const bool avif_only
    ) const {
        const auto current = load_snapshot();
        ImageListResponse response;
        const auto dir = sung::tostr(dir_path.lexically_normal());

        if (dir.empty() || dir == ".") {
            for (const auto& namespace_name : current->namespaces_) {
                const auto sort_time = current->namespace_sort_times_.find(
                    namespace_name
                );
                response.add_dir(
                    namespace_name,
                    sung::fromstr(namespace_name),
                    sort_time == current->namespace_sort_times_.end()
                        ? 0
                        : sort_time->second
                );
            }
            response.sort(sort_order);
            return response;
        }

        const sung::detail::ImageQuery query{ query_text };
        for (const auto& file : current->files_) {
            if (avif_only) {
                auto ext = file.info_.path_.extension().string();
                absl::AsciiStrToLower(&ext);
                if (ext != ".avif")
                    continue;
            }
            const auto in_directory = recursive
                                          ? is_descendant_or_child(
                                                dir, file.parent_browser_path_
                                            )
                                          : file.parent_browser_path_ == dir;
            if (!in_directory)
                continue;
            if (!query.matches_dimensions(
                    file.info_.width_, file.info_.height_
                )) {
                continue;
            }
            if (query.needs_metadata() &&
                !query.matches_metadata(
                    file.model_, file.prompts_, file.tags_
                )) {
                continue;
            }
            response.add_file(
                file.info_.name_,
                file.info_.path_,
                file.info_.width_,
                file.info_.height_,
                file.info_.sort_time_ns_
            );
        }

        for (const auto& folder : current->folders_) {
            if (folder.parent_path_ == dir)
                response.add_dir(
                    folder.name_,
                    sung::fromstr(folder.path_),
                    folder.sort_time_ns_
                );
        }
        response.sort(sort_order);
        return response;
    }

    void remove_api_path(const std::string_view api_path) {
        snapshots_.remove_api_path(api_path);
    }

    void flush_pending_removals() {
        std::lock_guard refresh_lock{ refresh_mutex_ };
        flush_pending_removals_locked();
    }

    void flush_pending_removals_locked() {
        const auto pending = snapshots_.take_removals();
        if (pending.empty())
            return;
        std::set<std::string> deleted_directories;
        for (const auto& [api_path, file] : pending) {
            if (!file.physical_path_.empty()) {
                deleted_directories.insert(
                    make_path_key(fromstr(file.physical_path_).parent_path())
                );
            } else if (api_path.starts_with("/img/")) {
                // Cold-start deletions may precede the first indexed entry.
                const auto browser = fromstr(api_path.substr(5));
                for (const auto& [name, root_key] : binding_roots_) {
                    const auto relative =
                        browser.lexically_relative(fromstr(name));
                    if (!relative.empty() && *relative.begin() != "..")
                        deleted_directories.insert(
                            make_path_key((root_key / relative).parent_path())
                        );
                }
            }
        }
        scan_journal_.invalidate([deleted_directories = std::move(
                                      deleted_directories
                                  )](const Path& path, bool folders) {
            return !folders &&
                   deleted_directories.contains(make_path_key(path));
        });
        std::vector<std::string> removed_physical_paths;
        for (const auto& [api_path, file] : pending) {
            if (file.physical_path_.empty())
                continue;
            removed_physical_paths.push_back(file.physical_path_);
            metadata_.erase(file.physical_path_);
            const auto logical_path =
                file.logical_path_.empty()
                    ? make_path_key(sprintboard_proxy_source_path(
                                        fromstr(file.physical_path_)
                      )
                                        .value_or(fromstr(file.physical_path_)))
                    : file.logical_path_;
            proxy_queue_.forget(fromstr(logical_path));
            tag_queue_.forget(logical_path);
            tag_analyses_.erase(logical_path);
            if (!erase_tag_analysis(logical_path)) {
                std::println(
                    "ImageIndex: Failed to remove tag analysis for {}",
                    logical_path
                );
            }
        }
        // Ownership reconciliation touches every persisted tag's filesystem
        // path. It belongs to full scans, not individual deletions.
        flush_stores({}, removed_physical_paths, false);
        schedule_snapshot();
    }

    bool persistent() const {
        return !root_status_.empty() && std::all_of(
                                            root_status_.begin(),
                                            root_status_.end(),
                                            [](const auto& item) {
                                                return item.second.healthy_ &&
                                                       !item.second.blocked_;
                                            }
                                        );
    }

private:
    std::shared_ptr<const IndexSnapshot> load_snapshot() const {
        return snapshots_.load();
    }

    // Caller holds refresh_mutex_; submit only captures immutable pointers.
    void schedule_snapshot() {
        if (snapshot_configs_)
            snapshot_ticket_ =
                snapshot_writer_->submit(snapshot_configs_, load_snapshot());
    }

    void store_snapshot(std::shared_ptr<IndexSnapshot> snapshot) {
        snapshots_.store(std::move(snapshot));
        schedule_snapshot();
    }

    std::map<std::string, RootStatus> root_status_;
    std::map<std::string, Path> binding_roots_;
    std::unordered_map<std::string, CachedMetadata> metadata_;
    std::unordered_map<std::string, CachedTagAnalysis> tag_analyses_;
    std::unordered_map<std::string, uint64_t> metadata_versions_, tag_versions_;
    std::unordered_map<std::string, TagWrite> pending_tags_;
    std::string storage_signature_;
    uint64_t storage_sequence_ = 0, hydration_sequence_ = 0, storage_epoch_ = 0;
    std::mutex storage_mutex_;
    std::condition_variable storage_cv_;
    std::deque<StorageBatch> storage_batches_;
    uint64_t storage_completed_ = 0;
    bool storage_stopping_ = false;
    std::thread storage_thread_;
    std::unique_ptr<gallery_cache::SnapshotWriter> snapshot_writer_;
    uint64_t snapshot_ticket_ = 0;
    std::string current_analyzer_fingerprint_;
    detail::TagQueue tag_queue_;
    // Accessed only by the tagging worker; queue/analyses use refresh_mutex_.
    std::optional<TaggerInfo> tagger_info_;
    std::optional<detail::TagQueue::Clock::time_point> tagger_info_checked_at_;
    std::pair<std::string, int> tagger_endpoint_;
    detail::ProxyQueue proxy_queue_;
    nlohmann::json proxy_config_signature_;
    std::vector<std::pair<Path, ServerConfigs::AvifOptions>> proxy_roots_;

    static std::string scan_bindings(const ServerConfigs& configs) {
        return nlohmann::json{
            gallery_cache::bindings(configs), configs.cache_dir_
        }
            .dump();
    }

    detail::ScanJournal scan_journal_;
    std::function<void(std::string_view)> scan_observer_;
    std::atomic_bool cache_loading_{false};
    gallery_cache::SnapshotStore snapshots_;
    std::shared_ptr<const ServerConfigs> snapshot_configs_;
    mutable std::mutex refresh_mutex_;
    mutable std::mutex visits_mutex_;
    std::string priority_bindings_;
    uint64_t priority_epoch_ = 0;
    std::unordered_map<
        std::string,
        std::pair<std::chrono::steady_clock::time_point, int>>
        priority_retries_;
    std::deque<std::string> priority_folders_;
    std::set<std::string> priority_pending_;
    std::set<std::string> unscanned_folders_;
    std::deque<std::string> visits_;
    std::unordered_map<std::string, bool> pending_visits_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        visited_at_;
};

namespace sung {

    nlohmann::json ImageIndexRefreshStats::make_json() const {
        return {
            { "filesScanned", files_scanned_ },
            { "metadataReused", metadata_reused_ },
            { "metadataIndexed", metadata_indexed_ },
            { "metadataRemoved", metadata_removed_ },
            { "imagesAvailable", images_available_ },
            { "foldersAvailable", folders_available_ },
            { "elapsedSeconds", elapsed_seconds_ },
            { "persistent", persistent_ },
        };
    }

    ImageIndex::ImageIndex()
        : ImageIndex(std::function<void(std::string_view)>{}) {
    }

    ImageIndex::ImageIndex(std::function<void(std::string_view)> observer)
        : impl_(std::make_unique<Impl>()) {
        impl_->set_scan_observer(std::move(observer));
    }

    ImageIndex::~ImageIndex() {
        auto_refresh_stop_ = true;
        auto_tagging_stop_ = true;
        for (auto* worker :
             {&auto_refresh_thread_,
              &auto_recent_thread_,
              &auto_discovery_thread_,
              &auto_visit_thread_,
              &auto_folder_poll_thread_,
              &auto_new_folder_thread_})
            if (worker->joinable())
                worker->join();
        if (auto_tagging_thread_.joinable())
            auto_tagging_thread_.join();
        try {
            impl_->flush_pending_removals();
        } catch (const std::exception& error) {
            std::println(
                stderr,
                "ImageIndex: Cannot flush deletions at shutdown: {}",
                error.what()
            );
        }
    }

    ImageIndexRefreshStats ImageIndex::initialize(
        std::shared_ptr<const ServerConfigs> configs
    ) {
        return impl_->initialize(configs);
    }

    void ImageIndex::set_initial_refresh_state(
        const InitialRefreshState state
    ) {
        auto previous = initial_refresh_state_.load();
        while (previous != InitialRefreshState::ready) {
            // Keep the published fast-scan phase visible during the full scan.
            if (previous == InitialRefreshState::recent_ready &&
                state == InitialRefreshState::running)
                return;
            if (initial_refresh_state_.compare_exchange_weak(previous, state))
                return;
        }
    }

    bool ImageIndex::ready_for_background_work() const {
        return initial_refresh_state_.load() == InitialRefreshState::ready;
    }

    std::string_view ImageIndex::initial_refresh_state() const {
        if (!ready_for_background_work() && impl_->loading_cache())
            return "loading_cache";
        switch (initial_refresh_state_.load()) {
            case InitialRefreshState::pending:
                return "pending";
            case InitialRefreshState::running:
                return "running";
            case InitialRefreshState::recent_ready:
                return "recent_ready";
            case InitialRefreshState::ready:
                return "ready";
            case InitialRefreshState::failed:
                return "failed";
        }
        return "failed";
    }

    ImageIndexRefreshStats ImageIndex::refresh(
        std::shared_ptr<const ServerConfigs> configs
    ) {
        set_initial_refresh_state(InitialRefreshState::running);
        try {
            const auto stats = impl_->refresh(configs);
            if (!stats.superseded_)
            set_initial_refresh_state(InitialRefreshState::ready);
            return stats;
        } catch (...) {
            set_initial_refresh_state(InitialRefreshState::failed);
            throw;
        }
    }

    ImageIndexRefreshStats ImageIndex::refresh_date(
        std::shared_ptr<const ServerConfigs> configs,
        const std::string_view date
    ) {
        return impl_->refresh(configs, date_range(date, 1));
    }

    ImageIndexRefreshStats ImageIndex::refresh_recent(
        std::shared_ptr<const ServerConfigs> configs,
        const std::string_view date
    ) {
        return impl_->refresh(configs, date_range(date, 5));
    }

    void ImageIndex::discover_folders(
        std::shared_ptr<const ServerConfigs> configs
    ) {
        impl_->discover_folders(configs);
    }

    void ImageIndex::poll_folders(
        std::shared_ptr<const ServerConfigs> configs
    ) {
        impl_->poll_folders(configs);
    }

    void ImageIndex::refresh_new_folder(
        std::shared_ptr<const ServerConfigs> configs
    ) {
        impl_->refresh_new_folder(configs);
    }

    ImageIndexRefreshStats ImageIndex::refresh_folder(
        std::shared_ptr<const ServerConfigs> configs,
        const Path& directory,
        bool recursive
    ) {
        return impl_->refresh_folder(configs, directory, recursive);
    }

    void ImageIndex::visit_page(
        const Path& directory, const nlohmann::json& page, bool recursive
    ) {
        impl_->visit_page(directory, page, recursive);
    }

    size_t ImageIndex::unscanned_folders(const Path& directory) const {
        return impl_->unscanned_folders(directory);
    }

    uint64_t ImageIndex::generation() const { return impl_->generation(); }

    std::vector<ProxyWorkItem> ImageIndex::take_proxy_work(
        const ServerConfigs& configs, size_t limit
    ) {
        return impl_->take_proxy_work(configs, limit);
    }

    void ImageIndex::retry_proxy_work(ProxyWorkItem item) {
        impl_->retry_proxy_work(std::move(item));
    }

    void ImageIndex::start_auto_refresh(
        std::function<std::shared_ptr<const ServerConfigs>()> configs_provider
    ) {
        using Clock = std::chrono::steady_clock;
        // Each worker owns its cadence and retries. Filesystem observations
        // overlap; the index serializes only shared-state
        // publication/storage.
        const auto launch = [this](const char* name, auto work) {
            return std::thread([this, name, work = std::move(work)]() mutable {
                while (!auto_refresh_stop_) {
                    bool failed = false;
                    try {
                        if (work())
                            return;
                    } catch (const std::exception& error) {
                        failed = true;
                        std::println(
                            "ImageIndex: {} scan failed; will retry: {}",
                            name,
                            error.what()
                        );
                    } catch (...) {
                        failed = true;
                        std::println(
                            "ImageIndex: {} scan failed; will retry.", name
                        );
                    }
                    for (double waited = 0;
                         waited < (failed ? 1.0 : 0.1) && !auto_refresh_stop_;
                         waited += 0.1)
                        sung::sleep_naive(0.1);
                }
            });
        };
        auto_refresh_thread_ = launch("full", [this, configs_provider] {
            set_initial_refresh_state(InitialRefreshState::running);
            try {
                const auto stats =
                    impl_->refresh(configs_provider(), {}, {}, false, false);
                if (!stats.superseded_)
                    set_initial_refresh_state(InitialRefreshState::ready);
                return !stats.superseded_;
            } catch (...) {
                set_initial_refresh_state(InitialRefreshState::failed);
                throw;
            }
        });
        auto_recent_thread_ = launch(
            "recent",
            [this,
             configs_provider,
             last = std::optional<Clock::time_point>{}]() mutable {
                const auto configs = configs_provider();
                const auto interval =
                    configs->image_index_date_refresh_interval_seconds_;
                if (!last ||
                    (interval > 0 &&
                     std::chrono::duration<double>(Clock::now() - *last)
                             .count() >= interval)) {
                    const auto stats = impl_->refresh(
                        configs,
                        date_range(local_date_folder(), 5),
                        {},
                        false,
                        false
                    );
                    if (!stats.superseded_) {
                        if (!last)
                            set_initial_refresh_state(
                                InitialRefreshState::recent_ready
                            );
                        last = Clock::now();
                    }
                }
                return false;
            }
        );
        auto_discovery_thread_ = launch(
            "discovery",
            [this,
             configs_provider,
             last = std::optional<Clock::time_point>{}]() mutable {
                const auto configs = configs_provider();
                if (!last ||
                    std::chrono::duration<double>(Clock::now() - *last)
                            .count() >=
                        configs
                            ->image_index_folder_discovery_interval_seconds_) {
                    impl_->discover_folders(configs, false);
                    last = Clock::now();
                }
                return false;
            }
        );
        auto_folder_poll_thread_ = launch(
            "folder-poll",
            [this,
             configs_provider,
             last = std::optional<Clock::time_point>{}]() mutable {
                const auto configs = configs_provider();
                const auto interval =
                    configs->image_index_folder_poll_interval_seconds_;
                if (!last ||
                    (interval > 0 &&
                     std::chrono::duration<double>(Clock::now() - *last)
                             .count() >= interval)) {
                    impl_->poll_folders(configs);
                    last = Clock::now();
                }
                return false;
            }
        );
        auto_new_folder_thread_ =
            launch("new-folder", [this, configs_provider] {
                this->refresh_new_folder(configs_provider());
                return false;
            });
        auto_visit_thread_ = launch("visit", [this, configs_provider] {
            impl_->flush_pending_removals();
            impl_->refresh_next_visit(configs_provider());
            return false;
        });
    }

    void ImageIndex::start_auto_tagging(
        std::function<std::shared_ptr<const ServerConfigs>()> configs_provider
    ) {
        auto_tagging_thread_ = std::thread(
            [this, configs_provider = std::move(configs_provider)] {
                while (!auto_tagging_stop_) {
                    if (!ready_for_background_work()) {
                        sung::sleep_naive(0.1);
                        continue;
                    }
                    const auto configs = configs_provider();
                    impl_->refresh_tags(configs);
                    // Work drains independently of the model-info cadence.
                    sung::sleep_naive(0.1);
                }
            }
        );
    }

    ImageListResponse ImageIndex::query(
        const Path& dir,
        const std::string& query,
        const bool recursive,
        const ImageSortOrder sort_order,
        const bool avif_only
    ) const {
        return impl_->query(dir, query, recursive, sort_order, avif_only);
    }

    void ImageIndex::remove_api_path(const std::string_view api_path) {
        impl_->remove_api_path(api_path);
    }

    std::optional<nlohmann::json> ImageIndex::tag_analysis(
        const Path& physical_path
    ) const {
        return impl_->tag_analysis(physical_path);
    }

    std::optional<TagAnalysisRecord> ImageIndex::current_tag_analysis(
        const Path& source_path, const bool require_current_analyzer
    ) const {
        return impl_->current_tag_analysis(
            source_path, require_current_analyzer
        );
    }

    bool ImageIndex::proxy_materialization_current(
        const Path& proxy_path, const std::string_view materialization_id
    ) const {
        return impl_->proxy_materialization_current(
            proxy_path, materialization_id
        );
    }

    void ImageIndex::mark_proxy_materialized(
        const Path& source_path,
        const Path& proxy_path,
        std::string materialization_id
    ) {
        impl_->mark_proxy_materialized(
            source_path, proxy_path, std::move(materialization_id)
        );
    }

}  // namespace sung
