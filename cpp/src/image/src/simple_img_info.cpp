#include "sung/image/simple_img_info.hpp"

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

    bool SimpleImageInfo::is_png() const {
        return mime_type_ == std::string_view("image/png");
    }

    bool SimpleImageInfo::is_avif() const {
        return mime_type_ == std::string_view("image/avif");
    }


    bool get_simple_img_info(const Path& file_path, SimpleImageInfo& out) {
        const auto info = imageinfo::parse<::FileReader>(file_path);
        if (!info.ok())
            return false;

        out.mime_type_ = info.mimetype();
        out.width_ = info.size().width;
        out.height_ = info.size().height;
        return true;
    }

    std::optional<SimpleImageInfo> get_simple_img_info(const Path& file_path) {
        SimpleImageInfo result;
        if (get_simple_img_info(file_path, result))
            return result;
        else
            return std::nullopt;
    }

}  // namespace sung
