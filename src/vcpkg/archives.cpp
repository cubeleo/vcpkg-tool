#include <vcpkg/base/parse.h>
#include <vcpkg/base/system.debug.h>
#include <vcpkg/base/system.print.h>
#include <vcpkg/base/system.process.h>
#include <vcpkg/base/util.h>

#include <vcpkg/archives.h>
#include <vcpkg/commands.h>
#include <vcpkg/tools.h>

namespace
{
    using namespace vcpkg;

#if defined(_WIN32)
    void win32_extract_nupkg(const ToolCache& tools, MessageSink& status_sink, const Path& archive, const Path& to_path)
    {
        const auto nuget_exe = tools.get_tool_path(Tools::NUGET, status_sink);

        const auto stem = archive.stem();

        // assuming format of [name].[version in the form d.d.d]
        // This assumption may not always hold
        auto dot_after_name = Util::find_nth_from_last(stem, '.', 2);

        auto is_digit_or_dot = [](char ch) { return ch == '.' || ParserBase::is_ascii_digit(ch); };
        if (dot_after_name == stem.end() || !std::all_of(dot_after_name, stem.end(), is_digit_or_dot))
        {
            Checks::msg_exit_with_message(VCPKG_LINE_INFO, msgCouldNotDeduceNugetIdAndVersion, msg::path = archive);
        }

        auto nugetid = StringView{stem.begin(), dot_after_name};
        auto version = StringView{dot_after_name + 1, stem.end()};

        Command nuget_command{nuget_exe};
        nuget_command.string_arg("install")
            .string_arg(nugetid)
            .string_arg("-Version")
            .string_arg(version)
            .string_arg("-OutputDirectory")
            .string_arg(to_path)
            .string_arg("-Source")
            .string_arg(archive.parent_path())
            .string_arg("-nocache")
            .string_arg("-DirectDownload")
            .string_arg("-NonInteractive")
            .string_arg("-ForceEnglishOutput")
            .string_arg("-PackageSaveMode")
            .string_arg("nuspec");

        const auto result = flatten(cmd_execute_and_capture_output(nuget_command), Tools::NUGET);
        if (!result)
        {
            Checks::exit_with_message(
                VCPKG_LINE_INFO, "Failed to extract '%s' with message:\n%s", archive, result.error());
        }
    }

    void win32_extract_msi(const Path& archive, const Path& to_path)
    {
        // MSI installation sometimes requires a global lock and fails if another installation is concurrent. Loop
        // to enable retries.
        for (int i = 0;; ++i)
        {
            // msiexec is a WIN32/GUI application, not a console application and so needs special attention to wait
            // until it finishes (wrap in cmd /c).
            const auto maybe_code_and_output = cmd_execute_and_capture_output(
                Command{"cmd"}
                    .string_arg("/c")
                    .string_arg("msiexec")
                    // "/a" is administrative mode, which unpacks without modifying the system
                    .string_arg("/a")
                    .string_arg(archive)
                    .string_arg("/qn")
                    // msiexec requires quotes to be after "TARGETDIR=":
                    //      TARGETDIR="C:\full\path\to\dest"
                    .raw_arg(Strings::concat("TARGETDIR=", Command{to_path}.extract())),
                default_working_directory,
                default_environment,
                Encoding::Utf16);

            if (auto code_and_output = maybe_code_and_output.get())
            {
                if (code_and_output->exit_code == 0)
                {
                    // Success
                    break;
                }

                if (i < 19 && code_and_output->exit_code == 1618)
                {
                    // ERROR_INSTALL_ALREADY_RUNNING
                    msg::println(msgAnotherInstallationInProgress);
                    std::this_thread::sleep_for(std::chrono::seconds(6));
                    continue;
                }
            }

            Checks::msg_exit_with_message(VCPKG_LINE_INFO, flatten(maybe_code_and_output, "msiexec").error());
        }
    }

    void win32_extract_with_seven_zip(const Path& seven_zip, const Path& archive, const Path& to_path)
    {
        static bool recursion_limiter_sevenzip = false;
        Checks::check_exit(VCPKG_LINE_INFO, !recursion_limiter_sevenzip);
        recursion_limiter_sevenzip = true;
        const auto maybe_output =
            flatten(cmd_execute_and_capture_output(Command{seven_zip}
                                                       .string_arg("x")
                                                       .string_arg(archive)
                                                       .string_arg(Strings::format("-o%s", to_path))
                                                       .string_arg("-y")),
                    Tools::SEVEN_ZIP);

        if (!maybe_output)
        {
            Checks::exit_with_message(
                VCPKG_LINE_INFO, "7zip failed while extracting '%s' with message:\n%s", archive, maybe_output.error());
        }

        recursion_limiter_sevenzip = false;
    }
#endif // ^^^ _WIN32

