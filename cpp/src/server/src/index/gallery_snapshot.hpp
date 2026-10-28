#pragma once

#include <map>
#include <memory>
#include <print>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include "response/img_list.hpp"
#include "sung/auxiliary/filesys.hpp"
#include "sung/auxiliary/server_configs.hpp"
#include "tag_analysis.hpp"

namespace sung::gallery_cache {

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
        std::unordered_map<std::string, TagAnalysisRecord> tag_analyses_;
    };


    // This is a disposable browsing cache, never an authority for tag or
    // encoder work. Loading it must not touch individual library paths.
    inline Path snapshot_path(const ServerConfigs& configs) {
        return fromstr(configs.cache_dir_) / "gallery-snapshot.json";
    }

    inline nlohmann::json bindings(const ServerConfigs& configs) {
        auto result = nlohmann::json::object();
        for (const auto& [name, binding] : configs.dir_bindings_)
            result[name] = tostr(
                fs::absolute(binding.local_dir_).lexically_normal()
            );
        return result;
    }

    inline bool safe_relative(const std::string& value) {
        const auto path = fromstr(value);
        if (value.empty() || path.has_root_path() ||
            value.find('\\') != std::string::npos ||
            value.find(':') != std::string::npos ||
            value.find('\0') != std::string::npos)
            return false;
        for (const auto& part : path)
            if (part == ".." || part == ".")
                return false;
        return true;
    }

    inline nlohmann::json encode(
        const ServerConfigs& configs, const IndexSnapshot& snapshot
    ) {
        auto files = nlohmann::json::array();
        for (const auto& file : snapshot.files_) {
            files.push_back(
                { { "root", file.root_key_ },
                  { "physical", file.physical_path_ },
                  { "browser", file.browser_path_ },
                  { "name", file.info_.name_ },
                  { "width", file.info_.width_ },
                  { "height", file.info_.height_ },
                  { "time", file.info_.sort_time_ns_ },
                  { "model", file.model_ },
                  { "prompts", file.prompts_ },
                  { "tags", file.tags_ } }
            );
        }
        auto folders = nlohmann::json::array();
        for (const auto& folder : snapshot.folders_)
            folders.push_back(
                { { "root", folder.root_key_ },
                  { "path", folder.path_ },
                  { "time", folder.sort_time_ns_ } }
            );
        return { { "version", 1 },
                 { "bindings", bindings(configs) },
                 { "files", files },
                 { "folders", folders },
                 { "namespaceTimes", snapshot.namespace_sort_times_ } };
    }

    inline std::shared_ptr<IndexSnapshot> decode(
        const ServerConfigs& configs, const nlohmann::json& json
    ) {
        if (json.at("version").get<int>() != 1 ||
            json.at("bindings") != bindings(configs))
            throw std::runtime_error("incompatible gallery snapshot");
        if (!json.at("files").is_array() || !json.at("folders").is_array() ||
            !json.at("namespaceTimes").is_object())
            throw std::runtime_error("invalid gallery snapshot collections");
        auto result = std::make_shared<IndexSnapshot>();
        // Canonicalize each configured root once, never each cached file.
        std::map<std::string, Path> roots;
        for (const auto& [name, binding] : configs.dir_bindings_) {
            std::error_code error;
            auto root = fs::weakly_canonical(
                fs::absolute(binding.local_dir_), error
            );
            if (error)
                root = fs::absolute(binding.local_dir_).lexically_normal();
            while (root.has_relative_path() && root.filename().empty())
                root = root.parent_path();
            roots.emplace(name + '\n' + tostr(root), root);
            result->namespaces_.insert(name);
        }
        const auto validate = [&](const std::string& key,
                                  const std::string& browser) {
            if (!roots.contains(key) || !safe_relative(browser))
                throw std::runtime_error("invalid gallery path");
            const auto name = key.substr(0, key.find('\n'));
            const auto path = fromstr(browser);
            const auto relative = path.lexically_relative(fromstr(name));
            if (relative.empty() || !safe_relative(tostr(relative)))
                throw std::runtime_error("gallery path outside namespace");
            return relative;
        };
        std::set<std::string> seen_files, seen_folders;
        for (const auto& item : json.at("files")) {
            IndexedFile file;
            file.root_key_ = item.at("root").get<std::string>();
            file.browser_path_ = item.at("browser").get<std::string>();
            const auto relative = validate(file.root_key_, file.browser_path_);
            file.physical_path_ = item.at("physical").get<std::string>();
            if (file.physical_path_ !=
                    tostr(roots.at(file.root_key_) / relative) ||
                !seen_files.insert(file.browser_path_).second)
                throw std::runtime_error("invalid gallery file");
            file.parent_browser_path_ = tostr(
                fromstr(file.browser_path_).parent_path()
            );
            file.info_.path_ = Path{ "/img" } / fromstr(file.browser_path_);
            file.info_.name_ = item.at("name").get<std::string>();
            file.info_.width_ = item.at("width").get<int>();
            file.info_.height_ = item.at("height").get<int>();
            if (!safe_relative(file.info_.name_) ||
                fromstr(file.info_.name_).has_parent_path() ||
                file.info_.width_ <= 0 || file.info_.height_ <= 0)
                throw std::runtime_error("invalid gallery image metadata");
            file.info_.sort_time_ns_ = item.at("time").get<int64_t>();
            file.model_ = item.at("model").get<std::string>();
            file.prompts_ = item.at("prompts").get<std::vector<std::string>>();
            file.tags_ = item.at("tags").get<std::vector<std::string>>();
            // Work fields intentionally stay empty until filesystem validation.
            result->files_.push_back(std::move(file));
        }
        for (const auto& item : json.at("folders")) {
            IndexedFolder folder;
            folder.root_key_ = item.at("root").get<std::string>();
            folder.path_ = item.at("path").get<std::string>();
            validate(folder.root_key_, folder.path_);
            if (!seen_folders.insert(folder.path_).second)
                throw std::runtime_error("duplicate gallery folder");
            folder.name_ = tostr(fromstr(folder.path_).filename());
            folder.parent_path_ = tostr(fromstr(folder.path_).parent_path());
            folder.sort_time_ns_ = item.at("time").get<int64_t>();
            result->folders_.push_back(std::move(folder));
        }
        result->namespace_sort_times_ =
            json.at("namespaceTimes")
                .get<std::unordered_map<std::string, int64_t>>();
        for (const auto& [name, time] : result->namespace_sort_times_)
            if (!result->namespaces_.contains(name))
                throw std::runtime_error("invalid gallery namespace");
        return result;
    }

    inline std::shared_ptr<IndexSnapshot> load(const ServerConfigs& configs) {
        try {
            std::string contents;
            if (read_file(snapshot_path(configs), contents))
                return decode(configs, nlohmann::json::parse(contents));
        } catch (const std::exception& error) {
            std::println(
                "ImageIndex: Ignoring gallery snapshot: {}", error.what()
            );
        }
        auto empty = std::make_shared<IndexSnapshot>();
        for (const auto& [name, binding] : configs.dir_bindings_)
            empty->namespaces_.insert(name);
        return empty;
    }

    inline void save(
        const ServerConfigs& configs, const IndexSnapshot& snapshot
    ) {
        try {
            const auto path = snapshot_path(configs);
            fs::create_directories(path.parent_path());
            const auto contents = encode(configs, snapshot).dump();
            std::error_code error;
            if (!write_file_atomically(path, contents, error))
                throw std::system_error(error);
        } catch (const std::exception& error) {
            std::println(
                "ImageIndex: Cannot save gallery snapshot: {}", error.what()
            );
        }
    }

}  // namespace sung::gallery_cache
