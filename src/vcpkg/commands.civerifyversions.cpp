#include <vcpkg/base/checks.h>
#include <vcpkg/base/files.h>
#include <vcpkg/base/json.h>
#include <vcpkg/base/system.debug.h>

#include <vcpkg/commands.civerifyversions.h>
#include <vcpkg/paragraphs.h>
#include <vcpkg/registries.h>
#include <vcpkg/sourceparagraph.h>
#include <vcpkg/vcpkgcmdarguments.h>
#include <vcpkg/vcpkgpaths.h>
#include <vcpkg/versiondeserializers.h>

namespace
{
    using namespace vcpkg;

    std::string get_scheme_name(Versions::Scheme scheme)
    {
        switch (scheme)
        {
            case Versions::Scheme::Relaxed: return "version";
            case Versions::Scheme::Semver: return "version-semver";
            case Versions::Scheme::String: return "version-string";
            case Versions::Scheme::Date: return "version-date";
            default: Checks::unreachable(VCPKG_LINE_INFO);
        }
    }
}

namespace vcpkg::Commands::CIVerifyVersions
{
    static constexpr StringLiteral OPTION_EXCLUDE = "exclude";
    static constexpr StringLiteral OPTION_VERBOSE = "verbose";
    static constexpr StringLiteral OPTION_VERIFY_GIT_TREES = "verify-git-trees";

    static constexpr CommandSwitch VERIFY_VERSIONS_SWITCHES[]{
        {OPTION_VERBOSE, "Print result for each port instead of just errors."},
        {OPTION_VERIFY_GIT_TREES, "Verify that each git tree object matches its declared version (this is very slow)"},
    };

    static constexpr CommandSetting VERIFY_VERSIONS_SETTINGS[] = {
        {OPTION_EXCLUDE, "Comma-separated list of ports to skip"},
    };

    const CommandStructure COMMAND_STRUCTURE{
        create_example_string(R"###(x-ci-verify-versions)###"),
        0,
        SIZE_MAX,
        {{VERIFY_VERSIONS_SWITCHES}, {VERIFY_VERSIONS_SETTINGS}, {}},
        nullptr,
    };