    void extract_archive_to_empty(
        Filesystem& fs, const ToolCache& tools, MessageSink& status_sink, const Path& archive, const Path& to_path)
    {
        const auto ext = archive.extension();
#if defined(_WIN32)
        if (Strings::case_insensitive_ascii_equals(ext, ".nupkg"))
        {
            win32_extract_nupkg(tools, status_sink, archive, to_path);
        }
        else if (Strings::case_insensitive_ascii_equals(ext, ".msi"))
        {
            win32_extract_msi(archive, to_path);
        }
        else if (Strings::case_insensitive_ascii_equals(ext, ".zip") ||
                 Strings::case_insensitive_ascii_equals(ext, ".7z"))
        {
            extract_tar_cmake(tools.get_tool_path(Tools::CMAKE, status_sink), archive, to_path);
        }
        else if (Strings::case_insensitive_ascii_equals(ext, ".exe"))
        {
            const Path filename = archive.filename();
            const Path stem = filename.stem();
            const Path to_archive = Path(archive.parent_path()) / stem;
            win32_extract_self_extracting_7z(fs, archive, to_archive);
            extract_archive_to_empty(fs, tools, status_sink, to_archive, to_path);
        }
#else
        (void)fs;
        if (ext == ".zip")
        {
            const auto code =
                cmd_execute(Command{"unzip"}.string_arg("-qqo").string_arg(archive), WorkingDirectory{to_path})
                    .value_or_exit(VCPKG_LINE_INFO);
            Checks::check_exit(VCPKG_LINE_INFO, code == 0, "unzip failed while extracting %s", archive);
        }
#endif
        else if (ext == ".gz" || ext == ".bz2" || ext == ".tgz")
        {
            vcpkg::extract_tar(tools.get_tool_path(Tools::TAR, status_sink), archive, to_path);
        }
        else
        {
            Checks::exit_maybe_upgrade(VCPKG_LINE_INFO, "Unexpected archive extension: %s", ext);
        }
    }

    Path extract_archive_to_temp_subdirectory(
        Filesystem& fs, const ToolCache& tools, MessageSink& status_sink, const Path& archive, const Path& to_path)
    {
        Path to_path_partial = to_path + ".partial";
#if defined(_WIN32)
        to_path_partial += "." + std::to_string(GetCurrentProcessId());
#endif

        fs.remove_all(to_path_partial, VCPKG_LINE_INFO);
        fs.create_directories(to_path_partial, VCPKG_LINE_INFO);
        extract_archive_to_empty(fs, tools, status_sink, archive, to_path_partial);
        return to_path_partial;
    }
}

namespace vcpkg
{
#ifdef _WIN32
    void win32_extract_self_extracting_7z(Filesystem& fs, const Path& archive, const Path& to_path)
    {
        constexpr static const char header_7z[] = "7z\xBC\xAF\x27\x1C";

        const Path stem = archive.stem();
        const auto subext = stem.extension();
        Checks::check_exit(VCPKG_LINE_INFO,
                           Strings::case_insensitive_ascii_equals(subext, ".7z"),
                           "Unable to extract 7z archive from Installer %s. Missing '.7z.exe' extension.",
                           archive);

        auto contents = fs.read_contents(archive, VCPKG_LINE_INFO);
        const auto pos = contents.find(header_7z);
        Checks::check_exit(VCPKG_LINE_INFO,
                           pos != std::string::npos,
                           "Unable to extract 7z archive from Installer %s. Unable to find 7z header.",
                           archive);
        contents = contents.substr(pos);
        fs.write_contents(to_path, std::move(contents), VCPKG_LINE_INFO);
    }

    void win32_extract_bootstrap_zip(
        Filesystem& fs, const ToolCache& tools, MessageSink& status_sink, const Path& archive, const Path& to_path)
    {
        fs.remove_all(to_path, VCPKG_LINE_INFO);
        Path to_path_partial = to_path + ".partial." + std::to_string(GetCurrentProcessId());

        fs.remove_all(to_path_partial, VCPKG_LINE_INFO);
        fs.create_directories(to_path_partial, VCPKG_LINE_INFO);
        const auto tar_path = get_system32().value_or_exit(VCPKG_LINE_INFO) / "tar.exe";
        if (fs.exists(tar_path, IgnoreErrors{}))
        {
            // On Windows 10, tar.exe is in the box.

            // Example:
            // tar unpacks cmake unpacks 7zip unpacks git
            extract_tar(tar_path, archive, to_path_partial);
        }
        else
        {
            // On Windows <10, we attempt to use msiexec to unpack 7zip.

            // Example:
            // msiexec unpacks 7zip_msi unpacks cmake unpacks 7zip unpacks git
            win32_extract_with_seven_zip(
                tools.get_tool_path(Tools::SEVEN_ZIP_MSI, status_sink), archive, to_path_partial);
        }
        fs.rename_with_retry(to_path_partial, to_path, VCPKG_LINE_INFO);
    }
#endif

