#include "texture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gif_lib.h>
#include <lz4.h>
#include <png.h>

namespace repkg {
namespace {

using Bytes = std::vector<std::uint8_t>;
using Pixel = std::array<std::uint8_t, 4>;

std::size_t pixel_count(int width, int height, std::size_t byte_limit)
{
    if (width <= 0 || height <= 0 ||
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) > byte_limit / 4) {
        throw std::runtime_error("Invalid or oversized raster dimensions");
    }
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
}

struct Raster {
    std::size_t offset(int x, int y) const
    {
        return 4 * (static_cast<std::size_t>(y) * width + x);
    }

    int width;
    int height;
    Bytes pixels;
};

Bytes read_payload(InputFile& input, const Mipmap& mipmap)
{
    Bytes stored(mipmap.stored_size);
    input.slice(mipmap.offset, mipmap.stored_size).read(stored);
    if (!mipmap.lz4) {
        return stored;
    }
    Bytes decoded(mipmap.decoded_size);
    const int size = LZ4_decompress_safe(
        reinterpret_cast<const char*>(stored.data()), reinterpret_cast<char*>(decoded.data()),
        static_cast<int>(stored.size()), static_cast<int>(decoded.size()));
    if (size != static_cast<int>(decoded.size())) {
        throw std::runtime_error("LZ4 data does not match the declared decoded size");
    }
    return decoded;
}

void require_mp4(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < 12 ||
        std::string_view(reinterpret_cast<const char*>(bytes.data() + 4), 4) != "ftyp") {
        throw std::runtime_error("Expected an MP4 file type box");
    }
}

void write_embedded_media(InputFile& input, const Mipmap& mipmap, std::FILE* output, bool video)
{
    if (mipmap.lz4) {
        const auto bytes = read_payload(input, mipmap);
        if (video) {
            require_mp4(bytes);
        }
        write_bytes(output, bytes);
        return;
    }

    auto reader = input.slice(mipmap.offset, mipmap.stored_size);
    if (video) {
        std::array<std::uint8_t, 12> header{};
        reader.read(header);
        require_mp4(header);
        write_bytes(output, header);
    }
    copy_bytes(reader, output);
}

int little_endian_16(const std::uint8_t* data)
{
    return data[0] | (data[1] << 8);
}

Pixel rgb565(int bits)
{
    const int r = (bits >> 11) & 31;
    const int g = (bits >> 5) & 63;
    const int b = bits & 31;
    return {static_cast<std::uint8_t>((r << 3) | (r >> 2)),
            static_cast<std::uint8_t>((g << 2) | (g >> 4)),
            static_cast<std::uint8_t>((b << 3) | (b >> 2)), 255};
}

std::array<Pixel, 4> color_table(const std::uint8_t* block, bool dxt1)
{
    const int first = little_endian_16(block);
    const int second = little_endian_16(block + 2);
    std::array<Pixel, 4> colors{rgb565(first), rgb565(second), Pixel{}, Pixel{}};
    const bool transparent = dxt1 && first <= second;
    for (int channel = 0; channel < 3; ++channel) {
        const int a = colors[0][channel];
        const int b = colors[1][channel];
        colors[2][channel] = static_cast<std::uint8_t>(transparent ? (a + b) / 2 : (2 * a + b) / 3);
        colors[3][channel] = static_cast<std::uint8_t>(transparent ? 0 : (a + 2 * b) / 3);
    }
    colors[2][3] = 255;
    colors[3][3] = transparent ? 0 : 255;
    return colors;
}

std::array<std::uint8_t, 16> alpha_values(const std::uint8_t* block, TextureFormat format)
{
    std::array<std::uint8_t, 16> alpha{};
    if (format == TextureFormat::dxt3) {
        for (int index = 0; index < 16; ++index) {
            alpha[index] = static_cast<std::uint8_t>(((block[index / 2] >> (4 * (index % 2))) & 15) * 17);
        }
    } else if (format == TextureFormat::dxt5) {
        std::array<int, 8> values{block[0], block[1]};
        const int divisor = values[0] > values[1] ? 7 : 5;
        for (int index = 1; index < divisor; ++index) {
            values[index + 1] = ((divisor - index) * values[0] + index * values[1]) / divisor;
        }
        if (divisor == 5) {
            values[7] = 255;
        }
        std::uint64_t selectors = 0;
        for (int index = 0; index < 6; ++index) {
            selectors |= std::uint64_t{block[index + 2]} << (8 * index);
        }
        for (int index = 0; index < 16; ++index) {
            alpha[index] = static_cast<std::uint8_t>(values[(selectors >> (3 * index)) & 7]);
        }
    }
    return alpha;
}

