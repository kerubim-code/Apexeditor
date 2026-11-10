#define _CRT_SECURE_NO_WARNINGS

#include "videocfg.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <urlmon.h>

#include <cwchar>
#include <fstream>
#include <mutex>
#include <thread>

#pragma comment(lib, "urlmon.lib")

namespace apex::videocfg {

namespace {

constexpr const char* kRepo =
    "https://raw.githubusercontent.com/kerubim-code/APEX-SETTINGS/main";

constexpr const char* kSteamAppId = "1172470";

constexpr const char* kGameExes[] = {"r5apex_dx12.exe", "r5apex.exe"};

std::mutex g_mutex;
Report     g_settings;
Report     g_autoexec;

std::string UserProfile() {
    char buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableA("USERPROFILE", buffer, MAX_PATH);
    return (length > 0 && length < MAX_PATH) ? std::string(buffer, length)
                                              : std::string("C:\\\\");
}

bool PathExists(const std::string& path) {
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool FileExists(const std::string& path) {
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

void EnsureDir(const std::string& dir) {
    std::string built;
    built.reserve(dir.size());
    for (char c : dir) {
        built += c;
        if ((c == '\\' || c == '/') && built.size() > 3)
            CreateDirectoryA(built.c_str(), nullptr);
    }
    CreateDirectoryA(dir.c_str(), nullptr);
}

std::string BaseName(const std::string& rel) {
    const auto slash = rel.find_last_of('/');
    return slash == std::string::npos ? rel : rel.substr(slash + 1);
}

void Push(Report& report, const std::string& line) {
    std::lock_guard<std::mutex> lock(g_mutex);
    report.lines.push_back(line);
}

bool Download(const std::string& url, const std::string& dest) {

    const std::string tmp = dest + ".apexdl";
    DeleteFileA(tmp.c_str());

    const HRESULT hr = URLDownloadToFileA(nullptr, url.c_str(), tmp.c_str(), 0, nullptr);
    if (FAILED(hr)) {
        DeleteFileA(tmp.c_str());
        return false;
    }

    DeleteFileA(dest.c_str());
    if (!MoveFileA(tmp.c_str(), dest.c_str())) {
        DeleteFileA(tmp.c_str());
        return false;
    }
    return true;
}

struct Job {
    std::string url;
    std::string dest;
    std::string name;
};

void RunJobs(Report& report, std::vector<Job>& jobs, const std::string& skipLine) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        report.total   = static_cast<int>(jobs.size());
        report.done    = 0;
        report.ok      = true;
        report.lines.clear();
        report.finished = false;
    }

    if (!skipLine.empty())
        Push(report, skipLine);

    int failed = 0;
    for (const auto& job : jobs) {
        const bool ok = Download(job.url, job.dest);
        if (!ok) ++failed;
        Push(report, std::string(ok ? "ok     " : "failed ") + job.name);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            ++report.done;
        }
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    report.running  = false;
    report.finished = true;
    report.ok       = (failed == 0);
    report.done     = static_cast<int>(jobs.size());
}

void AddJob(std::vector<Job>& jobs, const char* rel, const std::string& dir) {
    jobs.push_back({std::string(kRepo) + "/" + rel, dir + "\\" + BaseName(rel),
                    BaseName(rel)});
}

void SettingsWorker() {
    const std::string local   = LocalDir();
    const std::string profile = ProfileDir();

    EnsureDir(SavedGamesDir());
    EnsureDir(local);
    EnsureDir(profile);

    std::vector<Job> jobs;
    AddJob(jobs, "local/videoconfig.txt", local);
    AddJob(jobs, "local/voice_volumes.dat", local);
    AddJob(jobs, "profile/steam_autocloud.vdf", profile);

    RunJobs(g_settings, jobs, std::string());
}

void AutoexecWorker() {
    const bool wantSteam = SteamFound();
    const bool gotSteam  = wantSteam && Elevated();

    if (gotSteam)
        EnsureDir(SteamCfgDir());

    std::vector<Job> jobs;
    if (gotSteam)
        AddJob(jobs, "autoexec.cfg", SteamCfgDir());

    std::string skip;
    if (!wantSteam)
        skip = "failed steam not found  -  autoexec.cfg cannot be installed";
    else if (!gotSteam)
        skip = "skip  autoexec.cfg  -  restart APEX as administrator";

    RunJobs(g_autoexec, jobs, skip);

    if (!wantSteam || !gotSteam) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_autoexec.ok = false;
    }
}

std::string RegString(HKEY key, const char* valueName) {
    char     buffer[MAX_PATH] = {};
    DWORD    size = sizeof(buffer) - 1;
    DWORD    type = 0;
    if (RegQueryValueExA(key, valueName, nullptr, &type,
                         reinterpret_cast<LPBYTE>(buffer), &size) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ))
        return std::string();
    buffer[MAX_PATH - 1] = '\0';
    return std::string(buffer);
}

std::string VdfPathValue(const std::string& line) {
    const auto keyAt = line.find("\"path\"");
    if (keyAt == std::string::npos) return std::string();

    const auto q1 = line.find('"', keyAt + 6);
    if (q1 == std::string::npos) return std::string();
    const auto q2 = line.find('"', q1 + 1);
    if (q2 == std::string::npos) return std::string();

    std::string raw = line.substr(q1 + 1, q2 - q1 - 1);
    std::string clean;
    clean.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size())
            clean += raw[++i];
        else
            clean += raw[i];
    }
    return clean;
}

