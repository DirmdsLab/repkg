#include "texture.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace repkg {
namespace {

using Json = nlohmann::ordered_json;

constexpr std::size_t max_images = 100;
constexpr std::size_t max_mipmaps = 32;
constexpr std::size_t max_frames = 100'000;

struct EmbeddedFormat {
    std::string_view name;
    std::string_view extension;
};

constexpr std::array<EmbeddedFormat, 36> embedded_formats{{
    {"BMP", "bmp"}, {"ICO", "ico"}, {"JPEG", "jpg"}, {"JNG", "jng"},
    {"KOALA", "koa"}, {"LBM", "lbm"}, {"MNG", "mng"}, {"PBM", "pbm"},
    {"PBMRAW", "pbm"}, {"PCD", "pcd"}, {"PCX", "pcx"}, {"PGM", "pgm"},
    {"PGMRAW", "pgm"}, {"PNG", "png"}, {"PPM", "ppm"}, {"PPMRAW", "ppm"},
    {"RAS", "ras"}, {"TARGA", "tga"}, {"TIFF", "tif"}, {"WBMP", "wbmp"},
    {"PSD", "psd"}, {"CUT", "cut"}, {"XBM", "xbm"}, {"XPM", "xpm"},
    {"DDS", "dds"}, {"GIF", "gif"}, {"HDR", "hdr"}, {"FAXG3", "g3"},
    {"SGI", "sgi"}, {"EXR", "exr"}, {"J2K", "j2k"}, {"JP2", "jp2"},
    {"PFM", "pfm"}, {"PICT", "pict"}, {"RAW", "raw"}, {"MP4", "mp4"},
}};

std::string_view format_name(TextureFormat format)
{
    switch (format) {
        case TextureFormat::rgba8888: return "rgba8888";
        case TextureFormat::dxt5: return "dxt5";
        case TextureFormat::dxt3: return "dxt3";
        case TextureFormat::dxt1: return "dxt1";
        case TextureFormat::rg88: return "rg88";
        case TextureFormat::r8: return "r8";
    }
    throw std::runtime_error("Unsupported TEX format " + std::to_string(static_cast<int>(format)));
}

void expect_magic(BinaryReader& reader, std::string_view expected)
{
    const auto actual = reader.cstring(16);
    if (actual != expected) {
        throw std::runtime_error("Expected " + std::string(expected) + ", got " + actual);
    }
}

Mipmap read_mipmap(BinaryReader& reader, int version, std::size_t byte_limit)
{
    if (version == 4) {
        if (reader.i32() != 1 || reader.i32() != 2) {
            throw std::runtime_error("Unsupported TEXB0004 video header");
        }
        reader.cstring(1024 * 1024);
        if (reader.i32() != 1) {
            throw std::runtime_error("Unsupported TEXB0004 video condition");
        }
    }

    Mipmap mipmap;
    mipmap.width = reader.i32();
    mipmap.height = reader.i32();
    if (version >= 2) {
        const auto compression = reader.i32();
        if (compression != 0 && compression != 1) {
            throw std::runtime_error("Unsupported mipmap compression " + std::to_string(compression));
        }
        mipmap.lz4 = compression == 1;
        const auto decoded_size = reader.i32();
        if (mipmap.lz4 && (decoded_size <= 0 || static_cast<std::size_t>(decoded_size) > byte_limit)) {
            throw std::runtime_error("Invalid decompressed mipmap size");
        }
        if (decoded_size > 0) {
            mipmap.decoded_size = static_cast<std::size_t>(decoded_size);
        }
    }
    mipmap.stored_size = reader.count(byte_limit, "mipmap size");
    if (mipmap.lz4 && mipmap.stored_size == 0) {
        throw std::runtime_error("Empty LZ4 mipmap");
    }
    mipmap.offset = reader.position();
    reader.skip(mipmap.stored_size);
    return mipmap;
}

double read_frame_component(BinaryReader& reader, bool integer_components)
{
    if (integer_components) {
        return reader.i32();
    }
    const double value = reader.f32();
    if (!std::isfinite(value)) {
        throw std::runtime_error("Invalid TEX animation coordinates");
    }
    return value;
}

int first_frame_dimension(double value)
{
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
        throw std::runtime_error("Invalid TEX animation dimensions");
    }
    return static_cast<int>(value);
}

void read_frames(BinaryReader& reader, Texture& texture)
{
    texture.frame_magic = reader.cstring(16);
    const bool integer_components = texture.frame_magic == "TEXS0001";
    const bool has_canvas = texture.frame_magic == "TEXS0003";
    if (!integer_components && !has_canvas && texture.frame_magic != "TEXS0002") {
        throw std::runtime_error("Unsupported TEX frame header " + texture.frame_magic);
    }
    const auto count = reader.count(max_frames, "frame count");
    if (count == 0) {
        throw std::runtime_error("TEX animation contains no frames");
    }
    if (has_canvas) {
        texture.gif_width = reader.i32();
        texture.gif_height = reader.i32();
    }
    texture.frames.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        Frame frame;
        frame.image = reader.count(texture.images.size() - 1, "frame image index");
        frame.seconds = reader.f32();
        if (!std::isfinite(frame.seconds) || frame.seconds < 0) {
            throw std::runtime_error("Invalid TEX animation frame duration");
        }
        frame.x = read_frame_component(reader, integer_components);
        frame.y = read_frame_component(reader, integer_components);
        frame.width = read_frame_component(reader, integer_components);
        frame.width_y = read_frame_component(reader, integer_components);
        frame.height_x = read_frame_component(reader, integer_components);
        frame.height = read_frame_component(reader, integer_components);
        texture.frames.push_back(frame);
    }
    if (texture.gif_width == 0 || texture.gif_height == 0) {
        const auto& first = texture.frames.front();
        // The two older frame tables store the canvas size implicitly.
        texture.gif_width = first_frame_dimension(std::hypot(first.width, first.width_y));
        texture.gif_height = first_frame_dimension(std::hypot(first.height_x, first.height));
    }
}

