#include "util/simple_img_info.hpp"

#include <imageinfo.hpp>


namespace {

    class FileReader {

    public:
        explicit FileReader(const sung::Path& path)
            : file_(path, std::ios::in | std::ios::binary) {}

        ~FileReader() {
            if (file_.is_open()) {
                file_.close();
            }
        }

        size_t size() {
            if (file_.is_open()) {
                file_.seekg(0, std::ios::end);
                return (size_t)file_.tellg();
            } else {
                return 0;
            }
        }

        void read(void* buf, off_t offset, size_t size) {
            file_.seekg(offset, std::ios::beg);
            file_.read((char*)buf, (std::streamsize)size);
        }

    private:
        std::ifstream file_;
    };

}  // namespace


namespace sung {

    std::optional<SimpleImageInfo> get_simple_img_info(const Path& file_path) {
        const auto info = imageinfo::parse<::FileReader>(file_path);
        if (!info.ok())
            return std::nullopt;

        SimpleImageInfo result;
        result.mime_type_ = info.mimetype();
        result.width_ = info.size().width;
        result.height_ = info.size().height;
        return result;
    }

}  // namespace sung
