#include "index/image_index.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <print>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <absl/strings/ascii.h>
#include <sqlite3.h>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>
#include <sung/basic/os_detect.hpp>
#include <sung/basic/time.hpp>

#include "sung/image/img_info.hpp"

#include "image_query.hpp"
#include "tag_sidecar.hpp"
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

    constexpr int DATABASE_SCHEMA_VERSION = 4;
    constexpr int64_t NANOSECONDS_PER_SECOND = 1'000'000'000;


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
        std::string physical_path_;
        int64_t file_size_ = 0;
        int64_t modified_time_ = 0;
        int64_t sort_time_ns_ = 0;
        bool eligible_ = false;
        int width_ = 0;
        int height_ = 0;
        std::string model_;
        std::vector<std::string> prompts_;
    };

    using CachedTagAnalysis = sung::TagAnalysisRecord;

    struct IndexedFile {
        std::string root_key_;
        std::string physical_path_;
        std::string browser_path_;
        std::string parent_browser_path_;
        sung::ImageListResponse::FileInfo info_;
        std::string model_;
        std::vector<std::string> prompts_;
        std::string logical_path_;
        std::string tag_input_path_;
        int64_t tag_input_size_ = 0;
        int64_t tag_input_modified_time_ = 0;
        std::vector<std::string> tags_;
    };

    struct IndexedFolder {
        std::string root_key_;
        std::string name_;
        std::string path_;
        std::string parent_path_;
        int64_t sort_time_ns_ = 0;
    };

    struct IndexSnapshot {
        uint64_t generation_ = 0;
        std::vector<IndexedFile> files_;
        std::vector<IndexedFolder> folders_;
        std::set<std::string> namespaces_;
        std::unordered_map<std::string, int64_t> namespace_sort_times_;
    };


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

    int64_t get_modified_time(const sung::Path& path, std::error_code& ec) {
        const auto value = sung::fs::last_write_time(path, ec);
        if (ec)
            return 0;
        return static_cast<int64_t>(value.time_since_epoch().count());
    }

    int64_t get_file_size(const sung::Path& path, std::error_code& ec) {
        const auto value = sung::fs::file_size(path, ec);
        if (ec)
            return 0;
        return static_cast<int64_t>(std::min<uintmax_t>(
            value, static_cast<uintmax_t>(std::numeric_limits<int64_t>::max())
        ));
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

        if (info.load_img_metadata() && info.parse_comfyui_workflow()) {
            info.parse_stable_diffusion_model();
            info.parse_stable_diffusion_prompt();
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

        std::error_code stat_error;
        const auto size = get_file_size(physical_path, stat_error);
        const auto modified = get_modified_time(physical_path, stat_error);
        if (stat_error) {
            probe.stat_failed_ = true;
            return probe;
        }

        if (existing && existing->file_size_ == size &&
            existing->modified_time_ == modified) {
            probe.reused_ = true;
            probe.metadata_ = *existing;
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


class sung::ImageIndex::Impl {

public:
    explicit Impl(Path database_path)
        : database_path_(std::move(database_path)) {
        snapshot_ = std::make_shared<const IndexSnapshot>();
    }

    ~Impl() {
        if (database_)
            sqlite3_close(database_);
    }

    void open_database() {
        std::error_code ec;
        const auto parent = database_path_.parent_path();
        if (!parent.empty())
            fs::create_directories(parent, ec);
        if (ec) {
            std::println(
                "ImageIndex: Cannot create cache directory: {}. Using memory "
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
            !execute_sql(database_, "PRAGMA synchronous=NORMAL;")) {
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

        if (schema_version == 1) {
            if (!execute_sql(
                    database_,
                    "BEGIN IMMEDIATE;"
                    "ALTER TABLE image_metadata ADD COLUMN sort_time_ns "
                    "INTEGER NOT NULL DEFAULT 0;"
                    "PRAGMA user_version=2;"
                    "COMMIT;"
                )) {
                sqlite3_close(database_);
                database_ = nullptr;
                return;
            }
            schema_version = 2;
        }

        const char* create_tag_table =
            "CREATE TABLE IF NOT EXISTS image_tag_analysis ("
            "logical_path TEXT PRIMARY KEY,"
            "input_path TEXT NOT NULL DEFAULT '',"
            "input_size INTEGER NOT NULL DEFAULT 0,"
            "input_modified_time INTEGER NOT NULL DEFAULT 0,"
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
            "sidecar_path TEXT NOT NULL DEFAULT '',"
            "proxy_path TEXT NOT NULL DEFAULT '',"
            "proxy_size INTEGER NOT NULL DEFAULT 0,"
            "proxy_modified_time INTEGER NOT NULL DEFAULT 0,"
            "proxy_materialization_id TEXT NOT NULL DEFAULT ''"
            ");";

        if (schema_version == 2) {
            if (!execute_sql(database_, "BEGIN IMMEDIATE;") ||
                !execute_sql(database_, create_tag_table) ||
                !execute_sql(database_, "PRAGMA user_version=4;") ||
                !execute_sql(database_, "COMMIT;")) {
                execute_sql(database_, "ROLLBACK;");
                sqlite3_close(database_);
                database_ = nullptr;
                return;
            }
            schema_version = 4;
        }

        if (schema_version == 3) {
            if (!execute_sql(
                    database_,
                    "BEGIN IMMEDIATE;"
                    "ALTER TABLE image_tag_analysis ADD COLUMN analysis_id "
                    "TEXT NOT NULL DEFAULT '';"
                    "ALTER TABLE image_tag_analysis ADD COLUMN sidecar_path "
                    "TEXT NOT NULL DEFAULT '';"
                    "ALTER TABLE image_tag_analysis ADD COLUMN proxy_path "
                    "TEXT NOT NULL DEFAULT '';"
                    "ALTER TABLE image_tag_analysis ADD COLUMN proxy_size "
                    "INTEGER NOT NULL DEFAULT 0;"
                    "ALTER TABLE image_tag_analysis ADD COLUMN "
                    "proxy_modified_time INTEGER NOT NULL DEFAULT 0;"
                    "ALTER TABLE image_tag_analysis ADD COLUMN "
                    "proxy_materialization_id TEXT NOT NULL DEFAULT '';"
                    "PRAGMA user_version=4;"
                    "COMMIT;"
                )) {
                execute_sql(database_, "ROLLBACK;");
                sqlite3_close(database_);
                database_ = nullptr;
                return;
            }
            schema_version = 4;
        }

        if (schema_version != DATABASE_SCHEMA_VERSION) {
            if (!execute_sql(
                    database_,
                    "BEGIN;"
                    "DROP TABLE IF EXISTS image_metadata;"
                    "DROP TABLE IF EXISTS image_tag_analysis;"
                    "CREATE TABLE image_metadata ("
                    "physical_path TEXT PRIMARY KEY,"
                    "file_size INTEGER NOT NULL,"
                    "modified_time INTEGER NOT NULL,"
                    "sort_time_ns INTEGER NOT NULL,"
                    "eligible INTEGER NOT NULL,"
                    "width INTEGER NOT NULL,"
                    "height INTEGER NOT NULL,"
                    "model TEXT NOT NULL,"
                    "prompts_json TEXT NOT NULL"
                    ");"
                ) ||
                !execute_sql(database_, create_tag_table) ||
                !execute_sql(database_, "PRAGMA user_version=4;") ||
                !execute_sql(database_, "COMMIT;")) {
                execute_sql(database_, "ROLLBACK;");
                sqlite3_close(database_);
                database_ = nullptr;
                return;
            }
        } else if (
            !execute_sql(
                database_,
                "CREATE TABLE IF NOT EXISTS image_metadata ("
                "physical_path TEXT PRIMARY KEY,"
                "file_size INTEGER NOT NULL,"
                "modified_time INTEGER NOT NULL,"
                "sort_time_ns INTEGER NOT NULL,"
                "eligible INTEGER NOT NULL,"
                "width INTEGER NOT NULL,"
                "height INTEGER NOT NULL,"
                "model TEXT NOT NULL,"
                "prompts_json TEXT NOT NULL"
                ");"
            ) ||
            !execute_sql(database_, create_tag_table)
        ) {
            sqlite3_close(database_);
            database_ = nullptr;
            return;
        }

        load_metadata();
        load_tag_analyses();
    }

    void load_metadata() {
        if (!database_)
            return;

        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(
                database_,
                "SELECT physical_path, file_size, modified_time, sort_time_ns, "
                "eligible, width, height, model, prompts_json FROM "
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

            try {
                const auto* prompts_text = reinterpret_cast<const char*>(
                    sqlite3_column_text(statement, 8)
                );
                metadata.prompts_ = nlohmann::json::parse(prompts_text)
                                        .get<std::vector<std::string>>();
            } catch (const std::exception&) {
                metadata.prompts_.clear();
            }
            metadata_[metadata.physical_path_] = std::move(metadata);
        }
        sqlite3_finalize(statement);
    }

    void load_tag_analyses() {
        if (!database_)
            return;

        sqlite3_stmt* statement = nullptr;
        const char* query =
            "SELECT logical_path, input_path, input_size, "
            "input_modified_time, analyzer_fingerprint, model_id, "
            "general_threshold, character_threshold, analysis_json, "
            "analyzed_at, attempt_input_path, attempt_input_size, "
            "attempt_input_modified_time, attempt_analyzer_fingerprint, "
            "last_attempt_at, failure_count, last_error, analysis_id, "
            "sidecar_path, proxy_path, proxy_size, proxy_modified_time, "
            "proxy_materialization_id "
            "FROM image_tag_analysis;";
        if (sqlite3_prepare_v2(database_, query, -1, &statement, nullptr) !=
            SQLITE_OK) {
            std::println(
                "ImageIndex: Cannot load tag cache: {}",
                sqlite3_errmsg(database_)
            );
            return;
        }

        while (sqlite3_step(statement) == SQLITE_ROW) {
            CachedTagAnalysis analysis;
            analysis.logical_path_ = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 0)
            );
            analysis.input_path_ = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 1)
            );
            analysis.input_size_ = sqlite3_column_int64(statement, 2);
            analysis.input_modified_time_ = sqlite3_column_int64(statement, 3);
            analysis.analyzer_fingerprint_ = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 4)
            );
            analysis.model_id_ = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 5)
            );
            analysis.general_threshold_ = sqlite3_column_double(statement, 6);
            analysis.character_threshold_ = sqlite3_column_double(statement, 7);
            const auto* json_text = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 8)
            );
            if (json_text && *json_text) {
                try {
                    analysis.analysis_ = nlohmann::json::parse(json_text);
                    analysis.searchable_tags_ =
                        sung::searchable_tags_from_analysis(analysis.analysis_);
                } catch (const std::exception&) {
                    analysis.analysis_ = nlohmann::json{};
                }
            }
            analysis.analyzed_at_ = sqlite3_column_int64(statement, 9);
            analysis.attempt_input_path_ = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 10)
            );
            analysis.attempt_input_size_ = sqlite3_column_int64(statement, 11);
            analysis.attempt_input_modified_time_ = sqlite3_column_int64(
                statement, 12
            );
            analysis.attempt_analyzer_fingerprint_ =
                reinterpret_cast<const char*>(
                    sqlite3_column_text(statement, 13)
                );
            analysis.last_attempt_at_ = sqlite3_column_int64(statement, 14);
            analysis.failure_count_ = sqlite3_column_int(statement, 15);
            analysis.last_error_ = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 16)
            );
            analysis.analysis_id_ = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 17)
            );
            analysis.sidecar_path_ = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 18)
            );
            analysis.proxy_path_ = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 19)
            );
            analysis.proxy_size_ = sqlite3_column_int64(statement, 20);
            analysis.proxy_modified_time_ = sqlite3_column_int64(statement, 21);
            analysis.proxy_materialization_id_ = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 22)
            );
            if (analysis.analysis_id_.empty() &&
                !analysis.analysis_.is_null()) {
                analysis.analysis_id_ = sung::make_analysis_id(analysis);
            }
            tag_analyses_.insert_or_assign(
                analysis.logical_path_, std::move(analysis)
            );
        }
        sqlite3_finalize(statement);
    }

    bool persist_changes(
        const std::vector<CachedMetadata>& changed,
        const std::vector<std::string>& removed,
        const bool replace_all
    ) {
        if (!database_)
            return false;
        if (changed.empty() && removed.empty() && !replace_all)
            return true;
        if (!execute_sql(database_, "BEGIN IMMEDIATE;"))
            return false;

        if (replace_all &&
            !execute_sql(database_, "DELETE FROM image_metadata;")) {
            execute_sql(database_, "ROLLBACK;");
            return false;
        }

        sqlite3_stmt* upsert = nullptr;
        sqlite3_stmt* erase = nullptr;
        const auto upsert_sql =
            "INSERT INTO image_metadata "
            "(physical_path, file_size, modified_time, sort_time_ns, eligible, "
            "width, height, model, prompts_json) VALUES (?, ?, ?, ?, ?, ?, ?, "
            "?, ?) "
            "ON CONFLICT(physical_path) DO UPDATE SET "
            "file_size=excluded.file_size, "
            "modified_time=excluded.modified_time, "
            "sort_time_ns=excluded.sort_time_ns, "
            "eligible=excluded.eligible, width=excluded.width, "
            "height=excluded.height, model=excluded.model, "
            "prompts_json=excluded.prompts_json;";

        bool success = sqlite3_prepare_v2(
                           database_, upsert_sql, -1, &upsert, nullptr
                       ) == SQLITE_OK &&
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
            sqlite3_bind_text(
                upsert, 1, item.physical_path_.c_str(), -1, SQLITE_TRANSIENT
            );
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
            success = sqlite3_step(upsert) == SQLITE_DONE;
            sqlite3_reset(upsert);
            sqlite3_clear_bindings(upsert);
        }

        for (const auto& path : removed) {
            if (!success)
                break;
            sqlite3_bind_text(erase, 1, path.c_str(), -1, SQLITE_TRANSIENT);
            success = sqlite3_step(erase) == SQLITE_DONE;
            sqlite3_reset(erase);
            sqlite3_clear_bindings(erase);
        }

        sqlite3_finalize(upsert);
        sqlite3_finalize(erase);
        if (success)
            success = execute_sql(database_, "COMMIT;");
        else
            execute_sql(database_, "ROLLBACK;");
        return success;
    }

    bool persist_tag_analysis(const CachedTagAnalysis& item) {
        if (!database_)
            return true;

        const char* sql =
            "INSERT INTO image_tag_analysis ("
            "logical_path, input_path, input_size, input_modified_time, "
            "analyzer_fingerprint, model_id, general_threshold, "
            "character_threshold, analysis_json, analyzed_at, "
            "attempt_input_path, attempt_input_size, "
            "attempt_input_modified_time, attempt_analyzer_fingerprint, "
            "last_attempt_at, failure_count, last_error, analysis_id, "
            "sidecar_path, proxy_path, proxy_size, proxy_modified_time, "
            "proxy_materialization_id"
            ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
            "?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(logical_path) DO UPDATE SET "
            "input_path=excluded.input_path, input_size=excluded.input_size, "
            "input_modified_time=excluded.input_modified_time, "
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
            "sidecar_path=excluded.sidecar_path, "
            "proxy_path=excluded.proxy_path, proxy_size=excluded.proxy_size, "
            "proxy_modified_time=excluded.proxy_modified_time, "
            "proxy_materialization_id=excluded.proxy_materialization_id;";

        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) !=
            SQLITE_OK) {
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
        bind_text(2, item.input_path_);
        sqlite3_bind_int64(statement, 3, item.input_size_);
        sqlite3_bind_int64(statement, 4, item.input_modified_time_);
        bind_text(5, item.analyzer_fingerprint_);
        bind_text(6, item.model_id_);
        sqlite3_bind_double(statement, 7, item.general_threshold_);
        sqlite3_bind_double(statement, 8, item.character_threshold_);
        bind_text(9, analysis_json);
        sqlite3_bind_int64(statement, 10, item.analyzed_at_);
        bind_text(11, item.attempt_input_path_);
        sqlite3_bind_int64(statement, 12, item.attempt_input_size_);
        sqlite3_bind_int64(statement, 13, item.attempt_input_modified_time_);
        bind_text(14, item.attempt_analyzer_fingerprint_);
        sqlite3_bind_int64(statement, 15, item.last_attempt_at_);
        sqlite3_bind_int(statement, 16, item.failure_count_);
        bind_text(17, item.last_error_);
        bind_text(18, item.analysis_id_);
        bind_text(19, item.sidecar_path_);
        bind_text(20, item.proxy_path_);
        sqlite3_bind_int64(statement, 21, item.proxy_size_);
        sqlite3_bind_int64(statement, 22, item.proxy_modified_time_);
        bind_text(23, item.proxy_materialization_id_);

        const auto success = sqlite3_step(statement) == SQLITE_DONE;
        sqlite3_finalize(statement);
        return success;
    }

    bool erase_tag_analysis(const std::string& logical_path) {
        if (!database_)
            return true;
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(
                database_,
                "DELETE FROM image_tag_analysis WHERE logical_path=?;",
                -1,
                &statement,
                nullptr
            ) != SQLITE_OK) {
            return false;
        }
        sqlite3_bind_text(
            statement, 1, logical_path.c_str(), -1, SQLITE_TRANSIENT
        );
        const auto success = sqlite3_step(statement) == SQLITE_DONE;
        sqlite3_finalize(statement);
        return success;
    }

    ImageIndexRefreshStats refresh(
        const std::shared_ptr<const ServerConfigs>& configs
    ) {
        std::lock_guard refresh_lock{ refresh_mutex_ };
        sung::MonotonicRealtimeTimer timer;
        ImageIndexRefreshStats stats;
        stats.persistent_ = database_ != nullptr;

        const auto old_snapshot = load_snapshot();
        const auto initial_refresh = old_snapshot->generation_ == 0;
        if (initial_refresh) {
            std::println(
                "ImageIndex: Building and validating the image index before "
                "the server starts..."
            );
        }
        auto next = std::make_shared<IndexSnapshot>();
        next->generation_ = old_snapshot->generation_ + 1;

        std::unordered_set<std::string> seen_physical;
        std::unordered_set<std::string> seen_api_paths;
        std::unordered_map<std::string, size_t> seen_folder_paths;
        std::unordered_map<std::string, Path> seen_sidecars;
        std::vector<CachedMetadata> changed;
        bool all_roots_accessible = true;

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

        const auto preserve_root = [&](const std::string& root_key) {
            for (const auto& file : old_snapshot->files_) {
                if (file.root_key_ != root_key)
                    continue;
                if (seen_api_paths.insert(sung::tostr(file.info_.path_)).second)
                    next->files_.push_back(file);
                seen_physical.insert(file.physical_path_);
            }
            for (const auto& folder : old_snapshot->folders_) {
                if (folder.root_key_ != root_key)
                    continue;
                add_folder(folder);
            }
        };

        for (const auto& [namespace_name, binding] : configs->dir_bindings_) {
            next->namespaces_.insert(namespace_name);

            for (const auto& configured_root : binding.local_dirs_) {
                std::error_code ec;
                auto root = fs::absolute(configured_root, ec);
                if (ec)
                    root = configured_root.lexically_normal();
                else
                    root = root.lexically_normal();
                const auto root_key = make_root_key(namespace_name, root);

                if (!fs::is_directory(root, ec) || ec) {
                    all_roots_accessible = false;
                    const auto previous_time =
                        old_snapshot->namespace_sort_times_.find(
                            namespace_name
                        );
                    if (previous_time !=
                        old_snapshot->namespace_sort_times_.end()) {
                        auto& sort_time =
                            next->namespace_sort_times_[namespace_name];
                        sort_time = std::max(sort_time, previous_time->second);
                    }
                    std::println(
                        "ImageIndex: Root unavailable, preserving previous "
                        "snapshot: {}",
                        sung::tostr(root)
                    );
                    preserve_root(root_key);
                    continue;
                }

                auto& namespace_sort_time =
                    next->namespace_sort_times_[namespace_name];
                namespace_sort_time = std::max(
                    namespace_sort_time, get_image_sort_time(root)
                );

                std::vector<Path> physical_files;
                std::vector<Path> sidecar_files;
                std::vector<IndexedFolder> root_folders;
                fs::recursive_directory_iterator iterator{
                    root, fs::directory_options::skip_permission_denied, ec
                };
                bool scan_failed = static_cast<bool>(ec);
                const fs::recursive_directory_iterator end;
                while (!scan_failed && iterator != end) {
                    const auto entry = *iterator;
                    if (entry.is_directory(ec) && !ec) {
                        const auto relative = entry.path().lexically_relative(
                            root
                        );
                        const auto namespace_path = sung::fromstr(
                            namespace_name
                        );
                        const auto browser_path = sung::tostr(
                            namespace_path / relative
                        );
                        root_folders.push_back(
                            {
                                root_key,
                                sung::tostr(entry.path().filename()),
                                browser_path,
                                sung::tostr(
                                    (namespace_path / relative).parent_path()
                                ),
                                get_image_sort_time(entry.path()),
                            }
                        );
                    } else if (!ec && entry.is_regular_file(ec) && !ec) {
                        auto path =
                            fs::absolute(entry.path(), ec).lexically_normal();
                        if (!ec && sung::is_sprintboard_tag_sidecar_path(path))
                            sidecar_files.push_back(std::move(path));
                        else if (!ec)
                            physical_files.push_back(std::move(path));
                    }

                    if (ec) {
                        scan_failed = true;
                        break;
                    }
                    iterator.increment(ec);
                    scan_failed = static_cast<bool>(ec);
                }

                if (scan_failed) {
                    all_roots_accessible = false;
                    std::println(
                        "ImageIndex: Scan failed, preserving previous snapshot "
                        "for: {}",
                        sung::tostr(root)
                    );
                    preserve_root(root_key);
                    continue;
                }

                for (auto& folder : root_folders) add_folder(std::move(folder));

                for (const auto& sidecar_path : sidecar_files) {
                    const auto parsed = sung::read_tag_sidecar(sidecar_path);
                    if (!parsed) {
                        std::println(
                            "ImageIndex: Ignoring invalid tag sidecar {}: {}",
                            sung::tostr(sidecar_path),
                            parsed.error()
                        );
                        continue;
                    }
                    seen_sidecars.insert_or_assign(
                        parsed->logical_path_, sidecar_path
                    );

                    const auto existing = tag_analyses_.find(
                        parsed->logical_path_
                    );
                    if (existing != tag_analyses_.end()) {
                        if (existing->second.analyzed_at_ >
                            parsed->analyzed_at_) {
                            continue;
                        }
                        if (existing->second.analyzed_at_ ==
                                parsed->analyzed_at_ &&
                            existing->second.analysis_id_ !=
                                parsed->analysis_id_) {
                            continue;
                        }
                        if (existing->second.analysis_id_ ==
                                parsed->analysis_id_ &&
                            existing->second.sidecar_path_ ==
                                sung::tostr(sidecar_path) &&
                            existing->second.proxy_path_ ==
                                parsed->proxy_path_ &&
                            existing->second.proxy_size_ ==
                                parsed->proxy_size_ &&
                            existing->second.proxy_modified_time_ ==
                                parsed->proxy_modified_time_ &&
                            existing->second.proxy_materialization_id_ ==
                                parsed->proxy_materialization_id_) {
                            continue;
                        }
                    }

                    auto imported = *parsed;
                    if (existing != tag_analyses_.end()) {
                        imported.attempt_input_path_ =
                            existing->second.attempt_input_path_;
                        imported.attempt_input_size_ =
                            existing->second.attempt_input_size_;
                        imported.attempt_input_modified_time_ =
                            existing->second.attempt_input_modified_time_;
                        imported.attempt_analyzer_fingerprint_ =
                            existing->second.attempt_analyzer_fingerprint_;
                        imported.last_attempt_at_ =
                            existing->second.last_attempt_at_;
                        imported.failure_count_ =
                            existing->second.failure_count_;
                        imported.last_error_ = existing->second.last_error_;
                    }
                    tag_analyses_.insert_or_assign(
                        imported.logical_path_, imported
                    );
                    if (!persist_tag_analysis(imported)) {
                        std::println(
                            "ImageIndex: Failed to cache tag sidecar {}",
                            sung::tostr(sidecar_path)
                        );
                    }
                }

                for (const auto& path : physical_files) {
                    const auto path_str = sung::tostr(path);
                    seen_physical.insert(path_str);
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

                std::unordered_map<std::string, Path> proxy_sources;
                std::unordered_set<std::string> paired_sources;
                std::unordered_set<std::string> stale_proxies;
                for (const auto& path : physical_files) {
                    const auto source_path =
                        sung::sprintboard_proxy_source_path(path);
                    if (!source_path)
                        continue;
                    const auto source = sources_by_path.find(
                        make_path_key(*source_path)
                    );
                    if (source == sources_by_path.end())
                        continue;

                    std::error_code source_time_error;
                    std::error_code proxy_time_error;
                    const auto source_time = fs::last_write_time(
                        source->second, source_time_error
                    );
                    const auto proxy_time = fs::last_write_time(
                        path, proxy_time_error
                    );
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

                // The probe phase only reads `metadata_` (never writes it),
                // so concurrent lookups across files are safe; each file's
                // filesystem work (stat, and full decode for new/changed
                // files) can therefore overlap instead of running one at a
                // time, which matters a lot when the scan root is behind
                // something with high per-call latency (e.g. an encrypted
                // vault driver).
                std::vector<FileProbe> probes(physical_files.size());
                scan_arena_.execute([&] {
                    tbb::parallel_for(
                        tbb::blocked_range<size_t>(0, physical_files.size()),
                        [&](const tbb::blocked_range<size_t>& range) {
                            for (auto i = range.begin(); i != range.end();
                                 ++i) {
                                const auto path_str = sung::tostr(
                                    physical_files[i]
                                );
                                const auto it = metadata_.find(path_str);
                                const CachedMetadata* existing =
                                    it != metadata_.end() ? &it->second
                                                          : nullptr;
                                const auto path_key = make_path_key(
                                    physical_files[i]
                                );
                                const Path* sort_time_source = nullptr;
                                if (sung::is_sprintboard_proxy_path(
                                        physical_files[i]
                                    )) {
                                    const auto source = proxy_sources.find(
                                        path_key
                                    );
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

                for (size_t i = 0; i < physical_files.size(); ++i) {
                    const auto& physical_path = physical_files[i];
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

                    const auto relative = physical_path.lexically_relative(
                        root
                    );
                    if (relative.empty() ||
                        sung::tostr(relative).starts_with(".."))
                        continue;

                    const auto namespace_path = sung::fromstr(namespace_name);
                    const auto browser_path = sung::tostr(
                        namespace_path / relative
                    );
                    const auto api_path = sung::tostr(
                        Path{ "/img" } / namespace_path / relative
                    );
                    if (!seen_api_paths.insert(api_path).second)
                        continue;

                    IndexedFile entry;
                    entry.root_key_ = root_key;
                    entry.physical_path_ = path_str;
                    entry.browser_path_ = browser_path;
                    entry.parent_browser_path_ = sung::tostr(
                        (namespace_path / relative).parent_path()
                    );
                    const auto proxy_source = proxy_sources.find(
                        make_path_key(physical_path)
                    );
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
                    entry.logical_path_ = sung::detail::logical_image_key(
                        physical_path
                    );
                    const auto tag_input = proxy_source != proxy_sources.end()
                                               ? proxy_source->second
                                               : physical_path;
                    entry.tag_input_path_ = sung::tostr(tag_input);
                    std::error_code tag_stat_error;
                    entry.tag_input_size_ = get_file_size(
                        tag_input, tag_stat_error
                    );
                    entry.tag_input_modified_time_ = get_modified_time(
                        tag_input, tag_stat_error
                    );
                    if (tag_stat_error) {
                        entry.tag_input_size_ = 0;
                        entry.tag_input_modified_time_ = 0;
                    }
                    if (const auto tag_it =
                            tag_analyses_.find(entry.logical_path_);
                        tag_it != tag_analyses_.end() &&
                        !tag_it->second.analysis_.is_null()) {
                        entry.tags_ = tag_it->second.searchable_tags_;
                    }
                    next->files_.push_back(std::move(entry));
                }
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

        if (all_roots_accessible) {
            std::unordered_set<std::string> visible_logical_paths;
            visible_logical_paths.reserve(next->files_.size());
            for (const auto& file : next->files_)
                visible_logical_paths.insert(file.logical_path_);
            for (auto it = tag_analyses_.begin(); it != tag_analyses_.end();) {
                if (visible_logical_paths.contains(it->first)) {
                    ++it;
                    continue;
                }
                const auto source_path = sung::fromstr(it->first);
                const auto proxy_path = sung::make_sprintboard_proxy_path(
                    source_path
                );
                std::error_code source_error;
                std::error_code proxy_error;
                const bool source_exists = sung::fs::exists(
                    source_path, source_error
                );
                const bool proxy_exists = sung::fs::exists(
                    proxy_path, proxy_error
                );
                if (source_error || proxy_error || source_exists ||
                    proxy_exists) {
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
                if (const auto sidecar = seen_sidecars.find(it->first);
                    sidecar != seen_sidecars.end()) {
                    std::error_code sidecar_error;
                    fs::remove(sidecar->second, sidecar_error);
                    if (sidecar_error) {
                        std::println(
                            "ImageIndex: Failed to remove orphan tag sidecar "
                            "{}: {}",
                            sung::tostr(sidecar->second),
                            sidecar_error.message()
                        );
                    }
                }
                it = tag_analyses_.erase(it);
            }
        }

        if (database_) {
            std::vector<CachedMetadata> persistence_items;
            if (database_dirty_) {
                persistence_items.reserve(metadata_.size());
                for (const auto& [path, metadata] : metadata_)
                    persistence_items.push_back(metadata);
            } else {
                persistence_items = changed;
            }

            if (!persist_changes(persistence_items, removed, database_dirty_)) {
                database_dirty_ = true;
                std::println(
                    "ImageIndex: Cache update failed; continuing with memory "
                    "snapshot and retrying on the next refresh."
                );
            } else {
                database_dirty_ = false;
            }
        }

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
        store_snapshot(std::move(next));

        std::println(
            "ImageIndex: {} images, {} folders ({} reused, {} indexed, "
            "{} removed) in {:.3f} seconds",
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
        if (!configs->tagger_enabled_)
            return;

        const TaggerClient client{ configs->tagger_host_,
                                   configs->tagger_port_ };
        const auto info = client.get_info();
        if (!info) {
            std::println(
                "ImageTagger: Service unavailable at {}:{}: {}",
                configs->tagger_host_,
                configs->tagger_port_,
                info.error()
            );
            return;
        }

        {
            std::lock_guard refresh_lock{ refresh_mutex_ };
            current_analyzer_fingerprint_ = info->fingerprint_;
        }

        struct Candidate {
            std::string logical_path_;
            Path input_path_;
            int64_t input_size_ = 0;
            int64_t input_modified_time_ = 0;
        };

        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch()
        )
                             .count();
        std::vector<Candidate> candidates;
        {
            std::lock_guard refresh_lock{ refresh_mutex_ };
            const auto current = load_snapshot();
            std::unordered_set<std::string> queued;
            for (const auto& file : current->files_) {
                if (file.tag_input_size_ <= 0 ||
                    !queued.insert(file.logical_path_).second) {
                    continue;
                }

                const auto existing = tag_analyses_.find(file.logical_path_);
                if (existing != tag_analyses_.end() &&
                    !existing->second.analysis_.is_null() &&
                    existing->second.input_path_ != file.tag_input_path_ &&
                    sung::is_sprintboard_proxy_path(
                        sung::fromstr(file.tag_input_path_)
                    )) {
                    std::error_code old_input_error;
                    if (!sung::fs::is_regular_file(
                            sung::fromstr(existing->second.input_path_),
                            old_input_error
                        )) {
                        continue;
                    }
                }
                const bool current_analysis =
                    existing != tag_analyses_.end() &&
                    !existing->second.analysis_.is_null() &&
                    existing->second.input_path_ == file.tag_input_path_ &&
                    existing->second.input_size_ == file.tag_input_size_ &&
                    existing->second.input_modified_time_ ==
                        file.tag_input_modified_time_ &&
                    existing->second.analyzer_fingerprint_ ==
                        info->fingerprint_;
                if (current_analysis)
                    continue;

                if (existing != tag_analyses_.end()) {
                    const auto& attempt = existing->second;
                    const bool same_failed_attempt =
                        !attempt.last_error_.empty() &&
                        attempt.attempt_input_path_ == file.tag_input_path_ &&
                        attempt.attempt_input_size_ == file.tag_input_size_ &&
                        attempt.attempt_input_modified_time_ ==
                            file.tag_input_modified_time_ &&
                        attempt.attempt_analyzer_fingerprint_ ==
                            info->fingerprint_;
                    const auto shift = std::min(
                        std::max(attempt.failure_count_ - 1, 0), 7
                    );
                    const int64_t retry_delay = std::min<int64_t>(
                        30LL << shift, 3600
                    );
                    if (same_failed_attempt &&
                        now - attempt.last_attempt_at_ < retry_delay) {
                        continue;
                    }
                }

                candidates.push_back(
                    {
                        file.logical_path_,
                        sung::fromstr(file.tag_input_path_),
                        file.tag_input_size_,
                        file.tag_input_modified_time_,
                    }
                );
            }
        }

        const auto batch_size = static_cast<size_t>(
            std::max(configs->tagger_batch_size_, 1)
        );
        for (size_t offset = 0; offset < candidates.size();
             offset += batch_size) {
            const auto count = std::min(batch_size, candidates.size() - offset);
            std::vector<Path> paths;
            paths.reserve(count);
            for (size_t i = 0; i < count; ++i)
                paths.push_back(candidates[offset + i].input_path_);

            const auto results = client.analyze(paths, info->fingerprint_);
            if (!results) {
                std::println(
                    "ImageTagger: Batch request failed: {}", results.error()
                );
                return;
            }

            std::lock_guard refresh_lock{ refresh_mutex_ };
            const auto latest_snapshot = load_snapshot();
            bool snapshot_changed = false;
            for (size_t i = 0; i < count; ++i) {
                const auto& candidate = candidates[offset + i];
                const auto& result = results->at(i);

                const auto current_file = std::find_if(
                    latest_snapshot->files_.begin(),
                    latest_snapshot->files_.end(),
                    [&](const auto& file) {
                        return file.logical_path_ == candidate.logical_path_ &&
                               file.tag_input_path_ ==
                                   sung::tostr(candidate.input_path_) &&
                               file.tag_input_size_ == candidate.input_size_ &&
                               file.tag_input_modified_time_ ==
                                   candidate.input_modified_time_;
                    }
                );
                if (current_file == latest_snapshot->files_.end())
                    continue;

                std::error_code stat_error;
                const auto current_size = get_file_size(
                    candidate.input_path_, stat_error
                );
                const auto current_modified = get_modified_time(
                    candidate.input_path_, stat_error
                );
                if (stat_error || current_size != candidate.input_size_ ||
                    current_modified != candidate.input_modified_time_) {
                    continue;
                }

                auto analysis = tag_analyses_[candidate.logical_path_];
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
                    analysis.input_path_ = sung::tostr(candidate.input_path_);
                    analysis.input_size_ = candidate.input_size_;
                    analysis.input_modified_time_ =
                        candidate.input_modified_time_;
                    analysis.analyzer_fingerprint_ = info->fingerprint_;
                    analysis.model_id_ = info->model_id_;
                    analysis.general_threshold_ = info->general_threshold_;
                    analysis.character_threshold_ = info->character_threshold_;
                    analysis.analysis_ = result.analysis_;
                    analysis.searchable_tags_ = result.searchable_tags_;
                    analysis.analyzed_at_ = now;
                    analysis.analysis_id_ = sung::make_analysis_id(analysis);
                    analysis.sidecar_path_ = sung::tostr(
                        sung::make_sprintboard_tag_sidecar_path(
                            candidate.input_path_
                        )
                    );
                    analysis.proxy_path_.clear();
                    analysis.proxy_size_ = 0;
                    analysis.proxy_modified_time_ = 0;
                    analysis.proxy_materialization_id_.clear();
                    analysis.failure_count_ = 0;
                    analysis.last_error_.clear();
                    snapshot_changed = true;
                    std::println(
                        "ImageTagger: Saved {} tags for {}",
                        analysis.searchable_tags_.size(),
                        candidate.logical_path_
                    );
                }

                tag_analyses_.insert_or_assign(
                    candidate.logical_path_, analysis
                );
                if (!persist_tag_analysis(analysis)) {
                    std::println(
                        "ImageTagger: Failed to persist analysis state for {}",
                        candidate.logical_path_
                    );
                }
                if (result.error_.empty()) {
                    const auto sidecar_result = sung::write_tag_sidecar(
                        sung::fromstr(analysis.sidecar_path_), analysis
                    );
                    if (!sidecar_result) {
                        std::println(
                            "ImageTagger: Failed to write sidecar for {}: {}",
                            candidate.logical_path_,
                            sidecar_result.error()
                        );
                    }
                }
            }

            if (snapshot_changed) {
                auto next = std::make_shared<IndexSnapshot>(*latest_snapshot);
                for (auto& file : next->files_) {
                    const auto found = tag_analyses_.find(file.logical_path_);
                    if (found != tag_analyses_.end() &&
                        !found->second.analysis_.is_null()) {
                        file.tags_ = found->second.searchable_tags_;
                    }
                }
                ++next->generation_;
                store_snapshot(std::move(next));
            }
        }
    }

    std::optional<nlohmann::json> tag_analysis(
        const Path& physical_path
    ) const {
        std::lock_guard refresh_lock{ refresh_mutex_ };
        const auto logical_path = sung::detail::logical_image_key(
            physical_path
        );
        const auto found = tag_analyses_.find(logical_path);
        if (found == tag_analyses_.end() || found->second.analysis_.is_null())
            return std::nullopt;

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
    ) const {
        std::lock_guard refresh_lock{ refresh_mutex_ };
        const auto logical_path = sung::detail::logical_image_key(source_path);
        const auto found = tag_analyses_.find(logical_path);
        if (found == tag_analyses_.end() || found->second.analysis_.is_null())
            return std::nullopt;

        const auto fingerprint = sung::fingerprint_file(source_path);
        if (!fingerprint ||
            found->second.input_path_ != sung::tostr(source_path) ||
            found->second.input_size_ != fingerprint->size_ ||
            found->second.input_modified_time_ != fingerprint->modified_time_) {
            return std::nullopt;
        }
        if (require_current_analyzer &&
            !current_analyzer_fingerprint_.empty() &&
            found->second.analyzer_fingerprint_ !=
                current_analyzer_fingerprint_) {
            return std::nullopt;
        }
        return found->second;
    }

    bool proxy_materialization_current(
        const Path& proxy_path, const std::string_view materialization_id
    ) const {
        std::lock_guard refresh_lock{ refresh_mutex_ };
        const auto logical_path = sung::detail::logical_image_key(proxy_path);
        const auto found = tag_analyses_.find(logical_path);
        if (found == tag_analyses_.end() ||
            found->second.proxy_materialization_id_ != materialization_id ||
            found->second.proxy_path_ != sung::tostr(proxy_path)) {
            return false;
        }
        const auto fingerprint = sung::fingerprint_file(proxy_path);
        return fingerprint && found->second.proxy_size_ == fingerprint->size_ &&
               found->second.proxy_modified_time_ ==
                   fingerprint->modified_time_;
    }

    void mark_proxy_materialized(
        const Path& source_path,
        const Path& proxy_path,
        std::string materialization_id
    ) {
        std::lock_guard refresh_lock{ refresh_mutex_ };
        const auto logical_path = sung::detail::logical_image_key(source_path);
        const auto found = tag_analyses_.find(logical_path);
        if (found == tag_analyses_.end() || found->second.analysis_.is_null())
            return;
        const auto fingerprint = sung::fingerprint_file(proxy_path);
        if (!fingerprint)
            return;

        auto& analysis = found->second;
        analysis.proxy_path_ = sung::tostr(proxy_path);
        analysis.proxy_size_ = fingerprint->size_;
        analysis.proxy_modified_time_ = fingerprint->modified_time_;
        analysis.proxy_materialization_id_ = std::move(materialization_id);
        if (analysis.sidecar_path_.empty()) {
            analysis.sidecar_path_ = sung::tostr(
                sung::make_sprintboard_tag_sidecar_path(source_path)
            );
        }
        if (!persist_tag_analysis(analysis)) {
            std::println(
                "ImgWalker: Failed to cache proxy materialization for {}",
                logical_path
            );
        }
        const auto sidecar_result = sung::write_tag_sidecar(
            sung::fromstr(analysis.sidecar_path_), analysis
        );
        if (!sidecar_result) {
            std::println(
                "ImgWalker: Failed to update tag sidecar for {}: {}",
                logical_path,
                sidecar_result.error()
            );
        }
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
        std::lock_guard refresh_lock{ refresh_mutex_ };
        const auto current = load_snapshot();
        auto next = std::make_shared<IndexSnapshot>(*current);
        std::vector<std::string> removed_logical_paths;
        std::erase_if(next->files_, [&](const auto& file) {
            if (sung::tostr(file.info_.path_) != api_path)
                return false;
            removed_logical_paths.push_back(file.logical_path_);
            return true;
        });
        for (const auto& logical_path : removed_logical_paths) {
            tag_analyses_.erase(logical_path);
            if (!erase_tag_analysis(logical_path)) {
                std::println(
                    "ImageIndex: Failed to remove tag analysis for {}",
                    logical_path
                );
            }
        }
        ++next->generation_;
        store_snapshot(std::move(next));
    }

    bool persistent() const { return database_ != nullptr; }

private:
    std::shared_ptr<const IndexSnapshot> load_snapshot() const {
        std::lock_guard lock{ snapshot_mutex_ };
        return snapshot_;
    }

    void store_snapshot(std::shared_ptr<const IndexSnapshot> snapshot) {
        std::lock_guard lock{ snapshot_mutex_ };
        snapshot_ = std::move(snapshot);
    }

    Path database_path_;
    sqlite3* database_ = nullptr;
    bool database_dirty_ = false;
    std::unordered_map<std::string, CachedMetadata> metadata_;
    std::unordered_map<std::string, CachedTagAnalysis> tag_analyses_;
    std::string current_analyzer_fingerprint_;
    std::shared_ptr<const IndexSnapshot> snapshot_;
    mutable std::mutex refresh_mutex_;
    mutable std::mutex snapshot_mutex_;
    // Isolated from the default TBB arena (used by CPU-bound AVIF encoding)
    // since this one is deliberately oversubscribed for I/O latency-hiding.
    tbb::task_arena scan_arena_{ SCAN_CONCURRENCY };
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

    ImageIndex::ImageIndex(Path database_path)
        : impl_(std::make_unique<Impl>(std::move(database_path))) {}

    ImageIndex::~ImageIndex() {
        auto_refresh_stop_ = true;
        auto_tagging_stop_ = true;
        if (auto_refresh_thread_.joinable())
            auto_refresh_thread_.join();
        if (auto_tagging_thread_.joinable())
            auto_tagging_thread_.join();
    }

    ImageIndexRefreshStats ImageIndex::initialize(
        std::shared_ptr<const ServerConfigs> configs
    ) {
        impl_->open_database();
        return impl_->refresh(configs);
    }

    ImageIndexRefreshStats ImageIndex::refresh(
        std::shared_ptr<const ServerConfigs> configs
    ) {
        return impl_->refresh(configs);
    }

    void ImageIndex::start_auto_refresh(
        std::function<std::shared_ptr<const ServerConfigs>()> configs_provider,
        const double interval_seconds
    ) {
        auto_refresh_thread_ = std::thread(
            [this,
             configs_provider = std::move(configs_provider),
             interval_seconds] {
                while (!auto_refresh_stop_) {
                    for (double waited = 0;
                         waited < interval_seconds && !auto_refresh_stop_;
                         waited += 0.1) {
                        sung::sleep_naive(0.1);
                    }
                    if (auto_refresh_stop_)
                        break;
                    impl_->refresh(configs_provider());
                }
            }
        );
    }

    void ImageIndex::start_auto_tagging(
        std::function<std::shared_ptr<const ServerConfigs>()> configs_provider
    ) {
        auto_tagging_thread_ = std::thread(
            [this, configs_provider = std::move(configs_provider)] {
                while (!auto_tagging_stop_) {
                    const auto configs = configs_provider();
                    impl_->refresh_tags(configs);
                    const auto interval = std::max(
                        configs->tagger_poll_interval_seconds_, 1.0
                    );
                    for (double waited = 0;
                         waited < interval && !auto_tagging_stop_;
                         waited += 0.1) {
                        sung::sleep_naive(0.1);
                    }
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
