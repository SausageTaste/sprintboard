#include "index/image_index.hpp"

#include <algorithm>
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
#include <absl/strings/str_split.h>
#include <sqlite3.h>
#include <sung/basic/os_detect.hpp>
#include <sung/basic/time.hpp>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>

#include "sung/image/img_info.hpp"

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

}  // namespace sung::detail


namespace {

    constexpr int DATABASE_SCHEMA_VERSION = 2;
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


    class Query {

    public:
        explicit Query(const std::string& query) {
            for (auto part : absl::StrSplit(query, ',')) {
                part = absl::StripAsciiWhitespace(part);
                if (part.empty())
                    continue;

                if (part.starts_with("model:")) {
                    model_ = std::string{ part.substr(6) };
                } else if (part.starts_with("dim:")) {
                    const auto dim = part.substr(4);
                    if (dim == "ver") {
                        vertical_ = true;
                        horizontal_ = false;
                    } else if (dim == "hor") {
                        horizontal_ = true;
                        vertical_ = false;
                    }
                } else {
                    terms_.emplace_back(part);
                }
            }
        }

        bool matches_dimensions(const int width, const int height) const {
            if (vertical_ && height <= width)
                return false;
            if (horizontal_ && width <= height)
                return false;
            return true;
        }

        bool needs_metadata() const {
            return !model_.empty() || !terms_.empty();
        }

        bool matches_metadata(
            const std::string& model, const std::vector<std::string>& prompts
        ) const {
            if (!model_.empty() && !model.contains(model_))
                return false;

            for (const auto& prompt : prompts) {
                bool matched = true;
                for (const auto& term : terms_) {
                    if (!prompt.contains(term)) {
                        matched = false;
                        break;
                    }
                }
                if (matched)
                    return true;
            }
            return false;
        }

    private:
        std::vector<std::string> terms_;
        std::string model_;
        bool vertical_ = false;
        bool horizontal_ = false;
    };


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

    struct IndexedFile {
        std::string root_key_;
        std::string physical_path_;
        std::string browser_path_;
        std::string parent_browser_path_;
        sung::ImageListResponse::FileInfo info_;
        std::string model_;
        std::vector<std::string> prompts_;
    };

    struct IndexedFolder {
        std::string root_key_;
        std::string name_;
        std::string path_;
        std::string parent_path_;
    };

    struct IndexSnapshot {
        uint64_t generation_ = 0;
        std::vector<IndexedFile> files_;
        std::vector<IndexedFolder> folders_;
        std::set<std::string> namespaces_;
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
        const sung::Path& physical_path, const CachedMetadata* existing
    ) {
        FileProbe probe;

        auto extension = sung::tostr(physical_path.extension());
        absl::AsciiStrToLower(&extension);
        if (extension == ".png") {
            const auto avif_path = sung::replace_ext(physical_path, ".avif");
            std::error_code shadow_error;
            if (sung::fs::is_regular_file(avif_path, shadow_error) &&
                !shadow_error) {
                probe.shadowed_ = true;
                return probe;
            }
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
            if (probe.metadata_.sort_time_ns_ == 0) {
                probe.metadata_.sort_time_ns_ = get_image_sort_time(
                    physical_path
                );
                if (probe.metadata_.sort_time_ns_ > 0)
                    probe.needs_persist_ = true;
            }
        } else {
            const auto sort_time_ns = get_image_sort_time(physical_path);
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
        } else if (schema_version != DATABASE_SCHEMA_VERSION) {
            if (!execute_sql(
                    database_,
                    "BEGIN;"
                    "DROP TABLE IF EXISTS image_metadata;"
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
                    "PRAGMA user_version=2;"
                    "COMMIT;"
                )) {
                sqlite3_close(database_);
                database_ = nullptr;
                return;
            }
        } else if (!execute_sql(
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
                   )) {
            sqlite3_close(database_);
            database_ = nullptr;
            return;
        }

        load_metadata();
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
        std::unordered_set<std::string> seen_folder_paths;
        std::vector<CachedMetadata> changed;

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
                if (seen_folder_paths.insert(folder.path_).second)
                    next->folders_.push_back(folder);
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
                    std::println(
                        "ImageIndex: Root unavailable, preserving previous "
                        "snapshot: {}",
                        sung::tostr(root)
                    );
                    preserve_root(root_key);
                    continue;
                }

                std::vector<Path> physical_files;
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
                            }
                        );
                    } else if (!ec && entry.is_regular_file(ec) && !ec) {
                        physical_files.push_back(
                            fs::absolute(entry.path(), ec).lexically_normal()
                        );
                    }

                    if (ec) {
                        scan_failed = true;
                        break;
                    }
                    iterator.increment(ec);
                    scan_failed = static_cast<bool>(ec);
                }

                if (scan_failed) {
                    std::println(
                        "ImageIndex: Scan failed, preserving previous snapshot "
                        "for: {}",
                        sung::tostr(root)
                    );
                    preserve_root(root_key);
                    continue;
                }

                for (auto& folder : root_folders) {
                    if (seen_folder_paths.insert(folder.path_).second)
                        next->folders_.push_back(std::move(folder));
                }

                for (const auto& path : physical_files) {
                    const auto path_str = sung::tostr(path);
                    seen_physical.insert(path_str);
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
                                probes[i] = probe_file(
                                    physical_files[i], existing
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
                    entry.info_ = {
                        sung::tostr(physical_path.filename()),
                        sung::fromstr(api_path),
                        metadata.width_,
                        metadata.height_,
                        metadata.sort_time_ns_,
                    };
                    entry.model_ = metadata.model_;
                    entry.prompts_ = metadata.prompts_;
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

    ImageListResponse query(
        const Path& dir_path,
        const std::string& query_text,
        const bool recursive
    ) const {
        const auto current = load_snapshot();
        ImageListResponse response;
        const auto dir = sung::tostr(dir_path.lexically_normal());

        if (dir.empty() || dir == ".") {
            for (const auto& namespace_name : current->namespaces_)
                response.add_dir(namespace_name, sung::fromstr(namespace_name));
            response.sort();
            return response;
        }

        const Query query{ query_text };
        for (const auto& file : current->files_) {
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
                !query.matches_metadata(file.model_, file.prompts_)) {
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
                response.add_dir(folder.name_, sung::fromstr(folder.path_));
        }
        return response;
    }

    void remove_api_path(const std::string_view api_path) {
        std::lock_guard refresh_lock{ refresh_mutex_ };
        const auto current = load_snapshot();
        auto next = std::make_shared<IndexSnapshot>(*current);
        std::erase_if(next->files_, [&](const auto& file) {
            return sung::tostr(file.info_.path_) == api_path;
        });
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
    std::shared_ptr<const IndexSnapshot> snapshot_;
    std::mutex refresh_mutex_;
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
        if (auto_refresh_thread_.joinable())
            auto_refresh_thread_.join();
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
        std::function<std::shared_ptr<const ServerConfigs>()>
            configs_provider,
        const double interval_seconds
    ) {
        auto_refresh_thread_ = std::thread([this,
                                             configs_provider = std::move(
                                                 configs_provider
                                             ),
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
        });
    }

    ImageListResponse ImageIndex::query(
        const Path& dir, const std::string& query, const bool recursive
    ) const {
        return impl_->query(dir, query, recursive);
    }

    void ImageIndex::remove_api_path(const std::string_view api_path) {
        impl_->remove_api_path(api_path);
    }

}  // namespace sung