std::vector<std::string> SteamRoots() {
    std::vector<std::string> roots;

    auto add = [&roots](std::string root) {
        while (!root.empty() && (root.back() == '/' || root.back() == '\\'))
            root.pop_back();
        if (root.empty()) return;

        for (char& c : root)
            if (c == '/') c = '\\';
        for (const auto& existing : roots)
            if (existing == root) return;
        roots.push_back(root);
    };

    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", 0, KEY_READ, &key) ==
        ERROR_SUCCESS) {
        add(RegString(key, "SteamPath"));
        RegCloseKey(key);
        key = nullptr;
    }
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", 0,
                      KEY_READ, &key) == ERROR_SUCCESS) {
        add(RegString(key, "InstallPath"));
        RegCloseKey(key);
        key = nullptr;
    }
    add("C:\\Program Files (x86)\\Steam");
    add("C:\\Program Files\\Steam");

    std::vector<std::string> extra;
    for (const auto& root : roots) {
        std::ifstream in(root + "\\steamapps\\libraryfolders.vdf");
        if (!in) continue;
        std::string line;
        while (std::getline(in, line)) {
            std::string path = VdfPathValue(line);
            if (!path.empty()) extra.push_back(path);
        }
    }
    for (const auto& lib : extra)
        add(lib);

    return roots;
}

std::string GameExeIn(const std::string& folder) {
    for (const char* name : kGameExes) {
        const std::string candidate = folder + "\\" + name;
        if (FileExists(candidate)) return candidate;
    }
    return std::string();
}

bool SteamRunning() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    bool running = false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"steam.exe") == 0) {
                running = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return running;
}

std::string FindGameFolder() {
    for (const auto& root : SteamRoots()) {
        const std::string candidate = root + "\\steamapps\\common\\Apex Legends";
        if (!GameExeIn(candidate).empty())
            return candidate;
    }

    const char* eaRoots[] = {"C:\\Program Files\\Electronic Arts\\Apex Legends",
                             "C:\\Program Files (x86)\\Electronic Arts\\Apex Legends",
                             "D:\\Electronic Arts\\Apex Legends"};
    for (const char* candidate : eaRoots)
        if (!GameExeIn(candidate).empty())
            return std::string(candidate);
    return std::string();
}

}

std::string SavedGamesDir() { return UserProfile() + "\\Saved Games\\Respawn\\Apex"; }
std::string LocalDir() { return SavedGamesDir() + "\\local"; }
std::string ProfileDir() { return SavedGamesDir() + "\\profile"; }

std::string SteamCfgDir() {
    const std::string folder = GameDirFound();
    if (!folder.empty()) return folder + "\\cfg";
    return "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Apex Legends\\cfg";
}

std::string GameDir() {
    const std::string found = GameDirFound();
    return found.empty() ? SteamCfgDir() : found + "\\cfg";
}

bool SteamFound() { return PathExists(SteamCfgDir()); }

std::string GameDirFound() { return FindGameFolder(); }

bool GameFound() { return !GameDirFound().empty(); }

bool LaunchGame(std::string* detail) {
    const std::string folder = GameDirFound();
    const std::string exe    = folder.empty() ? std::string() : GameExeIn(folder);

    if (SteamRunning()) {
        const std::string protocol = std::string("steam://run/") + kSteamAppId;
        const HINSTANCE   result =
            ShellExecuteA(nullptr, "open", protocol.c_str(), nullptr, nullptr,
                          SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) > 32) {
            if (detail) *detail = "launching through Steam  -  " + protocol;
            return true;
        }
    }

    if (!exe.empty()) {
        const HINSTANCE result = ShellExecuteA(nullptr, "open", exe.c_str(),
                                               nullptr, folder.c_str(),
                                               SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) > 32) {
            if (detail) *detail = "launching " + exe + "  -  " + folder;
            return true;
        }
    }

    const std::string protocol = std::string("steam://run/") + kSteamAppId;
    const HINSTANCE   result =
        ShellExecuteA(nullptr, "open", protocol.c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) > 32) {
        if (detail) *detail = "asking Steam to launch Apex Legends  -  " + protocol;
        return true;
    }

    if (detail)
        *detail = "Apex Legends was not found on this PC";
    return false;
}

bool Elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD           size = 0;
    const BOOL ok =
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok == TRUE && elevation.TokenIsElevated != 0;
}

void StartInstall() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_settings.running) return;
        g_settings          = Report{};
        g_settings.running  = true;
    }
    std::thread([] { SettingsWorker(); }).detach();
}

Report Snapshot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_settings;
}

int InstalledCount() {
    int count = 0;
    if (FileExists(LocalDir() + "\\videoconfig.txt")) ++count;
    if (FileExists(LocalDir() + "\\voice_volumes.dat")) ++count;
    if (FileExists(ProfileDir() + "\\steam_autocloud.vdf")) ++count;
    return count;
}

void StartAutoexec() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_autoexec.running) return;
        g_autoexec          = Report{};
        g_autoexec.running  = true;
    }
    std::thread([] { AutoexecWorker(); }).detach();
}

Report SnapshotAutoexec() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_autoexec;
}

bool AutoexecInstalled() {
    return FileExists(SteamCfgDir() + "\\autoexec.cfg");
}

}