void decode_dxt(std::span<const std::uint8_t> data, TextureFormat format, Raster& raster)
{
    const bool dxt1 = format == TextureFormat::dxt1;
    const std::size_t block_size = dxt1 ? 8 : 16;
    const auto needed = static_cast<std::size_t>((raster.width + 3) / 4) * ((raster.height + 3) / 4);
    if (data.size() / block_size < needed) {
        throw std::runtime_error("Truncated DXT mipmap");
    }

    std::size_t offset = 0;
    for (int y = 0; y < raster.height; y += 4) {
        for (int x = 0; x < raster.width; x += 4, offset += block_size) {
            const auto* block = data.data() + offset;
            const auto* color_block = block + (dxt1 ? 0 : 8);
            const auto colors = color_table(color_block, dxt1);
            const auto alpha = alpha_values(block, format);
            for (int row = 0; row < 4 && y + row < raster.height; ++row) {
                for (int col = 0; col < 4 && x + col < raster.width; ++col) {
                    const auto selector = (color_block[4 + row] >> (2 * col)) & 3;
                    auto pixel = colors[selector];
                    if (!dxt1) {
                        pixel[3] = alpha[row * 4 + col];
                    }
                    std::copy(pixel.begin(), pixel.end(), raster.pixels.data() + raster.offset(x + col, y + row));
                }
            }
        }
    }
}

Raster decode_raster(InputFile& input, const Mipmap& mipmap, TextureFormat format, std::size_t byte_limit)
{
    const auto count = pixel_count(mipmap.width, mipmap.height, byte_limit);
    auto bytes = read_payload(input, mipmap);
    Raster raster{mipmap.width, mipmap.height, {}};
    switch (format) {
        case TextureFormat::rgba8888:
            if (bytes.size() < count * 4) {
                throw std::runtime_error("Truncated RGBA mipmap");
            }
            raster.pixels = std::move(bytes);
            break;
        case TextureFormat::r8:
        case TextureFormat::rg88: {
            const std::size_t stride = format == TextureFormat::r8 ? 1 : 2;
            if (bytes.size() / stride < count) {
                throw std::runtime_error("Truncated single- or dual-channel mipmap");
            }
            raster.pixels.resize(count * 4);
            for (std::size_t index = 0; index < count; ++index) {
                const auto gray = bytes[index * stride + stride - 1];
                const auto alpha = stride == 1 ? std::uint8_t{255} : bytes[index * stride];
                const Pixel pixel{gray, gray, gray, alpha};
                std::copy(pixel.begin(), pixel.end(), raster.pixels.data() + index * 4);
            }
            break;
        }
        case TextureFormat::dxt1:
        case TextureFormat::dxt3:
        case TextureFormat::dxt5:
            raster.pixels.resize(count * 4);
            decode_dxt(bytes, format, raster);
            break;
    }
    return raster;
}

void write_png(std::FILE* output, const Raster& raster, int width, int height)
{
    if (width <= 0 || height <= 0 || width > raster.width || height > raster.height) {
        throw std::runtime_error("TEX image dimensions exceed the source mipmap");
    }
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = static_cast<png_uint_32>(width);
    image.height = static_cast<png_uint_32>(height);
    image.format = PNG_FORMAT_RGBA;
    const bool written = png_image_write_to_stdio(
        &image, output, 0, raster.pixels.data(), raster.width * 4, nullptr) != 0;
    const std::string message(image.message);
    png_image_free(&image);
    if (!written) {
        throw std::runtime_error("PNG encoding failed: " + message);
    }
}

int gif_write(GifFileType* gif, const GifByteType* bytes, int count)
{
    auto* out = static_cast<std::FILE*>(gif->UserData);
    return static_cast<int>(std::fwrite(bytes, 1, static_cast<std::size_t>(count), out));
}

[[noreturn]] void gif_failure(int error)
{
    const auto* message = GifErrorString(error);
    throw std::runtime_error("GIF encoding failed: " + std::string(message ? message : "unknown error"));
}

void check_gif(GifFileType* gif, int result)
{
    if (result != GIF_OK) {
        gif_failure(gif->Error);
    }
}

void close_gif(GifFileType* gif)
{
    int error = 0;
    EGifCloseFile(gif, &error);
}

using GifMap = std::unique_ptr<ColorMapObject, decltype(&GifFreeMapObject)>;

