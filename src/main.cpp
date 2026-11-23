#include "io.hpp"
#include "package.hpp"
#include "texture.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace repkg {
namespace {

using Json = nlohmann::ordered_json;

struct CommonOptions {
    fs::path input;
    bool tex_directory{};
    std::size_t texture_byte_limit{default_texture_byte_limit};
};

struct ExtractOptions : CommonOptions {
    fs::path output{"./output"};
    std::string ignored_extensions;
    std::string only_extensions;
    bool debug{};
    bool flat{};
    bool recursive{};
    bool copy_project{};
    bool use_name{};
    bool no_tex_convert{};
    bool overwrite{};
};

struct InfoOptions : CommonOptions {
    bool sort{};
    bool print_entries{};
    std::string sort_by{"name"};
    std::string project_keys;
    std::string title_filter;
};

constexpr std::string_view general_help = R"(RePKG C++ 1.0.3

Usage:
  repkg extract [options] <file-or-directory>
  repkg info [options] <file-or-directory>
  repkg interactive
  repkg help [extract|info]
)";

constexpr std::string_view extract_help = R"(Usage: repkg extract [options] <file-or-directory>

  -o, --output DIR       Output directory (default: ./output)
  -i, --ignoreexts LIST  Skip package entries with these extensions
  -e, --onlyexts LIST    Extract only package entries with these extensions
  -d, --debuginfo        Print package and texture metadata during extraction
  -t, --tex              Convert loose TEX files when the input is a directory
  -s, --singledir        Put all extracted files in one output directory
  -r, --recursive        Scan all levels below the input directory
  -c, --copyproject      Copy project.json and the preview next to each PKG
  -n, --usename          Use the title from project.json as the project folder name
      --no-tex-convert   Keep package TEX assets without converting them
      --allow-high-size Allow TEX mipmaps and decoded images larger than 250 MB
      --overwrite        Replace existing output files

Extension lists are comma-separated. --ignoreexts takes precedence over --onlyexts.
Use -- before an input path beginning with a dash.
)";

constexpr std::string_view info_help = R"(Usage: repkg info [options] <file-or-directory>

  -e, --printentries      List package entries
  -s, --sort              Sort package entries
  -b, --sortby FIELD      Sort by name, extension or size (default: name)
  -t, --tex               Inspect loose TEX files when the input is a directory
  -p, --projectinfo KEYS  Print comma-separated project.json keys, or * for all
      --title-filter STR  Show only package titles containing this text
      --allow-high-size   Inspect TEX mipmaps larger than 250 MB
)";

class CommandArguments {
public:
    explicit CommandArguments(std::span<const std::string> arguments) : arguments_(arguments) {}

    bool has_next() const { return index_ < arguments_.size(); }

    std::string_view next()
    {
        const std::string_view token = arguments_[index_++];
        inline_value_ = {};
        has_inline_value_ = false;
        if (options_enabled_ && token.starts_with("--")) {
            const auto equals = token.find('=');
            if (equals != std::string_view::npos) {
                inline_value_ = token.substr(equals + 1);
                has_inline_value_ = true;
                return token.substr(0, equals);
            }
        }
        return token;
    }

    std::string value(std::string_view option)
    {
        if (has_inline_value_) {
            has_inline_value_ = false;
            return std::string(inline_value_);
        }
        if (!has_next() || arguments_[index_].starts_with('-')) {
            throw std::invalid_argument("Missing value for " + std::string(option));
        }
        return arguments_[index_++];
    }

    bool flag(std::string_view option) const
    {
        if (has_inline_value_) {
            throw std::invalid_argument("Unexpected value for " + std::string(option));
        }
        return true;
    }

    void input(std::string_view arg, CommonOptions& options)
    {
        if (options_enabled_ && arg == "--") {
            flag(arg);
            options_enabled_ = false;
            return;
        }
        if (is_option(arg)) {
            throw std::invalid_argument("Unknown option " + std::string(arg));
        }
        if (arg.empty() || !options.input.empty()) {
            throw std::invalid_argument("Expected exactly one input path");
        }
        options.input = std::string(arg);
    }

