#pragma once

#include "index/image_index.hpp"

namespace sung {

    struct ImageIndexTestAccess {
        static void refresh_new_folder(
            ImageIndex& index, std::shared_ptr<const ServerConfigs> configs
        ) {
            index.refresh_new_folder(std::move(configs));
        }

        static std::unique_ptr<ImageIndex> make(
            std::function<void(std::string_view)> observer
        ) {
            return std::unique_ptr<ImageIndex>{
                new ImageIndex(std::move(observer))
            };
        }
    };

}  // namespace sung