std::string as_text(bool value)
{
    return value ? "true" : "false";
}

bool is_power_of_two(int value)
{
    return value > 0 && (value & (value - 1)) == 0;
}

}  // namespace

Texture read_texture(BinaryReader reader, std::size_t byte_limit)
{
    expect_magic(reader, "TEXV0005");
    expect_magic(reader, "TEXI0001");

    Texture texture;
    texture.format = static_cast<TextureFormat>(reader.i32());
    format_name(texture.format);
    texture.flags = reader.u32();
    texture.texture_width = reader.i32();
    texture.texture_height = reader.i32();
    texture.image_width = reader.i32();
    texture.image_height = reader.i32();
    texture.reserved = reader.u32();
    texture.container_magic = reader.cstring(16);
    const auto image_count = reader.count(max_images, "image count");
    if (image_count == 0) {
        throw std::runtime_error("TEX contains no images");
    }

    int version = 0;
    if (texture.container_magic == "TEXB0001") {
        version = 1;
    } else if (texture.container_magic == "TEXB0002") {
        version = 2;
    } else if (texture.container_magic == "TEXB0003") {
        version = 3;
        texture.embedded_format = reader.i32();
    } else if (texture.container_magic == "TEXB0004") {
        texture.embedded_format = reader.i32();
        const bool video_payload = reader.i32() == 1;
        if (texture.embedded_format == -1 && video_payload) {
            texture.embedded_format = 35;
        }
        version = texture.embedded_format == 35 ? 4 : 3;
    } else {
        throw std::runtime_error("Unsupported TEX image header " + texture.container_magic);
    }
    if (texture.embedded_format < -1 || texture.embedded_format >= static_cast<int>(embedded_formats.size())) {
        throw std::runtime_error("Unsupported embedded format " + std::to_string(texture.embedded_format));
    }

    texture.images.reserve(image_count);
    for (std::size_t index = 0; index < image_count; ++index) {
        const auto mipmap_count = reader.count(max_mipmaps, "mipmap count");
        if (mipmap_count == 0) {
            throw std::runtime_error("TEX image contains no mipmaps");
        }
        std::vector<Mipmap> mipmaps;
        mipmaps.reserve(mipmap_count);
        for (std::size_t level = 0; level < mipmap_count; ++level) {
            mipmaps.push_back(read_mipmap(reader, version, byte_limit));
        }
        texture.images.push_back(std::move(mipmaps));
    }

    if (texture.animated()) {
        read_frames(reader, texture);
    }
    return texture;
}

std::string_view output_extension(const Texture& texture)
{
    if (texture.animated()) {
        return "gif";
    }
    if (texture.video()) {
        return "mp4";
    }
    if (texture.embedded_format >= 0) {
        return embedded_formats[texture.embedded_format].extension;
    }
    return "png";
}

nlohmann::ordered_json texture_sidecar(const Texture& texture)
{
    Json info{
        {"bleedtransparentcolors", true},
        {"clampuvs", (texture.flags & 2U) != 0},
        {"format", format_name(texture.format)},
        {"nomip", as_text(texture.images.front().size() == 1)},
        {"nointerpolation", as_text((texture.flags & 1U) != 0)},
        {"nonpoweroftwo", as_text(!is_power_of_two(texture.image_width) || !is_power_of_two(texture.image_height))},
    };
    if (texture.animated()) {
        info["spritesheetsequences"] = Json::array({
            {{"duration", 1}, {"frames", texture.frames.size()},
             {"width", texture.gif_width}, {"height", texture.gif_height}},
        });
    }
    return info;
}

nlohmann::ordered_json describe_texture(const Texture& texture)
{
    Json info{
        {"format", format_name(texture.format)},
        {"flags", texture.flags},
        {"texture_width", texture.texture_width},
        {"texture_height", texture.texture_height},
        {"image_width", texture.image_width},
        {"image_height", texture.image_height},
        {"reserved", texture.reserved},
        {"container", texture.container_magic},
        {"output_extension", output_extension(texture)},
        {"images", Json::array()},
    };
    if (texture.embedded_format >= 0) {
        info["embedded_format"] = embedded_formats[texture.embedded_format].name;
    }
    for (const auto& image : texture.images) {
        Json mipmaps = Json::array();
        for (const auto& mipmap : image) {
            mipmaps.push_back({
                {"width", mipmap.width}, {"height", mipmap.height}, {"lz4", mipmap.lz4},
                {"stored_size", mipmap.stored_size}, {"decoded_size", mipmap.decoded_size},
            });
        }
        info["images"].push_back(std::move(mipmaps));
    }
    if (texture.animated()) {
        info["animation"] = {
            {"container", texture.frame_magic}, {"frames", texture.frames.size()},
            {"width", texture.gif_width}, {"height", texture.gif_height},
        };
    }
    return info;
}

}  // namespace repkg
