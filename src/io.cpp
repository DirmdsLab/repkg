#include "io.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace repkg {
namespace {

[[noreturn]] void system_failure(const std::string& operation)
{
    throw std::system_error(errno, std::generic_category(), operation);
}

class FileDescriptor {
public:
    explicit FileDescriptor(int value) : value_(value) {}
    ~FileDescriptor() { if (value_ >= 0) ::close(value_); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept
        : value_(std::exchange(other.value_, -1)) {}

    FileDescriptor& operator=(FileDescriptor&& other) noexcept
    {
        std::swap(value_, other.value_);
        return *this;
    }

    int get() const { return value_; }
    int release() { return std::exchange(value_, -1); }

private:
    int value_;
};

FileDescriptor open_child_directory(int parent, const fs::path& name)
{
    if (::mkdirat(parent, name.c_str(), 0777) != 0 && errno != EEXIST) {
        system_failure("Cannot create output directory " + name.string());
    }
    FileDescriptor child(::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (child.get() < 0) {
        system_failure("Cannot open output directory " + name.string());
    }
    return child;
}

bool regular_output_exists(int directory, const fs::path& target)
{
    struct stat status {};
    if (::fstatat(directory, target.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(status.st_mode)) {
            throw std::runtime_error("Output path is not a regular file: " + target.string());
        }
        return true;
    }
    if (errno == ENOENT) {
        return false;
    }
    system_failure("Cannot inspect output path " + target.string());
}

class TemporaryOutput {
public:
    TemporaryOutput(int directory, const fs::path& target) : directory_(directory)
    {
        static std::atomic_uint64_t sequence{0};
        for (int attempt = 0; attempt < 128; ++attempt) {
            name_ = ".repkg-" + std::to_string(::getpid()) + "-" + std::to_string(sequence++);
            if (name_ == target.native()) {
                continue;
            }
            FileDescriptor handle(::openat(directory_, name_.c_str(),
                                           O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666));
            if (handle.get() >= 0) {
                file_.reset(::fdopen(handle.get(), "wb"));
                if (!file_) {
                    const int error = errno;
                    ::unlinkat(directory_, name_.c_str(), 0);
                    errno = error;
                    system_failure("Cannot open temporary output stream");
                }
                handle.release();
                return;
            }
            if (errno != EEXIST) {
                system_failure("Cannot create temporary output file");
            }
        }
        throw std::runtime_error("Cannot allocate a unique temporary output name");
    }

    ~TemporaryOutput()
    {
        if (!name_.empty()) {
            ::unlinkat(directory_, name_.c_str(), 0);
        }
    }

    std::FILE* file() const { return file_.get(); }

    bool commit(const fs::path& target, bool overwrite)
    {
        if (std::fclose(file_.release()) != 0) {
            system_failure("Cannot close output file");
        }
        if (overwrite) {
            if (::renameat(directory_, name_.c_str(), directory_, target.c_str()) != 0) {
                system_failure("Cannot replace " + target.string());
            }
            name_.clear();
            return true;
        }
        for (;;) {
            if (::linkat(directory_, name_.c_str(), directory_, target.c_str(), 0) == 0) {
                return true;
            }
            if (errno != EEXIST) {
                system_failure("Cannot publish " + target.string());
            }
            if (regular_output_exists(directory_, target)) {
                return false;
            }
        }
    }

private:
    struct CloseFile {
        void operator()(std::FILE* file) const noexcept { std::fclose(file); }
    };

    int directory_;
    std::string name_;
    std::unique_ptr<std::FILE, CloseFile> file_;
};

}  // namespace

InputFile::InputFile(const fs::path& path)
    : stream_(path, std::ios::binary | std::ios::ate)
{
    const std::streamoff end = stream_.tellg();
    if (end < 0) {
        throw std::runtime_error("Cannot open input file " + path.string());
    }
    size_ = static_cast<std::uint64_t>(end);
}

BinaryReader InputFile::reader()
{
    return slice(0, size_);
}

BinaryReader InputFile::slice(std::uint64_t offset, std::uint64_t size)
{
    if (offset > size_ || size > size_ - offset) {
        throw std::runtime_error("Input range exceeds file size");
    }
    return BinaryReader(stream_, offset, size);
}

BinaryReader::BinaryReader(std::ifstream& stream, std::uint64_t start, std::uint64_t length)
    : stream_(stream), position_(start), end_(start + length)
{
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(start));
    if (!stream_) {
        throw std::runtime_error("Cannot seek in input file");
    }
}

void BinaryReader::require(std::uint64_t bytes) const
{
    if (bytes > remaining()) {
        throw std::runtime_error("Unexpected end of input at byte " + std::to_string(position_));
    }
}

void BinaryReader::read(std::span<std::uint8_t> bytes)
{
    require(bytes.size());
    if (bytes.empty()) {
        return;
    }
    if (!stream_.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("Cannot read input at byte " + std::to_string(position_));
    }
    position_ += bytes.size();
}

void BinaryReader::skip(std::uint64_t bytes)
{
    require(bytes);
    if (!stream_.seekg(static_cast<std::streamoff>(bytes), std::ios::cur)) {
        throw std::runtime_error("Cannot skip bytes in input");
    }
    position_ += bytes;
}

std::uint32_t BinaryReader::u32()
{
    std::array<std::uint8_t, 4> bytes{};
    read(bytes);
    return std::uint32_t{bytes[0]} | (std::uint32_t{bytes[1]} << 8)
        | (std::uint32_t{bytes[2]} << 16) | (std::uint32_t{bytes[3]} << 24);
}

std::size_t BinaryReader::count(std::size_t limit, std::string_view field)
{
    const auto value = i32();
    if (value < 0 || static_cast<std::size_t>(value) > limit) {
        throw std::runtime_error("Invalid " + std::string(field) + " (" + std::to_string(value) + ")");
    }
    return static_cast<std::size_t>(value);
}

std::string BinaryReader::sized_string(std::size_t limit, std::string_view field)
{
    std::string value(count(limit, field), '\0');
    read({reinterpret_cast<std::uint8_t*>(value.data()), value.size()});
    return value;
}

std::string BinaryReader::cstring(std::size_t limit)
{
    std::string value;
    for (;;) {
        std::uint8_t ch{};
        read({&ch, 1});
        if (ch == 0) {
            return value;
        }
        if (value.size() == limit) {
            throw std::runtime_error("Input string exceeds its length limit");
        }
        value.push_back(static_cast<char>(ch));
    }
}

std::string lower_ascii(std::string text)
{
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return text;
}

std::string path_with_slashes(std::string text)
{
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

fs::path package_path(std::string name)
{
    name = path_with_slashes(std::move(name));
    const fs::path path(name);
    if (name.find('\0') != std::string::npos || path.is_absolute() || path.filename().empty()
        || (name.size() >= 2 && name[1] == ':')) {
        throw std::runtime_error("Invalid package path");
    }
    fs::path result;
    for (const auto& component : path) {
        if (component == "..") {
            throw std::runtime_error("Parent traversal in package path");
        }
        if (component != ".") {
            result /= component;
        }
    }
    if (result.empty() || path.filename() == ".") {
        throw std::runtime_error("Missing filename in package path");
    }
    return result;
}

std::string safe_project_name(std::string text)
{
    for (char& ch : text) {
        if (static_cast<unsigned char>(ch) < 32 || std::string_view("<>:\"/\\|?*").find(ch) != std::string_view::npos) {
            ch = '_';
        }
    }
    const auto begin = text.find_first_not_of(" .");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = text.find_last_not_of(" .");
    return text.substr(begin, end - begin + 1);
}

void write_bytes(std::FILE* file, std::span<const std::uint8_t> bytes)
{
    if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
        system_failure("Cannot write output bytes");
    }
}

void write_text(std::FILE* file, std::string_view text)
{
    write_bytes(file, {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

void copy_bytes(BinaryReader reader, std::FILE* output)
{
    std::array<std::uint8_t, 64 * 1024> buffer{};
    while (reader.remaining() != 0) {
        const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), reader.remaining()));
        auto block = std::span(buffer).first(size);
        reader.read(block);
        write_bytes(output, block);
    }
}

OutputTree::OutputTree(fs::path root, bool overwrite)
    : root_(std::move(root)), overwrite_(overwrite)
{
    fs::create_directories(root_);
}

bool OutputTree::write(const fs::path& relative, const std::function<void(std::FILE*)>& writer) const
{
    if (relative.is_absolute() || relative.filename().empty() || relative.filename() == "."
        || relative.filename() == "..") {
        throw std::runtime_error("Invalid relative output path");
    }
    FileDescriptor directory(::open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (directory.get() < 0) {
        system_failure("Cannot open output root " + root_.string());
    }
    for (const auto& component : relative.parent_path()) {
        if (component == "..") {
            throw std::runtime_error("Invalid parent directory in output path");
        }
        if (component != ".") {
            directory = open_child_directory(directory.get(), component);
        }
    }

    if (regular_output_exists(directory.get(), relative.filename()) && !overwrite_) {
        return false;
    }

    TemporaryOutput pending(directory.get(), relative.filename());
    writer(pending.file());
    return pending.commit(relative.filename(), overwrite_);
}

bool OutputTree::copy(const fs::path& source, const fs::path& relative) const
{
    std::error_code error;
    if (fs::equivalent(source, root_ / relative, error)) {
        return false;
    }
    return write(relative, [&source](std::FILE* output) {
        InputFile input(source);
        copy_bytes(input.reader(), output);
    });
}

}  // namespace repkg
