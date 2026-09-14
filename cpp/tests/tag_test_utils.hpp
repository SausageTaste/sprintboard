#pragma once

#include <sqlite3.h>

#include "index/image_index.hpp"

namespace tag_test {
    inline sung::Path database_path(const sung::Path& root) {
        return root / ".sprintboard" / "image-index.sqlite3";
    }
    inline bool seed(
        const sung::Path& database, const sung::TagAnalysisRecord& record
    ) {
        const auto root = sung::fs::weakly_canonical(
            database.parent_path().parent_path()
        );
        const auto relative = [&](const std::string& path) {
            return path.empty()
                       ? std::string{}
                       : sung::tostr(
                             sung::fs::weakly_canonical(sung::fromstr(path))
                                 .lexically_relative(root)
                         );
        };
        nlohmann::json values = {
            { "logical_path", relative(record.logical_path_) },
            { "input_kind", record.input_kind_ },
            { "input_path", relative(record.input_path_) },
            { "input_size", record.input_size_ },
            { "input_modified_time", record.input_modified_time_ },
            { "input_sha256", record.input_sha256_ },
            { "analysis_id", record.analysis_id_ },
            { "analyzer_fingerprint", record.analyzer_fingerprint_ },
            { "model_id", record.model_id_ },
            { "general_threshold", record.general_threshold_ },
            { "character_threshold", record.character_threshold_ },
            { "analysis_json", record.analysis_.dump() },
            { "analyzed_at", record.analyzed_at_ },
            { "attempt_input_path", relative(record.attempt_input_path_) },
            { "attempt_input_size", record.attempt_input_size_ },
            { "attempt_input_modified_time",
              record.attempt_input_modified_time_ },
            { "attempt_analyzer_fingerprint",
              record.attempt_analyzer_fingerprint_ },
            { "last_attempt_at", record.last_attempt_at_ },
            { "failure_count", record.failure_count_ },
            { "last_error", record.last_error_ },
            { "proxy_path", relative(record.proxy_path_) },
            { "proxy_size", record.proxy_size_ },
            { "proxy_modified_time", record.proxy_modified_time_ },
            { "proxy_sha256", record.proxy_sha256_ },
            { "proxy_materialization_id", record.proxy_materialization_id_ }
        };
        std::string columns, params;
        for (const auto& [key, value] : values.items()) {
            if (!columns.empty()) {
                columns += ',';
                params += ',';
            }
            columns += key;
            params += '?';
        }
        const auto sql = "INSERT OR REPLACE INTO image_tag_analysis (" +
                         columns + ") VALUES (" + params + ");";
        sqlite3* db = nullptr;
        if (sqlite3_open(sung::tostr(database).c_str(), &db) != SQLITE_OK) {
            if (db)
                sqlite3_close(db);
            return false;
        }
        sqlite3_stmt* stmt = nullptr;
        bool success = sqlite3_prepare_v2(
                           db, sql.c_str(), -1, &stmt, nullptr
                       ) == SQLITE_OK;
        if (success) {
            int column = 1;
            for (const auto& [key, value] : values.items()) {
                if (value.is_string())
                    sqlite3_bind_text(
                        stmt,
                        column,
                        value.get_ref<const std::string&>().c_str(),
                        -1,
                        SQLITE_TRANSIENT
                    );
                else if (value.is_number_integer())
                    sqlite3_bind_int64(stmt, column, value.get<int64_t>());
                else
                    sqlite3_bind_double(stmt, column, value.get<double>());
                ++column;
            }
            success = sqlite3_step(stmt) == SQLITE_DONE;
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return success;
    }
    inline sung::TagAnalysisRecord analysis(
        const sung::Path& image,
        const std::string& tag = "sqlite_tag",
        int64_t timestamp = 123
    ) {
        sung::TagAnalysisRecord result;
        result.logical_path_ = sung::detail::logical_image_key(image);
        result.input_path_ = sung::tostr(sung::fs::weakly_canonical(image));
        result.input_kind_ = "source";
        const auto fingerprint =
            sung::fingerprint_file_with_sha256(image).value();
        result.input_size_ = fingerprint.size_;
        result.input_modified_time_ = fingerprint.modified_time_;
        result.input_sha256_ = fingerprint.sha256_;
        result.analyzer_fingerprint_ = "sqlite-analyzer";
        result.model_id_ = "sqlite-model";
        result.general_threshold_ = .35;
        result.character_threshold_ = .75;
        result.analyzed_at_ = timestamp;
        result.analysis_ = {
            { "ratings",
              nlohmann::json::array(
                  { { { "name", "safe" }, { "confidence", .9 } } }
              ) },
            { "generalTags",
              nlohmann::json::array(
                  { { { "name", tag }, { "confidence", .8 } } }
              ) },
            { "characterTags", nlohmann::json::array() }
        };
        result.searchable_tags_ = { tag };
        result.analysis_id_ = sung::make_analysis_id(result);
        return result;
    }
}  // namespace tag_test