    bool is_option(std::string_view arg) const
    {
        return options_enabled_ && arg.starts_with('-');
    }

private:
    std::span<const std::string> arguments_;
    std::size_t index_{};
    bool options_enabled_{true};
    std::string_view inline_value_;
    bool has_inline_value_{};
};

template<class Options>
bool parse_options(std::span<const std::string> arguments, Options& options)
{
    CommandArguments args(arguments);
    while (args.has_next()) {
        const auto arg = args.next();
        if (args.is_option(arg)) {
            if (arg == "--help" || arg == "-h") {
                args.flag(arg);
                return false;
            }
            if (arg == "--tex" || arg == "-t") {
                options.tex_directory = args.flag(arg);
                continue;
            }
            if (arg == "--allow-high-size") {
                args.flag(arg);
                options.texture_byte_limit = high_texture_byte_limit;
                continue;
            }
            if constexpr (std::is_same_v<Options, ExtractOptions>) {
                if (arg == "--output" || arg == "-o") {
                    options.output = args.value(arg);
                } else if (arg == "--ignoreexts" || arg == "-i") {
                    options.ignored_extensions = args.value(arg);
                } else if (arg == "--onlyexts" || arg == "-e") {
                    options.only_extensions = args.value(arg);
                } else if (arg == "--debuginfo" || arg == "-d") {
                    options.debug = args.flag(arg);
                } else if (arg == "--singledir" || arg == "-s") {
                    options.flat = args.flag(arg);
                } else if (arg == "--recursive" || arg == "-r") {
                    options.recursive = args.flag(arg);
                } else if (arg == "--copyproject" || arg == "-c") {
                    options.copy_project = args.flag(arg);
                } else if (arg == "--usename" || arg == "-n") {
                    options.use_name = args.flag(arg);
                } else if (arg == "--no-tex-convert") {
                    options.no_tex_convert = args.flag(arg);
                } else if (arg == "--overwrite") {
                    options.overwrite = args.flag(arg);
                } else {
                    args.input(arg, options);
                }
            } else {
                if (arg == "--sort" || arg == "-s") {
                    options.sort = args.flag(arg);
                } else if (arg == "--printentries" || arg == "-e") {
                    options.print_entries = args.flag(arg);
                } else if (arg == "--sortby" || arg == "-b") {
                    options.sort_by = lower_ascii(args.value(arg));
                    options.sort = true;
                } else if (arg == "--projectinfo" || arg == "-p") {
                    options.project_keys = args.value(arg);
                } else if (arg == "--title-filter") {
                    options.title_filter = lower_ascii(args.value(arg));
                } else {
                    args.input(arg, options);
                }
            }
        } else {
            args.input(arg, options);
        }
    }

    if (options.input.empty()) {
        throw std::invalid_argument("An input file or directory is required");
    }
    return true;
}

std::set<std::string> split_list(std::string_view text, bool extensions)
{
    std::set<std::string> items;
    while (!text.empty()) {
        const auto comma = text.find(',');
        const auto item = text.substr(0, comma);
        const auto begin = item.find_first_not_of(" \t\r\n");
        if (begin != std::string_view::npos) {
            const auto end = item.find_last_not_of(" \t\r\n");
            auto value = lower_ascii(std::string(item.substr(begin, end - begin + 1)));
            if (extensions && !value.starts_with('.')) {
                value.insert(value.begin(), '.');
            }
            items.insert(std::move(value));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1);
    }
    return items;
}

std::string extension_of(const fs::path& path)
{
    return lower_ascii(path.extension().string());
}

bool same_file(const fs::path& first, const fs::path& second)
{
    std::error_code error;
    return fs::equivalent(first, second, error);
}

bool is_plain_file(const fs::directory_entry& entry)
{
    return fs::is_regular_file(entry.symlink_status());
}

