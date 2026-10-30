#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace openshao::update {
inline constexpr const char* kLatestReleaseUrl = "https://api.github.com/repos/marcoorquera/OpenShao/releases/latest";
inline constexpr const char* kReleasesPage = "https://github.com/marcoorquera/OpenShao/releases/latest";

struct Release {
    std::string version, page, package_name, package_url, checksums_url, sha256;
    uint64_t size = 0;
};
// Package names published by tools/package_release.py for this platform.
std::string PackageName(std::string_view version);
std::string ExecutableName();
// Reads a GitHub "latest release" response. The package must belong to this platform.
Release ParseLatest(std::string_view json);
// Finds the SHA-256 for one file in a checksums.txt listing.
std::string ChecksumFor(std::string_view listing, std::string_view file);
// Package files by relative path. Also accepts the top folder used by earlier Linux packages.
using Files = std::map<std::string, std::vector<uint8_t>>;
Files ReadPackage(const std::filesystem::path& archive);
// Replaces the installed files beside the executable. Running files move aside as *.old,
// which the next start removes. A failure restores every file that was already replaced.
void Install(const std::filesystem::path& folder, const Files& files);
void RemovePreviousFiles(const std::filesystem::path& folder);
// Starts the installed executable again with the current arguments.
bool Relaunch(const std::filesystem::path& folder, std::string& error);

enum class State { Idle, Checking, Current, Available, Downloading, Ready, Failed };
struct View {
    State state = State::Idle;
    Release release;
    std::string error;
    uint64_t done = 0, total = 0;
};
class Updater {
public:
    using Fetch = std::function<void(std::string_view url, const std::filesystem::path& file, uint64_t maximum,
                                     const std::function<bool(uint64_t, uint64_t)>& progress)>;
    Updater(std::filesystem::path install, std::filesystem::path cache, std::string current, Fetch fetch = {});
    ~Updater();
    View Get() const;
    void Check();
    // Downloads, verifies and installs the available release. Relaunch afterwards to use it.
    void Download();
    void Cancel();
private:
    void Start(std::function<void()> job);
    std::filesystem::path install_, cache_;
    std::string current_;
    Fetch fetch_;
    mutable std::mutex mutex_;
    View view_;
    std::atomic<bool> cancel_{false};
    std::jthread worker_;
};
}