    static ExpectedS<std::string> verify_version_in_db(const VcpkgPaths& paths,
                                                       const std::map<std::string, VersionT, std::less<>> baseline,
                                                       const std::string& port_name,
                                                       const fs::path& port_path,
                                                       const fs::path& versions_file_path,
                                                       const std::string& local_git_tree,
                                                       bool verify_git_trees)
    {
        auto maybe_versions = vcpkg::get_builtin_versions(paths, port_name);
        if (!maybe_versions.has_value())
        {
            return {
                Strings::format("Error: While attempting to parse versions for port %s from file: %s\n"
                                "       Found the following error(s):\n%s",
                                port_name,
                                fs::u8string(versions_file_path),
                                maybe_versions.error()),
                expected_right_tag,
            };
        }

        const auto& versions = maybe_versions.value_or_exit(VCPKG_LINE_INFO);
        if (versions.empty())
        {
            return {
                Strings::format("Error: While reading versions for port %s from file: %s\n"
                                "       File contains no versions.",
                                port_name,
                                fs::u8string(versions_file_path)),
                expected_right_tag,
            };
        }

        if (verify_git_trees)
        {
            for (auto&& version_entry : versions)
            {
                bool version_ok = false;
                for (const std::string& control_file : {"CONTROL", "vcpkg.json"})
                {
                    auto treeish = Strings::concat(version_entry.second, ':', control_file);
                    auto maybe_file = paths.git_show(Strings::concat(treeish), paths.root / fs::u8path(".git"));
                    if (!maybe_file.has_value()) continue;

                    const auto& file = maybe_file.value_or_exit(VCPKG_LINE_INFO);
                    auto maybe_scf = Paragraphs::try_load_port_text(file, treeish, control_file == "vcpkg.json");
                    if (!maybe_scf.has_value())
                    {
                        return {
                            Strings::format("Error: While reading versions for port %s from file: %s\n"
                                            "       While validating version: %s.\n"
                                            "       While trying to load port from: %s\n"
                                            "       Found the following error(s):\n%s",
                                            port_name,
                                            fs::u8string(versions_file_path),
                                            version_entry.first.versiont,
                                            treeish,
                                            maybe_scf.error()->error),
                            expected_right_tag,
                        };
                    }

                    const auto& scf = maybe_scf.value_or_exit(VCPKG_LINE_INFO);
                    auto&& git_tree_version = scf.get()->to_schemed_version();
                    if (version_entry.first.versiont != git_tree_version.versiont)
                    {
                        return {
                            Strings::format(
                                "Error: While reading versions for port %s from file: %s\n"
                                "       While validating version: %s.\n"
                                "       The version declared in file does not match checked-out version: %s\n"
                                "       Checked out Git SHA: %s",
                                port_name,
                                fs::u8string(versions_file_path),
                                version_entry.first.versiont,
                                git_tree_version.versiont,
                                version_entry.second),
                            expected_right_tag,
                        };
                    }
                    version_ok = true;
                    break;
                }

                if (!version_ok)
                {
                    return {
                        Strings::format(
                            "Error: While reading versions for port %s from file: %s\n"
                            "       While validating version: %s.\n"
                            "       The checked-out object does not contain a CONTROL file or vcpkg.json file.\n"
                            "       Checked out Git SHA: %s",
                            port_name,
                            fs::u8string(versions_file_path),
                            version_entry.first.versiont,
                            version_entry.second),
                        expected_right_tag,
                    };
                }
            }
        }

        const auto& top_entry = versions.front();

        auto maybe_scf = Paragraphs::try_load_port(paths.get_filesystem(), port_path);
        if (!maybe_scf.has_value())
        {
            return {
                Strings::format("Error: While attempting to load local port %s.\n"
                                "       Found the following error(s):\n%s",
                                port_name,
                                maybe_scf.error()->error),
                expected_right_tag,
            };
        }

        const auto local_port_version = maybe_scf.value_or_exit(VCPKG_LINE_INFO)->to_schemed_version();

        if (top_entry.first.versiont != local_port_version.versiont)
        {
            auto versions_end = versions.end();
            auto it = std::find_if(versions.begin(), versions_end, [&](auto&& entry) {
                return entry.first.versiont == local_port_version.versiont;
            });
            if (it != versions_end)
            {
                return {
                    Strings::format("Error: While reading versions for port %s from file: %s\n"
                                    "       Local port version `%s` exists in version file but it's not the first "
                                    "entry in the \"versions\" array.",
                                    port_name,
                                    fs::u8string(versions_file_path),
                                    local_port_version.versiont),
                    expected_right_tag,
                };
            }
            else
            {
                return {
                    Strings::format("Error: While reading versions for port %s from file: %s\n"
                                    "       Version `%s` was not found in versions file.\n"
                                    "       Run:\n\n"
                                    "           vcpkg x-add-version %s\n\n"
                                    "       to add the new port version.",
                                    port_name,
                                    fs::u8string(versions_file_path),
                                    local_port_version.versiont,
                                    port_name),
                    expected_right_tag,
                };
            }
        }

        if (top_entry.first.scheme != local_port_version.scheme)
        {
            return {
                Strings::format("Error: While reading versions for port %s from file: %s\n"
                                "       File declares version `%s` with scheme: `%s`.\n"
                                "       But local port declares the same version with a different scheme: `%s`.\n"
                                "       Version must be unique even between different schemes.\n"
                                "       Run:\n\n"
                                "           vcpkg x-add-version %s --overwrite-version\n\n"
                                "       to overwrite the declared version's scheme.",
                                port_name,
                                fs::u8string(versions_file_path),
                                top_entry.first.versiont,
                                get_scheme_name(top_entry.first.scheme),
                                get_scheme_name(local_port_version.scheme),
                                port_name),
                expected_right_tag,
            };
        }

        if (local_git_tree != top_entry.second)
        {
            return {
                Strings::format("Error: While reading versions for port %s from file: %s\n"
                                "       File declares version `%s` with SHA: %s\n"
                                "       But local port with the same verion has a different SHA: %s\n"
                                "       Please update the port's version fields and then run:\n\n"
                                "           vcpkg x-add-version %s\n\n"
                                "       to add a new version.",
                                port_name,
                                fs::u8string(versions_file_path),
                                top_entry.first.versiont,
                                top_entry.second,
                                local_git_tree,
                                port_name),
                expected_right_tag,
            };
        }

        auto maybe_baseline = baseline.find(port_name);
        if (maybe_baseline == baseline.end())
        {
            return {
                Strings::format("Error: While reading baseline version for port %s.\n"
                                "       Baseline version not found.\n"
                                "       Run:\n\n"
                                "           vcpkg x-add-version %s\n\n"
                                "       to set version %s as the baseline version.",
                                port_name,
                                port_name,
                                local_port_version.versiont),
                expected_right_tag,
            };
        }

        auto&& baseline_version = maybe_baseline->second;
        if (baseline_version != top_entry.first.versiont)
        {
            return {
                Strings::format("Error: While reading baseline version for port %s.\n"
                                "       While validating latest version from file: %s\n"
                                "       Baseline file declares version: %s.\n"
                                "       But the latest version in version files is: %s.\n"
                                "       Run:\n\n"
                                "           vcpkg x-add-version %s\n\n"
                                "       to update the baseline version.",
                                port_name,
                                fs::u8string(versions_file_path),
                                baseline_version,
                                top_entry.first.versiont,
                                port_name),
                expected_right_tag,
            };
        }

        return {
            Strings::format("OK: %s\t%s -> %s\n", top_entry.second, port_name, top_entry.first.versiont),
            expected_left_tag,
        };
    }

    void perform_and_exit(const VcpkgCmdArguments&, const VcpkgPaths&)
    {
        Checks::exit_success(VCPKG_LINE_INFO);
    }

    void CIVerifyVersionsCommand::perform_and_exit(const VcpkgCmdArguments& args, const VcpkgPaths& paths) const
    {
        CIVerifyVersions::perform_and_exit(args, paths);
    }
}