std::vector<fs::path> find_inputs(const CommonOptions& options, bool recursive)
{
    if (fs::is_regular_file(options.input)) {
        const auto extension = extension_of(options.input);
        if (extension != ".pkg" && extension != ".tex") {
            throw std::runtime_error("Unsupported input extension " + extension);
        }
        return {options.input};
    }
    if (!fs::is_directory(options.input)) {
        throw std::runtime_error("Input file or directory does not exist: " + options.input.string());
    }

    const auto extension = options.tex_directory ? ".tex" : ".pkg";
    std::vector<fs::path> inputs;
    const auto add_file = [&](const fs::directory_entry& entry) {
        if (is_plain_file(entry) && extension_of(entry.path()) == extension) {
            inputs.push_back(entry.path());
        }
    };
    if (recursive) {
        for (const auto& entry : fs::recursive_directory_iterator(options.input)) {
            add_file(entry);
        }
    } else {
        for (const auto& entry : fs::directory_iterator(options.input)) {
            add_file(entry);
            if (!options.tex_directory && fs::is_directory(entry.symlink_status())) {
                for (const auto& child : fs::directory_iterator(entry.path())) {
                    add_file(child);
                }
            }
        }
    }

    std::sort(inputs.begin(), inputs.end());
    return inputs;
}

fs::path sibling_file(const fs::path& directory, std::string_view name)
{
    const auto exact_match = directory / name;
    if (fs::is_regular_file(fs::symlink_status(exact_match))) {
        return exact_match;
    }
    const auto expected = lower_ascii(std::string(name));
    for (const auto& file : fs::directory_iterator(directory)) {
        if (is_plain_file(file) && lower_ascii(file.path().filename().string()) == expected) {
            return file.path();
        }
    }
    return {};
}

struct Project {
    fs::path file;
    Json data = Json::object();
};

Project read_project(const fs::path& directory)
{
    Project project;
    project.file = sibling_file(directory, "project.json");
    if (project.file.empty()) {
        return project;
    }
    std::ifstream source(project.file, std::ios::binary);
    if (!source) {
        throw std::runtime_error("Cannot read " + project.file.string());
    }
    project.data = Json::parse(source);
    if (!project.data.is_object()) {
        throw std::runtime_error("Expected a JSON object in " + project.file.string());
    }
    return project;
}

std::string string_property(const Json& data, std::string_view key)
{
    const auto property = data.find(std::string(key));
    if (property != data.end() && property->is_string()) {
        return property->get<std::string>();
    }
    return {};
}

void write_project(const Project& project, const fs::path& directory,
                   const fs::path& base, const OutputTree& output)
{
    if (!project.file.empty()) {
        output.copy(project.file, base / "project.json");
    }

    const auto configured_preview = string_property(project.data, "preview");
    if (!configured_preview.empty()) {
        const auto relative = package_path(configured_preview);
        auto parent = directory;
        bool found_parent = true;
        for (const auto& component : relative.parent_path()) {
            parent /= component;
            if (!fs::is_directory(fs::symlink_status(parent))) {
                found_parent = false;
                break;
            }
        }
        if (found_parent) {
            const auto preview = sibling_file(parent, relative.filename().string());
            if (!preview.empty()) {
                output.copy(preview, base / relative);
                return;
            }
        }
    }
    for (const auto* filename : {"preview.jpg", "preview.png", "preview.gif", "preview.jpeg", "preview.webp"}) {
        const auto preview = sibling_file(directory, filename);
        if (!preview.empty()) {
            output.copy(preview, base / preview.filename());
            return;
        }
    }
}

void print_write_status(bool written, const fs::path& path)
{
    std::cout << (written ? "  Wrote " : "  Skipped ") << path.string() << '\n';
}

struct TextureOutputPaths {
    fs::path media;
    fs::path metadata;
};

TextureOutputPaths texture_output_paths(const fs::path& raw_path, std::string_view extension,
                                        std::set<fs::path>& reserved_paths)
{
    const auto default_base = raw_path.parent_path() / raw_path.stem();
    const auto media_suffix = "." + std::string(extension);
    for (std::size_t attempt = 0;; ++attempt) {
        auto base = attempt == 0 ? default_base : raw_path;
        if (attempt > 1) {
            base += "." + std::to_string(attempt);
        }
        TextureOutputPaths paths{base, base};
        paths.media += media_suffix;
        paths.metadata += ".tex-json";
        if (!reserved_paths.contains(paths.media) && !reserved_paths.contains(paths.metadata)) {
            reserved_paths.insert(paths.media);
            reserved_paths.insert(paths.metadata);
            return paths;
        }
    }
}

