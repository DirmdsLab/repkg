#pragma once

#include "io.hpp"

#include <limits>
#include <nlohmann/json_fwd.hpp>
#include <vector>

namespace repkg {

inline constexpr std::size_t default_texture_byte_limit = 250'000'000;
inline constexpr std::size_t high_texture_byte_limit = std::numeric_limits<std::int32_t>::max();

enum class TextureFormat : std::int32_t {
    rgba8888 = 0,
    dxt5 = 4,
    dxt3 = 6,
    dxt1 = 7,
    rg88 = 8,
    r8 = 9,
};

struct Mipmap {
    int width{};
    int height{};
    bool lz4{};
    std::size_t decoded_size{};
    std::uint64_t offset{};
    std::size_t stored_size{};
};

struct Frame {
    std::size_t image{};
    float seconds{};
    double x{};
    double y{};
    double width{};
    double width_y{};
    double height_x{};
    double height{};
};

struct Texture {
    TextureFormat format{};
    std::uint32_t flags{};
    int texture_width{};
    int texture_height{};
    int image_width{};
    int image_height{};
    std::uint32_t reserved{};
    std::string container_magic;
    int embedded_format{-1};
    std::vector<std::vector<Mipmap>> images;
    std::string frame_magic;
    int gif_width{};
    int gif_height{};
    std::vector<Frame> frames;

    bool animated() const { return (flags & 4U) != 0; }
    bool video() const { return embedded_format == 35 || (flags & 32U) != 0; }
};

Texture read_texture(BinaryReader reader, std::size_t byte_limit);
std::string_view output_extension(const Texture& texture);
nlohmann::ordered_json texture_sidecar(const Texture& texture);
nlohmann::ordered_json describe_texture(const Texture& texture);

void write_texture_image(InputFile& input, const Texture& texture, std::FILE* output, std::size_t byte_limit);

}  // namespace repkg
