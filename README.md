# RePKG C++ for Ubuntu

This native C++20 command-line port of [RePKG](https://github.com/notscuffed/repkg)
extracts Wallpaper Engine PKG archives, converts TEX assets, and inspects both
formats. The format reference is upstream commit
`8005eb271f4cb5b9e375bafa912029a0d67b5eb8`.

## Build on Ubuntu 24.04

## Clone Repository

```bash
git clone https://github.com/DirmdsLab/repkg.git
cd repkg

# Switch to C++ branch
git switch repkg-cpp
git branch -a
```

## Build with Podman

```bash
podman run -it \
    --name wallpaper-engine \
    -v "$(pwd):/workspace/repkg" \
    -w /workspace \
    ubuntu:24.04 \
    bash
```

### Install Dependencies

Run inside the container:

```bash
cd /workspace

apt-get update

apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    unzip \
    liblz4-dev \
    libpng-dev \
    libgif-dev \
    nlohmann-json3-dev
```

### Build

```sh
cd /workspace/repkg
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The executable is at `/workspace/repkg/build/repkg`. The port uses the
development packages above and does not require .NET or Wine. For a non-root
Ubuntu shell, run the two `apt-get` commands with `sudo`.

## Usage

```sh
mkdir input
```

```sh
./build/repkg help
./build/repkg extract -o ./output ./input/scene.pkg
./build/repkg extract --allow-high-size -o ./output ./input/scene.pkg
./build/repkg extract -c -n -r -o ./projects ./workshop
./build/repkg extract --no-tex-convert -o ./raw ./input/scene.pkg
./build/repkg extract -t -r -o ./images ./textures
./build/repkg info -e -b size -p 'title,preview' ./input/scene.pkg
./build/repkg info ./materials/background.tex
```

A single PKG normally extracts directly into the output directory.
Directory mode finds PKGs in the selected directory and its immediate
subdirectories. With `extract -r`, it also searches deeper directories.
For loose TEX files, `-t` selects the current input directory and `-r` includes
subdirectories. Loose textures are converted into the output directory using
their original basenames. The converter adds a suffix when basenames collide.
For PKGs, relative archive paths are preserved unless
`--singledir` is specified.

Package extraction keeps original TEX files and writes converted images
alongside them with a `.tex-json` sidecar. `--no-tex-convert` skips both
converted outputs for package entries. If an output name conflicts with an
entry being extracted from a PKG in the same run, the converter uses a distinct name
for the converted file and its sidecar. `--copyproject` copies the adjacent
`project.json` and preview. When the metadata omits the preview, the program
looks for common `preview.*` filenames. Existing output files are skipped
unless `--overwrite` is given. A directory or other non-regular destination
blocks that file and is reported as an error.

`--allow-high-size` raises the per-mipmap and decoded raster limits from
250,000,000 to 2,147,483,647 bytes. It applies to TEX files extracted from a
PKG and standalone TEX files. Use it with `info` to inspect large standalone
textures. The input must still contain the declared payload and decoded images
must fit in available memory. Uncompressed embedded media are copied in chunks.

Run `repkg help extract` and `repkg help info` for the full options. The exit
status is 0 on success, 1 if any input or entry fails, and 2 for invalid
command-line arguments. Batch extraction continues after individual errors.

## Format support

- PKG entry table and payload extraction, including packages with Windows-style
  path separators. The extractor rejects absolute paths and parent traversal.
- TEX files with `TEXV0005`, `TEXI0001`, and image containers
  `TEXB0001` through `TEXB0004`, using the layouts implemented upstream.
- RGBA8888, R8, RG88, DXT1, DXT3, and DXT5 textures, including LZ4 blocks.
  Still images are exported as PNG at the dimensions recorded in the TEX header.
- `TEXS0001` through `TEXS0003` animations from raw or DXT sprite sheets,
  including rotated frames. Generated GIF files use a local palette per frame
  and GIF's single transparent palette entry.
- Embedded image files and MP4 payloads are extracted in their stored media
  format.

The port covers the original command-line workflows. The upstream
library-only PKG and TEX writers are outside this command-line port.
`info` also reports TEX metadata and handles sorting by file extension.
The archive contains the C++ source and CMake build configuration.

## License

RePKG is licensed under MIT. See `LICENSE` and `THIRD-PARTY-NOTICES.txt`.