void convert_texture(InputFile& input, BinaryReader reader, const fs::path& raw_path,
                     const OutputTree& output, const ExtractOptions& options,
                     std::set<fs::path>& reserved_paths)
{
    const auto texture = read_texture(reader, options.texture_byte_limit);
    if (options.debug) {
        std::cout << describe_texture(texture).dump(2) << '\n';
    }
    const auto paths = texture_output_paths(raw_path, output_extension(texture), reserved_paths);
    print_write_status(output.write(paths.media, [&](std::FILE* file) {
        write_texture_image(input, texture, file, options.texture_byte_limit);
    }), output.root() / paths.media);

    const auto metadata = texture_sidecar(texture).dump(2) + '\n';
    print_write_status(output.write(paths.metadata, [&](std::FILE* file) {
        write_text(file, metadata);
    }), output.root() / paths.metadata);
}

class Extractor {
public:
    explicit Extractor(const ExtractOptions& options)
        : options_(options), output_(options.output.empty() ? fs::path(".") : options.output, options.overwrite),
          ignored_(split_list(options.ignored_extensions, true)),
          allowed_(split_list(options.only_extensions, true)) {}

    void reserve_batch_outputs(const std::vector<fs::path>& paths, bool from_directory)
    {
        if (paths.size() < 2 || options_.no_tex_convert || extension_of(paths.front()) != ".pkg") {
            return;
        }
        // Directory inputs can contain several packages that share an output folder.
        // Reserve their original paths before the first conversion chooses a name.
        for (const auto& path : paths) {
            try {
                InputFile input(path);
                const auto package = read_package(input.reader());
                const auto location = package_location(path, from_directory);
                plan_entries(package, location.base, [](const PackageEntry&, const std::exception&) {});
            } catch (const std::exception&) {
                // The extraction pass reports the error against its source package.
            }
        }
    }

    bool extract(const fs::path& path, bool from_directory)
    {
        InputFile input(path);
        if (extension_of(path) == ".tex") {
            convert_texture(input, input.reader(), path.filename(), output_, options_, reserved_paths_);
            return true;
        }

        const auto package = read_package(input.reader());
        if (options_.debug) {
            std::cout << "  Magic: " << package.magic << '\n'
                      << "  Header bytes: " << package.header_size << '\n'
                      << "  Entries: " << package.entries.size() << '\n';
        }

        const auto location = package_location(path, from_directory);
        bool succeeded = true;
        const auto report_entry_error = [&](const PackageEntry& entry, const std::exception& error) {
            std::cerr << "Error in entry " << entry.name << ": " << error.what() << '\n';
            succeeded = false;
        };
        const auto plan = plan_entries(package, location.base, report_entry_error);
        for (const auto& item : plan) {
            try {
                extract_entry(input, path, item);
            } catch (const std::exception& error) {
                report_entry_error(item.entry, error);
            }
        }
        if (options_.copy_project && !options_.flat) {
            write_project(location.project, location.directory, location.base, output_);
        }
        return succeeded;
    }

private:
    struct PackageLocation {
        fs::path directory;
        fs::path base;
        Project project;
    };

    struct PlannedEntry {
        const PackageEntry& entry;
        fs::path target;
    };

    PackageLocation package_location(const fs::path& path, bool from_directory) const
    {
        PackageLocation location;
        location.directory = path.has_parent_path() ? path.parent_path() : fs::path(".");
        if (options_.flat) {
            return location;
        }
        if (options_.copy_project || options_.use_name) {
            location.project = read_project(location.directory);
        }
        if (from_directory) {
            location.base = location.directory.lexically_relative(options_.input);
            if (location.base == ".") {
                location.base.clear();
            }
        }
        if (options_.use_name) {
            const auto title = safe_project_name(string_property(location.project.data, "title"));
            if (!title.empty()) {
                location.base = location.base.parent_path() / title;
            }
        }
        return location;
    }

