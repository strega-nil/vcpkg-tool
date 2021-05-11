
#include <vcpkg/base/checks.h>
#include <vcpkg/base/files.h>
#include <vcpkg/base/json.h>

#include <vcpkg/commands.add-version.h>
#include <vcpkg/configuration.h>
#include <vcpkg/paragraphs.h>
#include <vcpkg/portfileprovider.h>
#include <vcpkg/registries.h>
#include <vcpkg/vcpkgcmdarguments.h>
#include <vcpkg/vcpkgpaths.h>
#include <vcpkg/versions.h>

using namespace vcpkg;

namespace
{
    constexpr StringLiteral BASELINE = "baseline";
    constexpr StringLiteral VERSION_RELAXED = "version";
    constexpr StringLiteral VERSION_SEMVER = "version-semver";
    constexpr StringLiteral VERSION_DATE = "version-date";
    constexpr StringLiteral VERSION_STRING = "version-string";

    using VersionGitTree = std::pair<SchemedVersion, std::string>;

    void insert_version_to_json_object(Json::Object& obj, const VersionT& version, StringLiteral version_field)
    {
        obj.insert(version_field, Json::Value::string(version.text()));
        obj.insert("port-version", Json::Value::integer(version.port_version()));
    }

    void insert_schemed_version_to_json_object(Json::Object& obj, const SchemedVersion& version)
    {
        if (version.scheme == Versions::Scheme::Relaxed)
        {
            return insert_version_to_json_object(obj, version.versiont, VERSION_RELAXED);
        }

        if (version.scheme == Versions::Scheme::Semver)
        {
            return insert_version_to_json_object(obj, version.versiont, VERSION_SEMVER);
        }

        if (version.scheme == Versions::Scheme::Date)
        {
            return insert_version_to_json_object(obj, version.versiont, VERSION_DATE);
        }

        if (version.scheme == Versions::Scheme::String)
        {
            return insert_version_to_json_object(obj, version.versiont, VERSION_STRING);
        }
        Checks::unreachable(VCPKG_LINE_INFO);
    }

    static Json::Object serialize_baseline(const std::map<std::string, VersionT, std::less<>>& baseline)
    {
        Json::Object port_entries_obj;
        for (auto&& kv_pair : baseline)
        {
            Json::Object baseline_version_obj;
            insert_version_to_json_object(baseline_version_obj, kv_pair.second, BASELINE);
            port_entries_obj.insert(kv_pair.first, baseline_version_obj);
        }

        Json::Object baseline_obj;
        baseline_obj.insert("default", port_entries_obj);
        return baseline_obj;
    }

    static Json::Object serialize_versions(const std::vector<VersionGitTree>& versions)
    {
        Json::Array versions_array;
        for (auto&& version : versions)
        {
            Json::Object version_obj;
            version_obj.insert("git-tree", Json::Value::string(version.second));
            insert_schemed_version_to_json_object(version_obj, version.first);
            versions_array.push_back(std::move(version_obj));
        }

        Json::Object output_object;
        output_object.insert("versions", versions_array);
        return output_object;
    }

    static void write_baseline_file(Files::Filesystem& fs,
                                    const std::map<std::string, VersionT, std::less<>>& baseline_map,
                                    const fs::path& output_path)
    {
        auto new_path = output_path;
        new_path += fs::u8path(".tmp");
        std::error_code ec;
        fs.create_directories(output_path.parent_path(), VCPKG_LINE_INFO);
        fs.write_contents(new_path,
                          Json::stringify(serialize_baseline(baseline_map), Json::JsonStyle::with_spaces(2)),
                          VCPKG_LINE_INFO);
        fs.rename(new_path, output_path, VCPKG_LINE_INFO);
    }

    static void write_versions_file(Files::Filesystem& fs,
                                    const std::vector<VersionGitTree>& versions,
                                    const fs::path& output_path)
    {
        auto new_path = output_path;
        new_path += fs::u8path(".tmp");
        std::error_code ec;
        fs.create_directories(output_path.parent_path(), VCPKG_LINE_INFO);
        fs.write_contents(
            new_path, Json::stringify(serialize_versions(versions), Json::JsonStyle::with_spaces(2)), VCPKG_LINE_INFO);
        fs.rename(new_path, output_path, VCPKG_LINE_INFO);
    }

    static void update_baseline_version(const VcpkgPaths& paths,
                                        const std::string& port_name,
                                        const VersionT& version,
                                        const fs::path& baseline_path,
                                        std::map<std::string, vcpkg::VersionT, std::less<>>& baseline_map,
                                        bool print_success)
    {
        auto& fs = paths.get_filesystem();

        auto it = baseline_map.find(port_name);
        if (it != baseline_map.end())
        {
            auto& baseline_version = it->second;
            if (baseline_version == version)
            {
                if (print_success)
                {
                    System::printf(System::Color::success,
                                   "Version `%s` is already in `%s`\n",
                                   version,
                                   fs::u8string(baseline_path));
                }
                return;
            }
            baseline_version = version;
        }
        else
        {
            baseline_map.emplace(port_name, version);
        }

        write_baseline_file(fs, baseline_map, baseline_path);
        if (print_success)
        {
            System::printf(System::Color::success,
                           "Added version `%s` to `%s`.\n",
                           version.to_string(),
                           fs::u8string(baseline_path));
        }
        return;
    }