GifMap make_gif_map(int size, const GifColorType* colors)
{
    GifMap map(GifMakeMapObject(size, colors), &GifFreeMapObject);
    if (!map) {
        throw std::runtime_error("Cannot allocate GIF palette");
    }
    return map;
}

int frame_coordinate(double value, int limit)
{
    if (value < 0 || value > limit) {
        throw std::runtime_error("GIF frame rectangle exceeds the source texture");
    }
    return static_cast<int>(value);
}

Raster make_gif_frame(const Texture& texture, const Frame& frame, const Raster& sheet, std::size_t canvas_pixels)
{
    const double signed_width = frame.width != 0 ? frame.width : frame.height_x;
    const double signed_height = frame.height != 0 ? frame.height : frame.width_y;
    const int left = frame_coordinate(frame.x + std::min(0.0, signed_width), sheet.width);
    const int top = frame_coordinate(frame.y + std::min(0.0, signed_height), sheet.height);
    const int width = frame_coordinate(std::abs(signed_width), sheet.width - left);
    const int height = frame_coordinate(std::abs(signed_height), sheet.height - top);
    const bool swap_axes = (signed_width < 0) != (signed_height < 0);
    const int rotated_width = swap_axes ? height : width;
    const int rotated_height = swap_axes ? width : height;
    if (width <= 0 || height <= 0 || rotated_width > texture.gif_width || rotated_height > texture.gif_height) {
        throw std::runtime_error("GIF frame dimensions exceed the animation canvas");
    }

    Raster canvas{texture.gif_width, texture.gif_height, Bytes(canvas_pixels * 4)};
    for (int y = 0; y < rotated_height; ++y) {
        for (int x = 0; x < rotated_width; ++x) {
            int source_x = x;
            int source_y = y;
            // Signed axes describe how the sprite was packed into the sheet.
            if (signed_width < 0 && signed_height < 0) {
                source_x = width - 1 - x;
                source_y = height - 1 - y;
            } else if (signed_width < 0) {
                source_x = width - 1 - y;
                source_y = x;
            } else if (signed_height < 0) {
                source_x = y;
                source_y = height - 1 - x;
            }
            const auto* source = sheet.pixels.data() + sheet.offset(left + source_x, top + source_y);
            auto* destination = canvas.pixels.data() + canvas.offset(x, y);
            std::copy_n(source, 4, destination);
        }
    }
    return canvas;
}

struct IndexedFrame {
    std::array<GifColorType, 256> palette{};
    std::vector<GifPixelType> pixels;
};

int quantize(std::uint8_t value, int steps)
{
    return (value * (steps - 1) + 127) / 255;
}

GifByteType palette_channel(int index, int steps)
{
    return static_cast<GifByteType>((index * 255 + (steps - 1) / 2) / (steps - 1));
}

void index_with_uniform_palette(const Raster& frame, IndexedFrame& indexed)
{
    constexpr int red_steps = 6;
    constexpr int green_steps = 7;
    constexpr int blue_steps = 6;
    for (int r = 0; r < red_steps; ++r) {
        for (int g = 0; g < green_steps; ++g) {
            for (int b = 0; b < blue_steps; ++b) {
                const int index = 1 + (r * green_steps + g) * blue_steps + b;
                indexed.palette[index] = {
                    palette_channel(r, red_steps), palette_channel(g, green_steps),
                    palette_channel(b, blue_steps),
                };
            }
        }
    }
    for (std::size_t index = 0; index < indexed.pixels.size(); ++index) {
        const auto* pixel = frame.pixels.data() + index * 4;
        if (pixel[3] < 128) {
            indexed.pixels[index] = 0;
            continue;
        }
        const int r = quantize(pixel[0], red_steps);
        const int g = quantize(pixel[1], green_steps);
        const int b = quantize(pixel[2], blue_steps);
        indexed.pixels[index] = static_cast<GifPixelType>(1 + (r * green_steps + g) * blue_steps + b);
    }
}

IndexedFrame index_frame(const Raster& frame)
{
    IndexedFrame indexed;
    indexed.pixels.resize(frame.pixels.size() / 4);
    std::unordered_map<std::uint32_t, GifPixelType> colors;
    colors.reserve(256);
    for (std::size_t index = 0; index < indexed.pixels.size(); ++index) {
        const auto* pixel = frame.pixels.data() + index * 4;
        if (pixel[3] < 128) {
            continue;
        }
        const auto key = (std::uint32_t{pixel[0]} << 16) | (std::uint32_t{pixel[1]} << 8) | pixel[2];
        auto [color, added] = colors.try_emplace(key, static_cast<GifPixelType>(colors.size() + 1));
        if (added) {
            if (colors.size() == indexed.palette.size()) {
                index_with_uniform_palette(frame, indexed);
                return indexed;
            }
            indexed.palette[color->second] = {pixel[0], pixel[1], pixel[2]};
        }
        indexed.pixels[index] = color->second;
    }
    return indexed;
}

