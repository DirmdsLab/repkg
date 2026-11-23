cmake_minimum_required(VERSION 3.22)
project(repkg VERSION 1.0.3 LANGUAGES CXX)

find_package(PNG REQUIRED)
find_package(GIF REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LZ4 REQUIRED IMPORTED_TARGET liblz4)

add_executable(repkg
    src/main.cpp
    src/io.cpp
    src/package.cpp
    src/texture.cpp
    src/image.cpp
)
target_compile_features(repkg PRIVATE cxx_std_20)
set_target_properties(repkg PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(repkg PRIVATE PNG::PNG GIF::GIF nlohmann_json::nlohmann_json PkgConfig::LZ4)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(repkg PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(GNUInstallDirs)
install(TARGETS repkg RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