    static void update_version_db_file(const VcpkgPaths& paths,
                                       const std::string& port_name,
                                       const SchemedVersion& version,
                                       const std::string& git_tree,
                                       const fs::path& version_db_file_path,
                                       bool overwrite_version,
                                       bool print_success,
                                       bool keep_going)
    {
        auto& fs = paths.get_filesystem();
        if (!fs.exists(VCPKG_LINE_INFO, version_db_file_path))
        {
            std::vector<VersionGitTree> new_entry{{version, git_tree}};
            write_versions_file(fs, new_entry, version_db_file_path);
            if (print_success)
            {
                System::printf(System::Color::success,
                               "Added version `%s` to `%s` (new file).\n",
                               version.versiont,
                               fs::u8string(version_db_file_path));
            }
            return;
        }

        auto maybe_versions = get_builtin_versions(paths, port_name);
        if (auto versions = maybe_versions.get())
        {
            const auto& versions_end = versions->end();

            auto found_same_sha = std::find_if(
                versions->begin(), versions_end, [&](auto&& entry) -> bool { return entry.second == git_tree; });
            if (found_same_sha != versions_end)
            {
                if (found_same_sha->first.versiont == version.versiont)
                {
                    if (print_success)
                    {
                        System::printf(System::Color::success,
                                       "Version `%s` is already in `%s`\n",
                                       version.versiont,
                                       fs::u8string(version_db_file_path));
                    }
                    return;
                }
                System::printf(System::Color::warning,
                               "Warning: Local port files SHA is the same as version `%s` in `%s`.\n"
                               "-- SHA: %s\n"
                               "-- Did you remember to commit your changes?\n"
                               "***No files were updated.***\n",
                               found_same_sha->first.versiont,
                               fs::u8string(version_db_file_path),
                               git_tree);
                if (keep_going) return;
                Checks::exit_fail(VCPKG_LINE_INFO);
            }

            auto it = std::find_if(versions->begin(), versions_end, [&](auto&& entry) -> bool {
                return entry.first.versiont == version.versiont;
            });

            if (it != versions_end)
            {
                if (!overwrite_version)
                {
                    System::printf(System::Color::error,
                                   "Error: Local changes detected for %s but no changes to version or port version.\n"
                                   "-- Version: %s\n"
                                   "-- Old SHA: %s\n"
                                   "-- New SHA: %s\n"
                                   "-- Did you remember to update the version or port version?\n"
                                   "-- Pass `--overwrite-version` to bypass this check.\n"
                                   "***No files were updated.***\n",
                                   port_name,
                                   version.versiont,
                                   it->second,
                                   git_tree);
                    if (keep_going) return;
                    Checks::exit_fail(VCPKG_LINE_INFO);
                }

                it->first = version;
                it->second = git_tree;
            }
            else
            {
                versions->insert(versions->begin(), std::make_pair(version, git_tree));
            }

            write_versions_file(fs, *versions, version_db_file_path);
            if (print_success)
            {
                System::printf(System::Color::success,
                               "Added version `%s` to `%s`.\n",
                               version.versiont,
                               fs::u8string(version_db_file_path));
            }
            return;
        }

        System::printf(System::Color::error,
                       "Error: Unable to parse versions file %s.\n%s\n",
                       fs::u8string(version_db_file_path),
                       maybe_versions.error());
        Checks::exit_fail(VCPKG_LINE_INFO);
    }
}

namespace vcpkg::Commands::AddVersion
{
    static constexpr StringLiteral OPTION_ALL = "all";
    static constexpr StringLiteral OPTION_OVERWRITE_VERSION = "overwrite-version";
    static constexpr StringLiteral OPTION_SKIP_FORMATTING_CHECK = "skip-formatting-check";
    static constexpr StringLiteral OPTION_VERBOSE = "verbose";

    const CommandSwitch COMMAND_SWITCHES[] = {
        {OPTION_ALL, "Process versions for all ports."},
        {OPTION_OVERWRITE_VERSION, "Overwrite `git-tree` of an existing version."},
        {OPTION_SKIP_FORMATTING_CHECK, "Skips the formatting check of vcpkg.json files."},
        {OPTION_VERBOSE, "Print success messages instead of just errors."},
    };

    const CommandStructure COMMAND_STRUCTURE{
        create_example_string(R"###(x-add-version <port name>)###"),
        0,
        1,
        {{COMMAND_SWITCHES}, {}, {}},
        nullptr,
    };

    void perform_and_exit(const VcpkgCmdArguments&, const VcpkgPaths&)
    {
        Checks::exit_success(VCPKG_LINE_INFO);
    }

    void AddVersionCommand::perform_and_exit(const VcpkgCmdArguments& args, const VcpkgPaths& paths) const
    {
        AddVersion::perform_and_exit(args, paths);
    }
}
