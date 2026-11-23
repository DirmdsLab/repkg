#pragma once

#include "io.hpp"

#include <vector>

namespace repkg {

struct PackageEntry {
    std::string name;
    std::uint64_t offset{};
    std::uint64_t length{};
};

struct Package {
    std::string magic;
    std::uint64_t header_size{};
    std::vector<PackageEntry> entries;
};

Package read_package(BinaryReader reader);

}  // namespace repkg