void write_gif_frame(GifFileType* gif, const Raster& frame, float seconds)
{
    auto indexed = index_frame(frame);
    const auto palette = make_gif_map(static_cast<int>(indexed.palette.size()), indexed.palette.data());
    const double delay = std::clamp(std::round(static_cast<double>(seconds) * 100), 0.0, 65535.0);
    const GraphicsControlBlock control{DISPOSE_BACKGROUND, false, static_cast<int>(delay), 0};
    std::array<GifByteType, 4> extension{};
    const auto extension_size = EGifGCBToExtension(&control, extension.data());
    check_gif(gif, EGifPutExtension(gif, GRAPHICS_EXT_FUNC_CODE, static_cast<int>(extension_size), extension.data()));
    check_gif(gif, EGifPutImageDesc(gif, 0, 0, frame.width, frame.height, false, palette.get()));
    for (int row = 0; row < frame.height; ++row) {
        auto* line = indexed.pixels.data() + static_cast<std::size_t>(row) * frame.width;
        check_gif(gif, EGifPutLine(gif, line, frame.width));
    }
}

void write_gif(InputFile& input, const Texture& texture, std::FILE* output, std::size_t byte_limit)
{
    if (texture.embedded_format == 25) {
        write_embedded_media(input, texture.images.front().front(), output, false);
        return;
    }
    if (texture.embedded_format >= 0 || texture.video()) {
        throw std::runtime_error("GIF sprite sheets require raw or DXT texture data");
    }
    const auto canvas_pixels = pixel_count(texture.gif_width, texture.gif_height, byte_limit);
    if (texture.gif_width > 65535 || texture.gif_height > 65535) {
        throw std::runtime_error("GIF canvas exceeds the format's dimension limit");
    }

    int error = 0;
    std::unique_ptr<GifFileType, decltype(&close_gif)> gif(EGifOpen(output, gif_write, &error), &close_gif);
    if (!gif) {
        gif_failure(error);
    }

    EGifSetGifVersion(gif.get(), true);
    const std::array<GifColorType, 2> background{};
    const auto palette = make_gif_map(static_cast<int>(background.size()), background.data());
    check_gif(gif.get(), EGifPutScreenDesc(gif.get(), texture.gif_width, texture.gif_height, 8, 0, palette.get()));
    check_gif(gif.get(), EGifPutExtensionLeader(gif.get(), APPLICATION_EXT_FUNC_CODE));
    check_gif(gif.get(), EGifPutExtensionBlock(gif.get(), 11, "NETSCAPE2.0"));
    const std::array<GifByteType, 3> repeat{1, 0, 0};
    check_gif(gif.get(), EGifPutExtensionBlock(gif.get(), static_cast<int>(repeat.size()), repeat.data()));
    check_gif(gif.get(), EGifPutExtensionTrailer(gif.get()));

    std::size_t sheet_id = texture.images.size();
    std::unique_ptr<Raster> sheet;
    for (const auto& frame : texture.frames) {
        if (frame.image != sheet_id) {
            sheet.reset();
            sheet = std::make_unique<Raster>(decode_raster(
                input, texture.images[frame.image].front(), texture.format, byte_limit));
            sheet_id = frame.image;
        }
        write_gif_frame(gif.get(), make_gif_frame(texture, frame, *sheet, canvas_pixels), frame.seconds);
    }

    if (EGifCloseFile(gif.release(), &error) != GIF_OK) {
        gif_failure(error);
    }
}

}  // namespace

void write_texture_image(InputFile& input, const Texture& texture, std::FILE* output, std::size_t byte_limit)
{
    if (texture.animated()) {
        write_gif(input, texture, output, byte_limit);
        return;
    }

    const auto& mipmap = texture.images.front().front();
    if (texture.video() || texture.embedded_format >= 0) {
        write_embedded_media(input, mipmap, output, texture.video());
        return;
    }

    const auto raster = decode_raster(input, mipmap, texture.format, byte_limit);
    write_png(output, raster, texture.image_width, texture.image_height);
}

}  // namespace repkg
