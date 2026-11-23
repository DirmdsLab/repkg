#include "package.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace repkg {

Package read_package(BinaryReader reader)
{
    constexpr auto max_field = std::numeric_limits<std::int32_t>::max();
    Package package;
    package.magic = reader.sized_string(32, "package magic length");
    if (package.magic.empty()) {
        throw std::runtime_error("Missing package magic");
    }
    const auto count = reader.count(1'000'000, "package entry count");
    if (count > reader.remaining() / 12) {
        throw std::runtime_error("Package directory exceeds input size");
    }
    package.entries.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        PackageEntry entry;
        entry.name = reader.sized_string(255, "package entry path length");
        entry.offset = reader.count(max_field, "package entry offset");
        entry.length = reader.count(max_field, "package entry length");
        package.entries.push_back(std::move(entry));
    }

    package.header_size = reader.position();
    const auto available = reader.remaining();
    for (auto& entry : package.entries) {
        if (entry.offset > available || entry.length > available - entry.offset) {
            throw std::runtime_error("Entry exceeds package bounds: " + entry.name);
        }
        entry.offset += package.header_size;
    }
    return package;
}

}  // namespace repkg