    void extract_tar(const Path& tar_tool, const Path& archive, const Path& to_path)
    {
        const auto code =
            cmd_execute(Command{tar_tool}.string_arg("xzf").string_arg(archive), WorkingDirectory{to_path});
        Checks::check_exit(VCPKG_LINE_INFO, succeeded(code), "tar failed while extracting %s", archive);
    }

    void extract_tar_cmake(const Path& cmake_tool, const Path& archive, const Path& to_path)
    {
        // Note that CMake's built in tar can extract more archive types than many system tars; e.g. 7z
        const auto code =
            cmd_execute(Command{cmake_tool}.string_arg("-E").string_arg("tar").string_arg("xzf").string_arg(archive),
                        WorkingDirectory{to_path});
        Checks::check_exit(VCPKG_LINE_INFO, succeeded(code), "CMake failed while extracting %s", archive);
    }

    void extract_archive(
        Filesystem& fs, const ToolCache& tools, MessageSink& status_sink, const Path& archive, const Path& to_path)
    {
        fs.remove_all(to_path, VCPKG_LINE_INFO);
        Path to_path_partial = extract_archive_to_temp_subdirectory(fs, tools, status_sink, archive, to_path);
        fs.rename_with_retry(to_path_partial, to_path, VCPKG_LINE_INFO);
    }

    ExpectedL<Unit> compress_directory_to_zip(
        Filesystem& fs, const ToolCache& tools, MessageSink& status_sink, const Path& source, const Path& destination)
    {
        fs.remove(destination, VCPKG_LINE_INFO);
#if defined(_WIN32)
        auto&& seven_zip_exe = tools.get_tool_path(Tools::SEVEN_ZIP, status_sink);

        return flatten(cmd_execute_and_capture_output(
                           Command{seven_zip_exe}.string_arg("a").string_arg(destination).string_arg(source / "*"),
                           default_working_directory,
                           get_clean_environment()),
                       Tools::SEVEN_ZIP);
#else
        (void)tools;
        (void)status_sink;
        return flatten(cmd_execute_and_capture_output(Command{"zip"}
                                                          .string_arg("--quiet")
                                                          .string_arg("-y")
                                                          .string_arg("-r")
                                                          .string_arg(destination)
                                                          .string_arg("*")
                                                          .string_arg("--exclude")
                                                          .string_arg(".DS_Store"),
                                                      WorkingDirectory{source}),
                       "zip");
#endif
    }

    Command decompress_zip_archive_cmd(const ToolCache& tools,
                                       MessageSink& status_sink,
                                       const Path& dst,
                                       const Path& archive_path)
    {
        Command cmd;
#if defined(_WIN32)
        auto&& seven_zip_exe = tools.get_tool_path(Tools::SEVEN_ZIP, status_sink);
        cmd.string_arg(seven_zip_exe)
            .string_arg("x")
            .string_arg(archive_path)
            .string_arg("-o" + dst.native())
            .string_arg("-y");
#else
        (void)tools;
        (void)status_sink;
        cmd.string_arg("unzip").string_arg("-qq").string_arg(archive_path).string_arg("-d" + dst.native());
#endif
        return cmd;
    }

    std::vector<ExpectedL<Unit>> decompress_in_parallel(View<Command> jobs)
    {
        auto results =
            cmd_execute_and_capture_output_parallel(jobs, default_working_directory, get_clean_environment());
#ifdef __APPLE__
        int i = 0;
        for (auto& maybe_result : results)
        {
            if (const auto result = maybe_result.get())
            {
                if (result->exit_code == 127 && result->output.empty())
                {
                    Debug::print(jobs[i].command_line(), ": pclose returned 127, try again \n");
                    maybe_result =
                        cmd_execute_and_capture_output(jobs[i], default_working_directory, get_clean_environment());
                }
            }
            ++i;
        }
#endif

        std::vector<ExpectedL<Unit>> filtered_results;
        filtered_results.reserve(jobs.size());
        for (std::size_t idx = 0; idx < jobs.size(); ++idx)
        {
            filtered_results.push_back(flatten(results[idx], jobs[idx].command_line()));
        }

        return filtered_results;
    }
}