    std::optional<fs::path> entry_target(const PackageEntry& entry, const fs::path& base) const
    {
        const auto relative = package_path(entry.name);
        const auto extension = extension_of(relative);
        if ((!ignored_.empty() && ignored_.contains(extension))
            || (ignored_.empty() && !allowed_.empty() && !allowed_.contains(extension))) {
            return std::nullopt;
        }
        return base / (options_.flat ? relative.filename() : relative);
    }

    template<class OnError>
    std::vector<PlannedEntry> plan_entries(const Package& package, const fs::path& base, const OnError& on_error)
    {
        std::vector<PlannedEntry> plan;
        plan.reserve(package.entries.size());
        for (const auto& entry : package.entries) {
            try {
                auto target = entry_target(entry, base);
                if (!target) {
                    continue;
                }
                if (!options_.no_tex_convert) {
                    for (auto path = *target; !path.empty(); path = path.parent_path()) {
                        if (!reserved_paths_.insert(path).second) {
                            break;
                        }
                    }
                }
                plan.push_back({entry, std::move(*target)});
            } catch (const std::exception& error) {
                on_error(entry, error);
            }
        }
        return plan;
    }

    void extract_entry(InputFile& input, const fs::path& source, const PlannedEntry& item)
    {
        const auto& [entry, target] = item;
        if (same_file(source, output_.root() / target)) {
            throw std::runtime_error("Output path refers to the input package");
        }
        print_write_status(output_.write(target, [&](std::FILE* file) {
            copy_bytes(input.slice(entry.offset, entry.length), file);
        }), output_.root() / target);

        if (!options_.no_tex_convert && extension_of(target) == ".tex") {
            convert_texture(input, input.slice(entry.offset, entry.length), target, output_, options_, reserved_paths_);
        }
    }

    const ExtractOptions& options_;
    const OutputTree output_;
    const std::set<std::string> ignored_;
    const std::set<std::string> allowed_;
    std::set<fs::path> reserved_paths_;
};

void sort_entries(Package& package, const InfoOptions& options)
{
    if (!options.sort) {
        return;
    }
    std::stable_sort(package.entries.begin(), package.entries.end(), [&](const auto& a, const auto& b) {
        if (options.sort_by == "size" && a.length != b.length) {
            return a.length < b.length;
        }
        if (options.sort_by == "extension") {
            const auto a_extension = extension_of(fs::path(path_with_slashes(a.name)));
            const auto b_extension = extension_of(fs::path(path_with_slashes(b.name)));
            if (a_extension != b_extension) {
                return a_extension < b_extension;
            }
        }
        return lower_ascii(a.name) < lower_ascii(b.name);
    });
}

void inspect(const fs::path& path, const InfoOptions& options, const std::set<std::string>& keys)
{
    if (extension_of(path) == ".tex") {
        InputFile input(path);
        const auto texture = read_texture(input.reader(), options.texture_byte_limit);
        std::cout << "Texture " << path.string() << '\n' << describe_texture(texture).dump(2) << '\n';
        return;
    }

    Project project;
    if (!keys.empty() || !options.title_filter.empty()) {
        project = read_project(path.has_parent_path() ? path.parent_path() : fs::path("."));
        if (!options.title_filter.empty()) {
            const auto title = lower_ascii(string_property(project.data, "title"));
            if (title.find(options.title_filter) == std::string::npos) {
                return;
            }
        }
    }

    InputFile input(path);
    auto package = read_package(input.reader());
    std::cout << "Package " << path.string() << '\n'
              << "  Magic: " << package.magic << '\n'
              << "  Header bytes: " << package.header_size << '\n'
              << "  Entries: " << package.entries.size() << '\n';

    for (auto field = project.data.begin(); field != project.data.end(); ++field) {
        if (keys.contains("*") || keys.contains(lower_ascii(field.key()))) {
            const auto text = field->is_string() ? field->get<std::string>() : field->dump(2);
            std::cout << "  " << field.key() << ": " << text << '\n';
        }
    }

    if (options.print_entries) {
        sort_entries(package, options);
        for (const auto& entry : package.entries) {
            std::cout << "  " << entry.name << " - " << entry.length << " bytes\n";
        }
    }
}

