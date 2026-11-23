#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace repkg {

namespace fs = std::filesystem;

class BinaryReader;

class InputFile {
public:
    explicit InputFile(const fs::path& path);
    BinaryReader reader();
    BinaryReader slice(std::uint64_t offset, std::uint64_t size);

private:
    std::ifstream stream_;
    std::uint64_t size_{};
};

class BinaryReader {
public:
    std::uint64_t position() const { return position_; }
    std::uint64_t remaining() const { return end_ - position_; }
    void read(std::span<std::uint8_t> bytes);
    void skip(std::uint64_t bytes);
    std::uint32_t u32();
    std::int32_t i32() { return std::bit_cast<std::int32_t>(u32()); }
    float f32() { return std::bit_cast<float>(u32()); }
    std::size_t count(std::size_t limit, std::string_view field);
    std::string sized_string(std::size_t limit, std::string_view field);
    std::string cstring(std::size_t limit);

private:
    friend class InputFile;
    BinaryReader(std::ifstream& stream, std::uint64_t start, std::uint64_t length);
    void require(std::uint64_t bytes) const;

    std::ifstream& stream_;
    std::uint64_t position_;
    std::uint64_t end_;
};

std::string lower_ascii(std::string text);
std::string path_with_slashes(std::string text);
fs::path package_path(std::string name);
std::string safe_project_name(std::string text);
void write_bytes(std::FILE* file, std::span<const std::uint8_t> bytes);
void write_text(std::FILE* file, std::string_view text);
void copy_bytes(BinaryReader reader, std::FILE* output);

class OutputTree {
public:
    explicit OutputTree(fs::path root, bool overwrite);

    bool write(const fs::path& relative, const std::function<void(std::FILE*)>& writer) const;
    bool copy(const fs::path& source, const fs::path& relative) const;
    const fs::path& root() const { return root_; }

private:
    fs::path root_;
    bool overwrite_;
};

}  // namespace repkg