template<class Action>
int process_inputs(const std::vector<fs::path>& files, const Action& action)
{
    if (files.empty()) {
        std::cout << "No matching files found.\n";
    }
    bool succeeded = true;
    for (const auto& file : files) {
        try {
            if (!action(file)) {
                succeeded = false;
            }
        } catch (const std::exception& error) {
            std::cerr << "Error in " << file.string() << ": " << error.what() << '\n';
            succeeded = false;
        }
    }
    return succeeded ? 0 : 1;
}

int run_command(std::span<const std::string> arguments);

std::vector<std::string> split_command(std::string_view line)
{
    std::vector<std::string> arguments;
    std::string token;
    char quote = 0;
    bool escaping = false;
    bool started = false;
    for (const char ch : line) {
        if (escaping) {
            token += ch;
            escaping = false;
        } else if (ch == '\\' && quote != '\'') {
            escaping = true;
        } else if (quote != 0) {
            if (ch == quote) {
                quote = 0;
            } else {
                token += ch;
            }
        } else if (ch == '"' || ch == '\'') {
            quote = ch;
            started = true;
        } else if (ch == ' ' || ch == '\t') {
            if (started) {
                arguments.push_back(token);
                token.clear();
                started = false;
            }
        } else {
            token += ch;
            started = true;
        }
        if (escaping || !token.empty()) {
            started = true;
        }
    }
    if (escaping || quote != 0) {
        throw std::invalid_argument("Unfinished escape or quote");
    }
    if (started) {
        arguments.push_back(token);
    }
    return arguments;
}

int interactive()
{
    std::cout << "RePKG interactive mode. Enter help for commands or exit to quit.\n";
    std::string line;
    while (std::cout << "repkg> " << std::flush, std::getline(std::cin, line)) {
        try {
            const auto arguments = split_command(line);
            if (arguments.empty() || arguments.front() == "exit" || arguments.front() == "quit") {
                break;
            }
            if (arguments.front() == "interactive") {
                throw std::invalid_argument("Already in interactive mode");
            }
            run_command(arguments);
        } catch (const std::exception& error) {
            std::cerr << "Error: " << error.what() << '\n';
        }
    }
    return 0;
}

int run_command(std::span<const std::string> arguments)
{
    if (arguments.empty() || (arguments.size() == 1 &&
        (arguments.front() == "help" || arguments.front() == "--help" || arguments.front() == "-h"))) {
        std::cout << general_help;
        return 0;
    }

    const auto& command = arguments.front();
    const auto tail = arguments.subspan(1);
    if (command == "help" && tail.size() == 1) {
        if (tail.front() == "extract") {
            std::cout << extract_help;
            return 0;
        }
        if (tail.front() == "info") {
            std::cout << info_help;
            return 0;
        }
    }
    if (command == "--version" && tail.empty()) {
        std::cout << "RePKG C++ 1.0.3\n";
        return 0;
    }
    if (command == "interactive" && tail.empty()) {
        return interactive();
    }

    if (command == "extract") {
        ExtractOptions options;
        if (!parse_options(tail, options)) {
            std::cout << extract_help;
            return 0;
        }
        const auto files = find_inputs(options, options.recursive);
        const bool directory = fs::is_directory(options.input);
        Extractor extractor(options);
        extractor.reserve_batch_outputs(files, directory);
        return process_inputs(files, [&](const fs::path& file) {
            std::cout << "Extracting " << file.string() << '\n';
            return extractor.extract(file, directory);
        });
    }

    if (command == "info") {
        InfoOptions options;
        if (!parse_options(tail, options)) {
            std::cout << info_help;
            return 0;
        }
        if (options.sort_by != "name" && options.sort_by != "extension" && options.sort_by != "size") {
            throw std::invalid_argument("Sort field must be name, extension or size");
        }
        const auto files = find_inputs(options, false);
        const auto keys = split_list(options.project_keys, false);
        return process_inputs(files, [&](const fs::path& file) {
            inspect(file, options, keys);
            return true;
        });
    }
    throw std::invalid_argument("Unknown command or arguments. Use repkg help.");
}

}  // namespace
}  // namespace repkg

int main(int argc, char** argv)
{
    try {
        std::vector<std::string> arguments(argv + 1, argv + argc);
        return repkg::run_command(arguments);
    } catch (const std::invalid_argument& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
